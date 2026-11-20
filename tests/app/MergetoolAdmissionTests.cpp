// Intake admission on the real socket and owned worker, without Git processes or a window.
#include "TestHarness.h"
#include "core/SessionFixtures.h"
#include "fixtures/Await.h"
#include "fixtures/ChoiceFile.h"
#include "mergetool/MergetoolSession.h"
#include "problems/ProblemText.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QLocalSocket>
#include <QScopeGuard>
#include <QStringList>
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

// A captured conflict at path, keyed the way a load keys it.
FileSession conflictAt(const QByteArray &path, const FileSession &choice)
{
    auto snapshots = *choice.snapshots();
    snapshots.path = path.toStdString();
    return FileSession(path.toHex().toStdString(), path.toStdString(), choice.current().draft,
                       {.snapshots = std::move(snapshots), .initializeChoices = true});
}

struct Session {
    // extra joins choices.txt in every session this owner loads.
    explicit Session(int deadline = 5000, std::vector<FileSession> extra = {})
        : others(std::move(extra)),
          session(std::make_unique<MergetoolSession>(
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
        QObject::connect(session.get(), &MergetoolSession::notice, session.get(),
                         [this](const QString &message) { notices.append(message); });
        QString error;
        CHECK(session->listen(repository, &error) == MergetoolSession::Listen::Listening);
    }
    void replace()
    {
        auto choice = test_support::choiceFile();
        std::vector<FileSession> files;
        files.emplace_back(QByteArray("choices.txt").toHex().toStdString(), choice.path(), choice.current().draft,
                           FileLaunch{.snapshots = *choice.snapshots(), .initializeChoices = true});
        files.insert(files.end(), others.begin(), others.end());
        CHECK(coordinator.replaceSession(directory.path(), "merge", std::move(files)));
        test_support::awaitRecovery(store);
    }
    AttachMessage attach(const QByteArray &path = "choices.txt")
    {
        MergetoolSources sources;
        sources.path = path;
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

    std::vector<FileSession> others;
    QTemporaryDir directory{QStringLiteral("/tmp/bt-admit-XXXXXX")};
    git::RepositoryContext repository;
    std::shared_ptr<Reader> reader = std::make_shared<Reader>();
    SessionStore store{directory.path() + "/recovery"};
    const git::GitRunners git = git::gitAdapters({});
    QThreadPool workers;
    SessionCoordinator coordinator{store, git, workers};
    ResolutionPublisher publisher{store, coordinator, git};
    std::unique_ptr<MergetoolSession> session;
    // Every notice the session gave the window, oldest first.
    QStringList notices;
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
    const auto late = QStringLiteral("The mergetool inputs could not be checked in time. Try again.");
    for (auto *peer : {&running, &queued}) peer->refused(late);
    // The window shows each refusal the launchers print.
    CHECK_EQ(owner.notices, QStringList({late, late}));
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

// The launcher prints the reason and the window shows it as a notice.
void refusedInBoth(Session &owner, const QByteArray &path, const QString &reason)
{
    Peer peer(owner, owner.attach(path));
    peer.refused(reason);
    owner.drained();
    CHECK(!owner.notices.isEmpty());
    CHECK_EQ(owner.notices.last(), reason);
}

// Files the window holds back from editing, beside choices.txt.
std::vector<FileSession> heldBackFiles()
{
    std::vector<FileSession> files;
    files.emplace_back(QByteArray("image.png").toHex().toStdString(), "image.png", Draft{},
                       FileLaunch{.support = Support::OutsideApp, .supportDetail = "binary"});
    files.emplace_back(QByteArray("broken.txt").toHex().toStdString(), "broken.txt", Draft{"crlf\r\n", {}, 0});
    return files;
}

void aPathOutsideTheSessionIsRefusedByName()
{
    Session owner;
    refusedInBoth(owner, "missing.txt",
                  QStringLiteral("missing.txt is not a conflict in the open Branch Truce session. Reload the session "
                                 "or open the repository that git mergetool runs in."));
    CHECK(!owner.session->parentSessionActive() && !owner.session->hasWaiter("missing.txt"));
}

void aFileOutsideTheAppIsRefusedByName()
{
    Session owner(5000, heldBackFiles());
    CHECK(owner.store.file(QByteArray("image.png").toHex().toStdString())->support() == Support::OutsideApp);
    refusedInBoth(owner, "image.png",
                  QStringLiteral("Branch Truce cannot edit image.png. Resolve it with another tool."));
    CHECK(!owner.session->parentSessionActive() && !owner.session->hasWaiter("image.png"));
}

void anUnreadableFileIsRefusedByName()
{
    Session owner(5000, heldBackFiles());
    CHECK(owner.store.file(QByteArray("broken.txt").toHex().toStdString())->support() == Support::Unreadable);
    refusedInBoth(owner, "broken.txt",
                  QStringLiteral("Branch Truce could not read broken.txt. Select it in this window to see why."));
    CHECK(!owner.session->parentSessionActive() && !owner.session->hasWaiter("broken.txt"));
}

void aSecondLauncherForAWaitingPathIsRefusedByName()
{
    Session owner;
    Peer first(owner, owner.attach());
    CHECK(std::holds_alternative<AttachedMessage>(first.next()));
    owner.drained();
    refusedInBoth(owner, "choices.txt",
                  QStringLiteral("Git is already waiting on choices.txt. Finish or leave that git mergetool run "
                                 "first."));
    CHECK(owner.session->waitingOn("choices.txt")); // The first launcher keeps its wait.
}

// A conflict over the large-file line limit at path. Distinct lines keep the comparison within its
// budgets, so a load makes it editable.
FileSession largeConflictAt(const QByteArray &path)
{
    std::string left;
    for (std::size_t line = 1; line <= kLargeFileLines + 1; ++line) left += "line " + std::to_string(line) + '\n';
    const auto right = "first\n" + left.substr(left.find('\n') + 1);
    return conflictAt(path, test_support::choiceFile(left, right));
}

void aLargeFileAttachWaitsUntilTheFileLoads()
{
    const QByteArray path = "large.txt";
    const auto id = path.toHex().toStdString();
    Session owner(5000, {largeConflictAt(path)});
    CHECK(owner.store.file(id)->support() == Support::LargeFile);
    Peer peer(owner, owner.attach(path));
    CHECK(std::holds_alternative<AttachedMessage>(peer.next()));
    owner.drained();
    // Git waits, but the held file cannot take the parent staging yet.
    CHECK(owner.session->parentSessionActive() && owner.session->hasWaiter(path));
    CHECK(owner.session->blocks(path) && !owner.session->waitingOn(path) && owner.session->waitingPath().isEmpty());
    CHECK(!owner.notices.isEmpty());
    CHECK_EQ(owner.notices.last(),
             QStringLiteral("Git is waiting on large.txt. Load the large file to resolve it, or leave it unresolved."));
    CHECK(test_support::await(owner.store.loadLargeFile(id)) == Transition::Applied);
    CHECK(owner.store.file(id)->support() == Support::Supported);
    CHECK_EQ(owner.session->waitingPath(), path);
    CHECK(owner.session->waitingOn(path) && !owner.session->blocks(path));
}

void leavingAnUnassignedLargeFileTellsOnlyItsLauncher()
{
    const QByteArray path = "large.txt";
    Session owner(5000, {largeConflictAt(path)});
    Peer assigned(owner, owner.attach());
    CHECK(std::holds_alternative<AttachedMessage>(assigned.next()));
    owner.drained();
    Peer held(owner, owner.attach(path));
    CHECK(std::holds_alternative<AttachedMessage>(held.next()));
    owner.drained();
    CHECK(owner.session->waitingOn("choices.txt") && owner.session->hasWaiter(path) && !owner.session->waitingOn(path));
    CHECK(owner.session->leave(path));
    CHECK(std::holds_alternative<LeaveMessage>(held.next()));
    held.closed();
    CHECK(!owner.session->hasWaiter(path));
    // The other launcher keeps its wait and gets nothing.
    CHECK(owner.session->waitingOn("choices.txt") && owner.session->parentSessionActive());
    CHECK(assigned.received.isEmpty() && assigned.socket.state() == QLocalSocket::ConnectedState);
}

void aLargeFileThatLoadsHeldBackKeepsGitWaiting()
{
    // Its comparison metadata is over budget, so the load holds it outside the app.
    const QByteArray path = "dense.txt";
    const auto id = path.toHex().toStdString();
    Session owner(5000,
                  {conflictAt(path, test_support::choiceFile(std::string(8200, '\n'), test_support::lines(8200)))});
    CHECK(owner.store.file(id)->support() == Support::LargeFile);
    Peer peer(owner, owner.attach(path));
    CHECK(std::holds_alternative<AttachedMessage>(peer.next()));
    owner.drained();
    const auto notices = owner.notices.size();
    int changes = 0;
    QObject observer;
    QObject::connect(owner.session.get(), &MergetoolSession::changed, &observer, [&] { ++changes; });
    CHECK(test_support::await(owner.store.loadLargeFile(id)) == Transition::Applied);
    CHECK(owner.store.file(id)->support() == Support::OutsideApp);
    CHECK(owner.session->hasWaiter(path) && owner.session->blocks(path) && owner.session->waitingPath().isEmpty());
    CHECK_EQ(owner.notices.size(), notices);
    CHECK_EQ(changes, 0);
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
    suite.run("a path outside the session is refused by name", aPathOutsideTheSessionIsRefusedByName);
    suite.run("a file outside the app is refused by name", aFileOutsideTheAppIsRefusedByName);
    suite.run("an unreadable file is refused by name", anUnreadableFileIsRefusedByName);
    suite.run("a second launcher for a waiting path is refused by name", aSecondLauncherForAWaitingPathIsRefusedByName);
    suite.run("a large file attach waits until the file loads", aLargeFileAttachWaitsUntilTheFileLoads);
    suite.run("leaving an unassigned large file tells only its launcher",
              leavingAnUnassignedLargeFileTellsOnlyItsLauncher);
    suite.run("a large file that loads held back keeps Git waiting", aLargeFileThatLoadsHeldBackKeepsGitWaiting);
    return suite.report();
}
