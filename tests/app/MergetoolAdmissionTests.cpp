// Intake admission on the real socket and owned worker, without Git processes or a window.
#include "TestHarness.h"
#include "fixtures/Await.h"
#include "fixtures/ChoiceFile.h"
#include "mergetool/MergetoolSession.h"
#include "problems/ProblemText.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QLocalSocket>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUuid>
#include <atomic>
#include <semaphore>
#include <stdexcept>

using namespace branch_truce;
using test_support::waitUntil;

namespace {
struct Reader {
    bool read(const QByteArray &path, QByteArray *bytes, Problem *error)
    {
        if (path.endsWith("/manifest.json")) {
            ++manifests;
            if (hold.exchange(false)) {
                entered = true;
                gate.acquire();
            }
        }
        if (throwAt.load() == (path.endsWith("/manifest.json") ? 1 : 2)) throw std::runtime_error("read failed");
        return readMergetoolFile(path, bytes, error);
    }
    std::atomic<int> manifests = 0;
    std::atomic<int> throwAt = 0;
    std::atomic_bool hold = false;
    std::atomic_bool entered = false;
    std::binary_semaphore gate{0};
};

struct Session {
    explicit Session(int deadline = 5000)
        : session(std::make_unique<MergetoolSession>(
              publisher, store, coordinator, [this] { return repository; },
              MergetoolSession::Config{
                  .socketDirectory = directory.path(),
                  .gitEnvironment = {},
                  .git = git,
                  .limits = {.attachDeadlineMs = deadline, .maxConnections = 2},
                  .readIntakeFile = [read = reader](const QByteArray &path, QByteArray *bytes, Problem *error) {
                      return read->read(path, bytes, error);
                  }}))
    {
        CHECK(directory.isValid());
        repository.supported = true;
        repository.operation = git::RepositoryOperation::Merge;
        repository.worktreeRoot = directory.path();
        repository.stage2.commitId = QByteArray(40, 'a');
        repository.stage3.commitId = QByteArray(40, 'b');
        replace();
        QString error;
        CHECK(session->listen(repository, &error) == MergetoolSession::Listen::Listening);
    }
    void replace()
    {
        auto choice = test_support::choiceFile();
        std::vector<FileSession> files;
        files.emplace_back(QByteArray("choices.txt").toHex().toStdString(), choice.path(), choice.current().draft,
                           FileLaunch{.snapshots = *choice.snapshots(), .initializeChoices = true});
        CHECK(coordinator.replaceSession(directory.path(), "merge", std::move(files)));
        test_support::awaitRecovery(store);
    }
    AttachMessage attach()
    {
        MergetoolSources sources;
        sources.path = "choices.txt";
        sources.merged = "merged\n";
        sources.local = "local\n";
        sources.localPresent = true;
        const auto invocation = QUuid::createUuid().toString(QUuid::WithoutBraces);
        Problem error;
        const auto intake = writeMergetoolIntake(mergetoolIntakeRoot(directory.path()), mergetoolSessionKey(repository),
                                                 invocation, sources, &error);
        CHECK(!intake.isEmpty() && error.isEmpty());
        return {invocation, intake, {}};
    }
    void drained()
    {
        waitUntil([&] { return session->pendingIntakes() == 0; }, "owned intake work did not settle");
    }

    QTemporaryDir directory{QStringLiteral("/tmp/bt-admit-XXXXXX")};
    git::RepositoryContext repository;
    std::shared_ptr<Reader> reader = std::make_shared<Reader>();
    SessionStore store{directory.path() + "/recovery"};
    const git::GitRunners git = git::gitAdapters({});
    QThreadPool workers;
    SessionCoordinator coordinator{store, git, workers};
    ResolutionPublisher publisher{store, coordinator, git};
    std::unique_ptr<MergetoolSession> session;
};

struct Peer {
    Peer(Session &owner, const AttachMessage &attach) : invocation(attach.invocation)
    {
        QObject::connect(&socket, &QLocalSocket::readyRead, &socket, [this] { received += socket.readAll(); });
        socket.connectToServer(mergetoolServerName(owner.directory.path(), owner.repository));
        waitUntil([&] { return socket.state() == QLocalSocket::ConnectedState; }, "peer did not connect");
        const auto frame = mergetoolFrame(encodeMergetoolMessage(attach));
        CHECK_EQ(socket.write(frame), frame.size());
        socket.flush();
    }
    MergetoolMessage next()
    {
        QByteArray payload;
        waitUntil([&] { return takeMergetoolFrame(&received, &payload) == MergetoolFrame::Ready; },
                  "peer got no reply");
        const auto decoded = decodeMergetoolMessage(payload);
        CHECK(decoded.message.has_value());
        if (!decoded.message) throw test_support::Failure("the peer reply has no decoded message");
        return *decoded.message;
    }
    void closed()
    {
        waitUntil([&] { return socket.state() == QLocalSocket::UnconnectedState; }, "peer did not close");
    }
    void refused(const QString &reason)
    {
        const auto reply = next();
        CHECK(std::holds_alternative<RejectedMessage>(reply));
        CHECK_EQ(std::get<RejectedMessage>(reply).invocation, invocation);
        CHECK_EQ(std::get<RejectedMessage>(reply).reason, reason);
        closed();
    }
    QLocalSocket socket;
    QString invocation;
    QByteArray received;
};

void disconnectPressureKeepsAdmissionUntilCompletion()
{
    Session owner;
    owner.reader->hold = true;
    auto release = qScopeGuard([&] { owner.reader->gate.release(); });
    Peer first(owner, owner.attach());
    waitUntil([&] { return owner.reader->entered.load(); }, "first read never entered");
    Peer queued(owner, owner.attach());
    waitUntil([&] { return owner.session->pendingIntakes() == 2; }, "second intake never queued");
    int disconnected = 0;
    QObject observer;
    auto *server = owner.session->findChild<MergetoolServer *>();
    CHECK(server);
    QObject::connect(server, &MergetoolServer::closed, &observer, [&] { ++disconnected; });
    first.socket.abort();
    queued.socket.abort();
    waitUntil([&] { return disconnected == 2; }, "server did not release the disconnected socket slots");
    CHECK_EQ(owner.session->pendingIntakes(), 2);
    for (int attempt = 0; attempt < 12; ++attempt) {
        Peer excess(owner, owner.attach());
        excess.refused(QStringLiteral("Too many mergetool inputs are being checked. Try again when a check finishes."));
        CHECK_EQ(owner.session->pendingIntakes(), 2);
        CHECK_EQ(owner.reader->manifests.load(), 1);
    }
    owner.reader->gate.release();
    release.dismiss();
    owner.drained();
    CHECK_EQ(owner.reader->manifests.load(), 1); // The disconnected queued job skipped I/O.
    CHECK(!owner.session->parentSessionActive() && !owner.session->hasWaiter("choices.txt"));
    const auto attach = owner.attach();
    Peer recovered(owner, attach);
    const auto reply = recovered.next();
    CHECK(std::holds_alternative<AttachedMessage>(reply));
    CHECK_EQ(std::get<AttachedMessage>(reply).invocation, attach.invocation);
    owner.drained();
    CHECK_EQ(owner.reader->manifests.load(), 2);
    CHECK(owner.session->waitingOn("choices.txt"));
    CHECK_EQ(owner.session->intake("choices.txt"), attach.intake);
    CHECK_EQ(owner.session->waitingMergedSha256("choices.txt"),
             QCryptographicHash::hash("merged\n", QCryptographicHash::Sha256).toHex());
    CHECK(owner.session->leave("choices.txt"));
    CHECK(std::holds_alternative<LeaveMessage>(recovered.next()));
}

void deadlinesCoverRunningAndQueuedWork()
{
    Session owner(200);
    owner.reader->hold = true;
    auto release = qScopeGuard([&] { owner.reader->gate.release(); });
    Peer running(owner, owner.attach());
    waitUntil([&] { return owner.reader->entered.load(); }, "first read never entered");
    Peer queued(owner, owner.attach());
    waitUntil([&] { return owner.session->pendingIntakes() == 2; }, "second intake never queued");
    for (auto *peer : {&running, &queued})
        peer->refused(QStringLiteral("The mergetool inputs could not be checked in time. Try again."));
    CHECK_EQ(owner.session->pendingIntakes(), 2);
    CHECK(!owner.session->parentSessionActive());
    Peer excess(owner, owner.attach());
    excess.refused(QStringLiteral("Too many mergetool inputs are being checked. Try again when a check finishes."));
    owner.reader->gate.release();
    release.dismiss();
    owner.drained();
    CHECK_EQ(owner.reader->manifests.load(), 1);
    CHECK(!owner.session->hasWaiter("choices.txt"));
    CHECK(running.received.isEmpty() && queued.received.isEmpty()); // No late Attached frame.
}

void readExceptionsRefuseAndReleaseAdmission()
{
    Session owner;
    // Both real read paths use the seam: the manifest, then a source hashed against it.
    for (int at : {1, 2}) {
        owner.reader->throwAt = at;
        Peer peer(owner, owner.attach());
        peer.refused(problem_text::text(ProblemCode::MergetoolIntakeCheckFailed));
        owner.drained();
        CHECK(!owner.session->parentSessionActive());
    }
    owner.reader->throwAt = 0;
    Peer peer(owner, owner.attach());
    CHECK(std::holds_alternative<AttachedMessage>(peer.next()));
    owner.drained();
    CHECK(owner.session->hasWaiter("choices.txt"));
}

void replacingTheSessionFencesTheResult()
{
    Session owner;
    owner.reader->hold = true;
    auto release = qScopeGuard([&] { owner.reader->gate.release(); });
    Peer peer(owner, owner.attach());
    waitUntil([&] { return owner.reader->entered.load(); }, "read never entered");
    owner.replace();
    owner.reader->gate.release();
    release.dismiss();
    peer.refused(QStringLiteral("The open session changed while checking the mergetool inputs. Try again."));
    owner.drained();
    CHECK(!owner.session->parentSessionActive() && !owner.session->hasWaiter("choices.txt"));
}

void destroyingTheOwnerDoesNotWaitForTheRead()
{
    Session owner;
    owner.reader->hold = true;
    auto release = qScopeGuard([&] { owner.reader->gate.release(); });
    Peer peer(owner, owner.attach());
    waitUntil([&] { return owner.reader->entered.load(); }, "read never entered");
    owner.session.reset(); // Must return while the read is still held.
    peer.closed();
    owner.reader->gate.release();
    release.dismiss();
    std::weak_ptr<Reader> work = owner.reader;
    owner.reader.reset();
    waitUntil([&] { return work.expired(); }, "value-only worker did not release its read state");
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    test_support::configure(argc, argv);
    test_support::Suite suite("MergetoolAdmissionTests");
    suite.run("disconnect pressure retains admission until work settles",
              disconnectPressureKeepsAdmissionUntilCompletion);
    suite.run("deadlines cover running and queued work", deadlinesCoverRunningAndQueuedWork);
    suite.run("read exceptions refuse and release admission", readExceptionsRefuseAndReleaseAdmission);
    suite.run("session replacement fences intake completion", replacingTheSessionFencesTheResult);
    suite.run("owner destruction drains without waiting for a read", destroyingTheOwnerDoesNotWaitForTheRead);
    return suite.report();
}
