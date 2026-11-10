// Mark resolved and the other result commands through the application on a real merge: what each
// writes to the repository and the index, what it refuses, and what survives a restart.
#include "TestHarness.h"
#include "app/AppPaths.h"
#include "app/Application.h"
#include "app/EngineText.h"
#include "app/Translations.h"
#include "fixtures/AppSession.h"
#include "fixtures/ConflictedRepository.h"
#include "fixtures/GitOnPath.h"
#include "fixtures/ScopedEnvironment.h"
#include "fixtures/ScriptedGit.h"
#include "fixtures/SyncFaults.h"
#include "mergetool/MergetoolProtocol.h"
#include "recovery/RecoveryRestore.h"
#include <QCoreApplication>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <map>
#include <memory>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>

using namespace branch_truce;

namespace publish_stall {
std::atomic<bool> arm{false};
std::atomic<int> readyWrite{-1};
std::atomic<int> releaseRead{-1};
}

namespace {
// The file system a session's recovery store and publisher use: SyncFaults' journal faults, and
// an exchange that PublishStall can hold after it lands.
class PublishFiles final : public test_support::SyncFaults {
public:
    int exchange(int fromDirectory, const char *from, int toDirectory, const char *to) const override
    {
        const int result = FileSystem::exchange(fromDirectory, from, toDirectory, to);
        if (result != 0 || !publish_stall::arm.exchange(false)) return result;
        const int ready = publish_stall::readyWrite.load();
        if (ready >= 0) {
            const char token = 1;
            ssize_t sent = 0;
            do {
                sent = ::send(ready, &token, 1, MSG_NOSIGNAL);
            } while (sent < 0 && errno == EINTR);
        }
        const int release = publish_stall::releaseRead.load();
        if (release >= 0) {
            char ack = 0;
            ssize_t received = 0;
            do {
                received = ::recv(release, &ack, 1, 0);
            } while (received < 0 && errno == EINTR);
        }
        return result;
    }
};
}

namespace {
using test_support::readFile;
using test_support::ScopedEnvironment;
using test_support::writeFile;

// A merge stopped with tracked.txt and excluded.txt in conflict and notes.txt staged, tracked.txt
// holding the user's manual merge.
class Repository : public test_support::ConflictedRepository {
public:
    Repository()
    {
        commit({{"tracked.txt", "base\n"}, {"excluded.txt", "base\n"}, {"notes.txt", "notes stay staged\n"}}, "base");
        conflict({{"tracked.txt", "main\n"}, {"excluded.txt", "main\n"}},
                 {{"tracked.txt", "side\n"}, {"excluded.txt", "side\n"}});
        writeFile(path("tracked.txt"), manual);
        notesObject = git({"rev-parse", ":notes.txt"}).trimmed();
    }
    QByteArray notesObject;
    const QByteArray manual = "resolved without a prior save\n";
};

void openController(Application &controller, const Repository &repo)
{
    const auto before = repo.snapshot();
    test_support::openRepository(controller, repo.root);
    if (controller.loader()->startupState() != "ready")
        throw std::runtime_error("controller startup: " + controller.loader()->detail().toStdString());
    CHECK(repo.snapshot() == before);
}

const FileSession &openedFile(Application &controller, const char *path = "tracked.txt")
{
    return test_support::loadedFile(controller, path);
}

QString selectTracked(Application &controller)
{
    const auto id = QString::fromStdString(test_support::fileIdOf("tracked.txt"));
    CHECK(controller.selection()->select(id));
    return id;
}

QString revisionOf(const FileSession &file) { return QString::number(static_cast<qulonglong>(file.current().id)); }

void waitFor(Application &controller, const std::function<bool()> &ready)
{
    test_support::waitUntil(ready, [&] {
        return (controller.feedback()->message() + " | " + controller.selectedFile()->status()
                + " | busy=" + QString::number(controller.commands()->resolutionBusy()))
            .toStdString();
    });
}

QSet<QByteArray> unmerged(const Repository &repo) { return repo.unmerged(); }

void expectUnchanged(const Repository &repo, const QMap<QString, QByteArray> &before, bool ignoreObjects = false)
{
    auto changed = Repository::changed(before, repo.snapshot());
    if (ignoreObjects)
        changed.removeIf([](const QString &path) {
            return path.startsWith(QStringLiteral(".git/objects/"))
                || path.startsWith(QStringLiteral("missing .git/objects/"));
        });
    if (!changed.isEmpty()) throw std::runtime_error(changed.join(", ").toStdString());
}

struct Session {
    Repository repo;
    ScopedEnvironment environment;
    std::shared_ptr<PublishFiles> files = std::make_shared<PublishFiles>();
    Application controller{Application::Config{.gitExecutable = gitOnPath(),
                                               .recoveryDirectory = newRecoveryDirectory(),
                                               .correspondence = {},
                                               .files = files}};
    QString id;
    explicit Session(bool unsupportedResult = false) : environment(repo.environment)
    {
        if (unsupportedResult) {
            // Keep the source stages as text, but make the captured launch result binary.
            const QByteArray bytes("unsupported\0data\n", 17);
            writeFile(repo.path("excluded.txt"), bytes);
            CHECK(readFile(repo.path("excluded.txt")) == bytes && bytes.size() == 17);
            CHECK(repo.git({"check-attr", "text", "eol", "ident", "working-tree-encoding", "--", "excluded.txt"})
                  == "excluded.txt: text: unspecified\nexcluded.txt: eol: unspecified\n"
                     "excluded.txt: ident: unspecified\nexcluded.txt: working-tree-encoding: unspecified\n");
        }
        openController(controller, repo);
        id = selectTracked(controller);
        CHECK(openedFile(controller).facts().unsaved);
        CHECK(openedFile(controller).facts().undecided > 0);
        CHECK(readFile(repo.path("tracked.txt")) == repo.manual);
        CHECK(unmerged(repo).contains("tracked.txt") && unmerged(repo).contains("excluded.txt"));
    }
    QString revision() { return revisionOf(openedFile(controller)); }
};

class PublishStall {
public:
    PublishStall()
    {
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, ready) == 0);
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, release) == 0);
        publish_stall::readyWrite.store(ready[1]);
        publish_stall::releaseRead.store(release[0]);
        publish_stall::arm.store(true);
    }
    ~PublishStall()
    {
        publish_stall::arm.store(false);
        if (release[1] >= 0) {
            ::shutdown(release[1], SHUT_WR);
            ::close(release[1]);
            release[1] = -1;
        }
        publish_stall::readyWrite.store(-1);
        publish_stall::releaseRead.store(-1);
        for (const int fd : {ready[0], ready[1], release[0]})
            if (fd >= 0) ::close(fd);
    }
    PublishStall(const PublishStall &) = delete;
    PublishStall &operator=(const PublishStall &) = delete;
    // Runs the event loop until the worker signals that its exchange landed.
    bool waitForExchange()
    {
        test_support::waitUntil(
            [this] {
                pollfd input{};
                input.fd = ready[0];
                input.events = POLLIN;
                return ::poll(&input, 1, 0) > 0 && (input.revents & POLLIN) != 0;
            },
            "the publisher did not reach its exchange");
        return true;
    }
    void releaseWorker()
    {
        const char ack = 1;
        ssize_t sent = 0;
        do {
            sent = ::send(release[1], &ack, 1, MSG_NOSIGNAL);
        } while (sent < 0 && errno == EINTR);
        CHECK(sent == 1);
    }

private:
    int ready[2] = {-1, -1};
    int release[2] = {-1, -1};
};

void publishWhileDraftChanges(Application &controller, const QString &id, const std::string &nextText)
{
    PublishStall stall;
    const auto launched = revisionOf(openedFile(controller));
    CHECK(controller.commands()->markResolved(id, launched, true));
    CHECK(stall.waitForExchange());
    auto draft = openedFile(controller).current().draft;
    draft.text = nextText;
    CHECK(controller.store()->replaceDraft(id.toStdString(), openedFile(controller).current().id, draft)
          == Transition::Applied);
    CHECK(revisionOf(openedFile(controller)) != launched);
    stall.releaseWorker();
    waitFor(controller, [&] { return !controller.commands()->resolutionBusy(); });
}

struct RestartedWrite {
    Repository repo;
    ScopedEnvironment environment;
    std::unique_ptr<Application> controller;
    QString id;
    QByteArray edited;
    RestartedWrite()
        : environment(repo.environment),
          id(QString::fromLatin1(QByteArray("tracked.txt").toHex())),
          edited("edited in app\n")
    {
        const auto hook = repo.path(".git/hooks/post-index-change");
        {
            Application first{Application::Config{gitOnPath(), newRecoveryDirectory(), {}}};
            openController(first, repo);
            CHECK(first.selection()->select(id));
            auto draft = openedFile(first).current().draft;
            draft.text = edited.toStdString();
            draft.unresolvedMarkers = 0;
            CHECK(first.store()->replaceDraft(id.toStdString(), openedFile(first).current().id, draft)
                  == Transition::Applied);
            writeFile(hook, "#!/bin/sh\nexit 1\n");
            CHECK(
                QFile::setPermissions(hook, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
            CHECK(first.commands()->markResolved(id, revisionOf(openedFile(first)), true));
            waitFor(first, [&] {
                const auto &file = openedFile(first);
                return !first.commands()->resolutionBusy() && file.publication().writeOutcome()
                    && file.publication().writeOutcome()->outcome == Outcome::Succeeded
                    && file.publication().stageOutcome();
            });
            CHECK(openedFile(first).publication().stageOutcome()->outcome != Outcome::Succeeded);
            CHECK(!openedFile(first).facts().resolved);
            CHECK(readFile(repo.path("tracked.txt")) == edited);
        }
        CHECK(QFile::remove(hook));
        controller = std::make_unique<Application>(Application::Config{gitOnPath(), newRecoveryDirectory(), {}});
        openController(*controller, repo);
        CHECK(controller->selection()->select(id));
        const auto &restored = openedFile(*controller);
        CHECK(restored.publication().written() && restored.publication().writeOutcome()
              && restored.publication().writeOutcome()->outcome == Outcome::Succeeded);
        CHECK(!restored.facts().resolved);
        CHECK(readFile(repo.path("tracked.txt")) == edited);
    }
};

// Every sessionEnded emission of one controller.
struct SessionEnd {
    int count = 0;
    QString outcome;
    bool clean = false;
};

void watchSessionEnd(Application &controller, SessionEnd &end)
{
    QObject::connect(controller.completion(), &SessionCompletion::sessionEnded, &controller,
                     [&end](const QString &outcome, bool clean) {
                         ++end.count;
                         end.outcome = outcome;
                         end.clean = clean;
                     });
}

QVariantMap handoffEntry(const QVariantMap &summary, const QString &fileId)
{
    for (const auto &group : summary.value("groups").toList())
        for (const auto &entry : group.toMap().value("entries").toList())
            if (entry.toMap().value("fileId").toString() == fileId) return entry.toMap();
    throw std::runtime_error("handoff has no entry for " + fileId.toStdString());
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    // The English catalog supplies the plural forms these checks read.
    branch_truce::installTranslations(application);
    QCoreApplication::setOrganizationName(QStringLiteral("branch-truce"));
    QCoreApplication::setApplicationName(QStringLiteral("branch-truce-resolution-tests"));
    test_support::Suite suite("MarkResolvedTests");
    suite.run("controller fixtures contain and remove their session sockets and restore the runtime environment", [] {
        const auto previous = qgetenv("XDG_RUNTIME_DIR");
        const auto wasSet = qEnvironmentVariableIsSet("XDG_RUNTIME_DIR");
        QString runtime;
        {
            Session session;
            runtime = session.repo.environment.value(QStringLiteral("XDG_RUNTIME_DIR"));
            CHECK(QDir::isAbsolutePath(runtime));
            CHECK(qEnvironmentVariable("XDG_RUNTIME_DIR") == runtime);
            CHECK(mergetoolSocketDirectory(qgetenv("XDG_RUNTIME_DIR")) == runtime + QStringLiteral("/branch-truce"));
            CHECK(QFileInfo::exists(mergetoolSocketDirectory(qgetenv("XDG_RUNTIME_DIR"))));
        }
        CHECK(qEnvironmentVariableIsSet("XDG_RUNTIME_DIR") == wasSet);
        CHECK(qgetenv("XDG_RUNTIME_DIR") == previous);
        CHECK(!QFileInfo::exists(runtime));
    });
    suite.run("confirmation, cancellation and a stale revision leave the repository unpublished", [] {
        Session session;
        const auto before = session.repo.snapshot();
        const auto text = openedFile(session.controller).current().draft.text;
        CHECK(!session.controller.commands()->markResolved(session.id, session.revision(), false));
        CHECK(session.controller.commands()->resolutionConfirmation());
        CHECK(session.controller.feedback()->message().contains(QStringLiteral("intentional")));
        CHECK(!openedFile(session.controller).publication().reviewed());
        expectUnchanged(session.repo, before);
        session.controller.commands()->cancelResolution();
        CHECK(!session.controller.commands()->resolutionConfirmation());
        CHECK(openedFile(session.controller).current().draft.text == text);
        const auto stale = session.revision();
        auto edited = openedFile(session.controller).current().draft;
        edited.text += "later edit\n";
        CHECK(session.controller.store()->replaceDraft(session.id.toStdString(),
                                                       openedFile(session.controller).current().id, edited)
              == Transition::Applied);
        CHECK(!session.controller.commands()->markResolved(session.id, stale, true));
        CHECK(session.controller.feedback()->message().contains(QStringLiteral("changed")));
        CHECK(readFile(session.repo.path("tracked.txt")) == session.repo.manual);
        expectUnchanged(session.repo, before);
        CHECK(!openedFile(session.controller).facts().resolved);
    });
    suite.run("a failed review persist publishes nothing", [] {
        Session session;
        const auto before = session.repo.snapshot();
        test_support::ArmedFault fault(*session.files, test_support::SyncFaults::Point::JournalSync);
        CHECK(!session.controller.commands()->markResolved(session.id, session.revision(), true));
        CHECK(session.files->failures == 1);
        CHECK(session.controller.files()->hasPendingRecovery());
        CHECK(!openedFile(session.controller).publication().reviewed());
        expectUnchanged(session.repo, before);
        CHECK(unmerged(session.repo).contains("tracked.txt"));
    });
    suite.run("a rejected repository write keeps the draft and the conflict", [] {
        Session session;
        CHECK(QFile::setPermissions(session.repo.path("tracked.txt"), QFileDevice::ReadOwner));
        const auto before = session.repo.snapshot();
        CHECK(session.controller.commands()->markResolved(session.id, session.revision(), true));
        waitFor(session.controller, [&] {
            return !session.controller.commands()->resolutionBusy()
                && openedFile(session.controller).publication().writeOutcome()
                && openedFile(session.controller).publication().writeOutcome()->outcome != Outcome::Succeeded;
        });
        // The publisher sees that the destination's permissions changed since the load and refuses
        // before it writes, so even root, which could write the file, gets a definite failure.
        CHECK(openedFile(session.controller).publication().writeOutcome()->outcome == Outcome::Failed);
        CHECK(openedFile(session.controller).publication().reviewed()
              && openedFile(session.controller).publication().checkpoint());
        CHECK(!openedFile(session.controller).facts().resolved
              && !openedFile(session.controller).publication().staged());
        CHECK(readFile(session.repo.path("tracked.txt")) == session.repo.manual);
        expectUnchanged(session.repo, before, true);
        CHECK(unmerged(session.repo).contains("tracked.txt") && unmerged(session.repo).contains("excluded.txt"));
        CHECK(session.repo.git({"rev-parse", ":notes.txt"}).trimmed() == session.repo.notesObject);
    });
    suite.run("staging that Git refuses leaves the written file unresolved", [] {
        Repository repo;
        ScopedEnvironment environment(repo.environment);
        // git add exits non-zero without touching the index.
        const auto refused = [](const git::Request &) {
            git::Result result;
            result.outcome = git::Outcome::ExitFailure;
            result.mutation = git::Mutation::ObservedUnchanged;
            result.exitCode = 128;
            result.diagnostic = QStringLiteral("Git refused to stage the file.");
            return result;
        };
        Application controller{Application::Config{
            .gitExecutable = gitOnPath(),
            .recoveryDirectory = newRecoveryDirectory(),
            .correspondence = {},
            .sessionGit = test_support::ScriptedGitRunner::runners(git::Operation::StageFile, refused, gitOnPath())}};
        openController(controller, repo);
        const auto id = selectTracked(controller);
        const auto before = repo.snapshot();
        CHECK(controller.commands()->markResolved(id, revisionOf(openedFile(controller)), true));
        waitFor(controller, [&] { return !controller.commands()->resolutionBusy(); });
        const auto &file = openedFile(controller);
        CHECK(file.publication().writeOutcome() && file.publication().writeOutcome()->outcome == Outcome::Succeeded);
        CHECK(file.publication().stageOutcome() && file.publication().stageOutcome()->outcome == Outcome::Failed);
        CHECK_EQ(QString::fromStdString(file.publication().stageOutcome()->detail),
                 QStringLiteral("Git refused to stage the file."));
        CHECK(file.facts().status == FileStatus::StageFailed && !file.facts().resolved);
        CHECK(readFile(repo.path("tracked.txt")) == repo.manual);
        expectUnchanged(repo, before, true);
        CHECK(unmerged(repo).contains("tracked.txt"));
    });
    suite.run("Mark resolved publishes one unsaved path and leaves the other conflict", [] {
        Session session;
        CHECK(!openedFile(session.controller).publication().checkpoint());
        CHECK(session.controller.commands()->markResolved(session.id, session.revision(), true));
        waitFor(session.controller, [&] { return openedFile(session.controller).facts().resolved; });
        CHECK(openedFile(session.controller).publication().checkpoint());
        CHECK(openedFile(session.controller).facts().status == FileStatus::Resolved);
        CHECK(session.controller.selectedFile()->status() == "Written and resolved");
        CHECK(session.repo.git({"show", ":tracked.txt"}) == session.repo.manual);
        CHECK(readFile(session.repo.path("tracked.txt")) == session.repo.manual);
        const auto remaining = unmerged(session.repo);
        CHECK(!remaining.contains("tracked.txt") && remaining.contains("excluded.txt")
              && !remaining.contains("notes.txt"));
        CHECK(session.repo.git({"rev-parse", ":notes.txt"}).trimmed() == session.repo.notesObject);
        CHECK(!session.controller.completion()->handoff().clean);
        CHECK(!session.controller.mergetool()->parentStagingOffer(session.id));
    });
    suite.run("parent-owned staging waits for Git and resolves only after that index entry matches", [] {
        Session session;
        CHECK(session.controller.mergetool()->assignParentStaging("tracked.txt"));
        CHECK(session.controller.commands()->markResolved(session.id, session.revision(), true));
        waitFor(session.controller, [&] {
            return !session.controller.commands()->resolutionBusy()
                && session.controller.mergetool()->parentStagingOffer(session.id).has_value();
        });
        CHECK(openedFile(session.controller).publication().writeOutcome()
              && openedFile(session.controller).publication().writeOutcome()->outcome == Outcome::Succeeded);
        CHECK(!openedFile(session.controller).publication().stageOutcome());
        CHECK(!openedFile(session.controller).facts().resolved);
        CHECK(readFile(session.repo.path("tracked.txt")) == session.repo.manual);
        CHECK(unmerged(session.repo).contains("tracked.txt") && unmerged(session.repo).contains("excluded.txt"));
        const auto offered = session.controller.mergetool()->parentStagingOffer(session.id);
        CHECK(offered && offered->path == "tracked.txt" && !offered->canonicalObjectId.isEmpty());
        CHECK(session.controller.mergetool()->parentStagingPath().endsWith(QStringLiteral("tracked.txt")));
        CHECK(session.controller.mergetool()->verifyParentAcknowledgment(session.id, session.revision()));
        waitFor(session.controller, [&] { return !session.controller.commands()->resolutionBusy(); });
        CHECK(!openedFile(session.controller).facts().resolved);
        CHECK(session.controller.mergetool()->parentStagingOffer(session.id));
        CHECK(unmerged(session.repo).contains("tracked.txt"));
        session.repo.git({"add", "--", "tracked.txt"});
        CHECK(!unmerged(session.repo).contains("tracked.txt"));
        CHECK(session.controller.mergetool()->verifyParentAcknowledgment(session.id, session.revision()));
        waitFor(session.controller, [&] { return openedFile(session.controller).facts().resolved; });
        CHECK(!session.controller.mergetool()->parentStagingOffer(session.id));
        CHECK(unmerged(session.repo).contains("excluded.txt"));
        CHECK(session.repo.git({"rev-parse", ":notes.txt"}).trimmed() == session.repo.notesObject);
        CHECK(session.repo.git({"show", ":tracked.txt"}) == session.repo.manual);
        CHECK(!session.controller.completion()->handoff().clean);
    });
    suite.run("a restarted retry keeps the successful write and resolves it", [] {
        RestartedWrite restarted;
        CHECK(restarted.controller->commands()->markResolved(restarted.id,
                                                             revisionOf(openedFile(*restarted.controller)), true));
        waitFor(*restarted.controller, [&] { return openedFile(*restarted.controller).facts().resolved; });
        const auto &file = openedFile(*restarted.controller);
        CHECK(file.publication().writeOutcome() && file.publication().writeOutcome()->outcome == Outcome::Succeeded
              && file.publication().writeOutcome()->revision == file.current().id);
        CHECK(file.facts().status == FileStatus::Resolved);
        CHECK(!restarted.controller->feedback()->message().contains(QStringLiteral("no longer matches")));
        CHECK(restarted.repo.git({"show", ":tracked.txt"}) == restarted.edited);
        CHECK(readFile(restarted.repo.path("tracked.txt")) == restarted.edited);
        const auto remaining = unmerged(restarted.repo);
        CHECK(!remaining.contains("tracked.txt") && remaining.contains("excluded.txt"));
        CHECK(restarted.repo.git({"rev-parse", ":notes.txt"}).trimmed() == restarted.repo.notesObject);
    });
    suite.run("a destination that differs from the written revision fails closed after restart", [] {
        RestartedWrite restarted;
        const auto tampered = QByteArray("tampered\n");
        writeFile(restarted.repo.path("tracked.txt"), tampered);
        CHECK(!restarted.controller->commands()->markResolved(restarted.id,
                                                              revisionOf(openedFile(*restarted.controller)), true));
        CHECK(!restarted.controller->commands()->resolutionBusy());
        const auto &file = openedFile(*restarted.controller);
        CHECK(file.publication().writeOutcome() && file.publication().writeOutcome()->outcome == Outcome::Failed);
        CHECK(!file.facts().resolved);
        CHECK(restarted.controller->feedback()->message().contains(QStringLiteral("written revision")));
        CHECK(readFile(restarted.repo.path("tracked.txt")) == tampered);
        const auto index = restarted.repo.git({"ls-files", "-s", "--", "tracked.txt"});
        CHECK(index.contains(" 1\ttracked.txt") && !index.contains(" 0\ttracked.txt"));
        CHECK(unmerged(restarted.repo).contains("tracked.txt") && unmerged(restarted.repo).contains("excluded.txt"));
        CHECK(restarted.repo.git({"rev-parse", ":notes.txt"}).trimmed() == restarted.repo.notesObject);
    });
    suite.run("a stale publish still allows a later Mark resolved when the disk matches", [] {
        Session session;
        const auto next = std::string("later resolved revision\n");
        publishWhileDraftChanges(session.controller, session.id, next);
        const auto &written = openedFile(session.controller);
        CHECK(written.publication().writeOutcome()
              && written.publication().writeOutcome()->outcome == Outcome::Succeeded);
        CHECK(written.publication().writeOutcome()->revision != written.current().id);
        CHECK(written.current().draft.text == next);
        CHECK(!written.facts().resolved);
        CHECK(readFile(session.repo.path("tracked.txt")) == session.repo.manual);
        CHECK(session.controller.feedback()->message().contains(QStringLiteral("draft changed")));
        CHECK(unmerged(session.repo).contains("tracked.txt") && unmerged(session.repo).contains("excluded.txt"));
        CHECK(session.controller.commands()->markResolved(session.id, revisionOf(written), true));
        waitFor(session.controller, [&] { return openedFile(session.controller).facts().resolved; });
        const auto &resolved = openedFile(session.controller);
        CHECK(resolved.facts().status == FileStatus::Resolved);
        CHECK(readFile(session.repo.path("tracked.txt")) == QByteArray::fromStdString(next));
        CHECK(session.repo.git({"show", ":tracked.txt"}) == QByteArray::fromStdString(next));
        CHECK(!session.controller.feedback()->message().contains(QStringLiteral("identity changed")));
        CHECK(session.controller.commands()->markResolved(session.id, revisionOf(resolved), true));
        CHECK(session.controller.feedback()->message().contains(QStringLiteral("Written and resolved")));
        CHECK(!unmerged(session.repo).contains("tracked.txt") && unmerged(session.repo).contains("excluded.txt"));
        CHECK(session.repo.git({"rev-parse", ":notes.txt"}).trimmed() == session.repo.notesObject);
    });
    suite.run("a second Mark resolved while the first is still publishing is refused and publishes once", [] {
        Session session;
        auto draft = openedFile(session.controller).current().draft;
        draft.text = "published once\n";
        draft.unresolvedMarkers = 0;
        CHECK(session.controller.store()->replaceDraft(session.id.toStdString(),
                                                       openedFile(session.controller).current().id, draft)
              == Transition::Applied);
        const auto revision = session.revision();
        {
            PublishStall stall;
            CHECK(session.controller.commands()->markResolved(session.id, revision, true));
            CHECK(stall.waitForExchange());
            // The first attempt holds the publisher between its write and its staging.
            CHECK(session.controller.commands()->resolutionBusy());
            CHECK(!session.controller.commands()->markResolved(session.id, revision, true));
            const auto again = session.controller.publisher()->start(session.id.toStdString(),
                                                                     openedFile(session.controller).current().id, true);
            CHECK(again.transition == Transition::Blocked);
            CHECK_EQ(again.detail, QStringLiteral("Mark resolved is already working on a file."));
            stall.releaseWorker();
            waitFor(session.controller, [&] { return !session.controller.commands()->resolutionBusy(); });
        }
        CHECK(openedFile(session.controller).facts().resolved);
        CHECK(readFile(session.repo.path("tracked.txt")) == "published once\n");
        CHECK(!unmerged(session.repo).contains("tracked.txt"));
        // One publication and one staging intent: the refused attempts recorded none.
        const auto replay = replayRecovery(storage::RecoveryStore(session.controller.store()->recoveryDirectory()));
        CHECK(replay.succeeded);
        const auto count = [&](MutationKind kind) {
            return std::count_if(replay.mutations.begin(), replay.mutations.end(),
                                 [kind](const RecoveryMutation &mutation) { return mutation.kind == kind; });
        };
        CHECK_EQ(count(MutationKind::Publication), 1);
        CHECK_EQ(count(MutationKind::Staging), 1);
    });
    suite.run("staging that times out leaves the outcome uncertain, the conflict in place and a retry refused", [] {
        Repository repo;
        ScopedEnvironment environment(repo.environment);
        const auto diagnostic = QStringLiteral("Git did not finish within the request deadline.");
        // Git add hangs past the adapter's deadline: the index may or may not have changed.
        const auto timedOut = [diagnostic](const git::Request &) {
            git::Result result;
            result.outcome = git::Outcome::TimedOut;
            result.mutation = git::Mutation::Uncertain;
            result.diagnostic = diagnostic;
            return result;
        };
        Application controller{Application::Config{
            .gitExecutable = gitOnPath(),
            .recoveryDirectory = newRecoveryDirectory(),
            .correspondence = {},
            .sessionGit = test_support::ScriptedGitRunner::runners(git::Operation::StageFile, timedOut, gitOnPath())}};
        openController(controller, repo);
        const auto id = selectTracked(controller);
        auto draft = openedFile(controller).current().draft;
        draft.text = "written before the time out\n";
        draft.unresolvedMarkers = 0;
        CHECK(controller.store()->replaceDraft(id.toStdString(), openedFile(controller).current().id, draft)
              == Transition::Applied);
        const auto revision = revisionOf(openedFile(controller));
        CHECK(controller.commands()->markResolved(id, revision, true));
        waitFor(controller, [&] { return !controller.commands()->resolutionBusy(); });
        const auto &file = openedFile(controller);
        CHECK(file.publication().writeOutcome() && file.publication().writeOutcome()->outcome == Outcome::Succeeded);
        CHECK(file.publication().stageOutcome() && file.publication().stageOutcome()->outcome == Outcome::Uncertain);
        CHECK_EQ(QString::fromStdString(file.publication().stageOutcome()->detail), diagnostic);
        CHECK(!file.facts().resolved);
        CHECK(readFile(repo.path("tracked.txt")) == "written before the time out\n");
        CHECK(unmerged(repo).contains("tracked.txt"));
        // An uncertain outcome has to be inspected before Mark resolved runs on that revision again.
        CHECK(!controller.commands()->markResolved(id, revision, true));
        CHECK_EQ(controller.feedback()->message(),
                 QStringLiteral("Outcome uncertain. Inspect the repository file and index before retrying."));
        CHECK(!controller.commands()->resolutionBusy());
    });
    suite.run("a destination that differs from the published bytes fails closed", [] {
        Session session;
        const auto next = std::string("later resolved revision\n");
        publishWhileDraftChanges(session.controller, session.id, next);
        const auto tampered = QByteArray("tampered after publish\n");
        writeFile(session.repo.path("tracked.txt"), tampered);
        CHECK(
            session.controller.commands()->markResolved(session.id, revisionOf(openedFile(session.controller)), true));
        waitFor(session.controller, [&] {
            const auto &file = openedFile(session.controller);
            return !session.controller.commands()->resolutionBusy() && file.publication().writeOutcome()
                && file.publication().writeOutcome()->outcome == Outcome::Failed
                && file.publication().writeOutcome()->revision == file.current().id;
        });
        CHECK(!openedFile(session.controller).facts().resolved);
        CHECK(readFile(session.repo.path("tracked.txt")) == tampered);
        CHECK(session.controller.feedback()->message().contains(QStringLiteral("changed")));
        CHECK(unmerged(session.repo).contains("tracked.txt") && unmerged(session.repo).contains("excluded.txt"));
        CHECK(session.repo.git({"rev-parse", ":notes.txt"}).trimmed() == session.repo.notesObject);
    });
    suite.run("Mark resolved republishes after Git restores the mergetool backup", [] {
        Session session;
        const auto merged = session.repo.path("tracked.txt");
        const auto backup = session.repo.path("tracked_BACKUP_1.txt");
        const QByteArray reviewed("reviewed in app\n");
        auto draft = openedFile(session.controller).current().draft;
        draft.text = reviewed.toStdString();
        draft.unresolvedMarkers = 0;
        CHECK(session.controller.store()->replaceDraft(session.id.toStdString(),
                                                       openedFile(session.controller).current().id, draft)
              == Transition::Applied);
        const auto revision = session.revision();
        // git mergetool moves MERGED to BACKUP and copies it back before the tool runs.
        CHECK(QFile::rename(merged, backup) && QFile::copy(backup, merged));
        const auto offered = [&] {
            CHECK(session.controller.mergetool()->assignParentStaging({})
                  && session.controller.mergetool()->assignParentStaging("tracked.txt"));
            CHECK(session.controller.commands()->markResolved(session.id, revision, true));
            waitFor(session.controller, [&] {
                return !session.controller.commands()->resolutionBusy()
                    && session.controller.mergetool()->parentStagingOffer(session.id).has_value();
            });
        };
        const auto inode = [&] {
            struct stat entry{};
            CHECK(::stat(QFile::encodeName(merged).constData(), &entry) == 0);
            return entry.st_ino;
        };
        offered();
        CHECK(readFile(merged) == reviewed);
        // The resolved reply is lost and the launcher exits non-zero: Git moves BACKUP onto MERGED.
        CHECK(::rename(QFile::encodeName(backup).constData(), QFile::encodeName(merged).constData()) == 0);
        CHECK(readFile(merged) == session.repo.manual);
        offered();
        CHECK(readFile(merged) == reviewed);
        CHECK(openedFile(session.controller).publication().writeOutcome()
              && openedFile(session.controller).publication().writeOutcome()->outcome == Outcome::Succeeded);
        CHECK(!openedFile(session.controller).facts().resolved && unmerged(session.repo).contains("tracked.txt"));
        const auto written = inode();
        offered();
        CHECK(inode() == written && readFile(merged) == reviewed);
        const QByteArray tampered("changed while Git was waiting\n");
        writeFile(merged, tampered);
        CHECK(session.controller.mergetool()->assignParentStaging({})
              && session.controller.mergetool()->assignParentStaging("tracked.txt"));
        CHECK(session.controller.commands()->markResolved(session.id, revision, true));
        waitFor(session.controller, [&] {
            const auto &file = openedFile(session.controller);
            return !session.controller.commands()->resolutionBusy() && file.publication().writeOutcome()
                && file.publication().writeOutcome()->outcome == Outcome::Failed;
        });
        CHECK(!session.controller.mergetool()->parentStagingOffer(session.id));
        CHECK(!openedFile(session.controller).facts().resolved);
        CHECK(readFile(merged) == tampered);
        const auto index = session.repo.git({"ls-files", "-s", "--", "tracked.txt"});
        CHECK(index.contains(" 1\ttracked.txt") && !index.contains(" 0\ttracked.txt"));
        CHECK(session.repo.git({"rev-parse", ":notes.txt"}).trimmed() == session.repo.notesObject);
    });
    suite.run("repository mode fails before git add when the written revision was edited outside the app", [] {
        Session session;
        const auto merged = session.repo.path("tracked.txt");
        const QByteArray reviewed("reviewed in app\n");
        auto draft = openedFile(session.controller).current().draft;
        draft.text = reviewed.toStdString();
        draft.unresolvedMarkers = 0;
        CHECK(session.controller.store()->replaceDraft(session.id.toStdString(),
                                                       openedFile(session.controller).current().id, draft)
              == Transition::Applied);
        const auto revision = session.revision();
        // The write succeeds and the refused staging hook keeps the conflict in the index.
        const auto hook = session.repo.path(".git/hooks/post-index-change");
        writeFile(hook, "#!/bin/sh\nexit 1\n");
        CHECK(QFile::setPermissions(hook, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        CHECK(session.controller.commands()->markResolved(session.id, revision, true));
        waitFor(session.controller, [&] {
            const auto &file = openedFile(session.controller);
            return !session.controller.commands()->resolutionBusy() && file.publication().writeOutcome()
                && file.publication().writeOutcome()->outcome == Outcome::Succeeded
                && file.publication().stageOutcome();
        });
        CHECK(openedFile(session.controller).publication().stageOutcome()->outcome == Outcome::Failed);
        CHECK(readFile(merged) == reviewed && unmerged(session.repo).contains("tracked.txt"));
        // Staging would now succeed, so any git add would reach stage 0.
        CHECK(QFile::remove(hook));
        const QByteArray external("edited outside the app after the write\n");
        writeFile(merged, external);
        CHECK(session.controller.commands()->markResolved(session.id, revision, true));
        waitFor(session.controller, [&] {
            const auto &file = openedFile(session.controller);
            return !session.controller.commands()->resolutionBusy() && file.publication().writeOutcome()
                && file.publication().writeOutcome()->outcome == Outcome::Failed;
        });
        const auto &file = openedFile(session.controller);
        CHECK(file.publication().writeOutcome()->revision == file.current().id && !file.facts().resolved);
        CHECK(session.controller.feedback()->message().contains(
            QStringLiteral("no longer matches the written revision")));
        CHECK(file.publication().stageOutcome() && file.publication().stageOutcome()->outcome == Outcome::Failed);
        CHECK(readFile(merged) == external);
        const auto index = session.repo.git({"ls-files", "-s", "--", "tracked.txt"});
        CHECK(index.contains(" 1\ttracked.txt") && !index.contains(" 0\ttracked.txt"));
        CHECK(unmerged(session.repo).contains("tracked.txt") && unmerged(session.repo).contains("excluded.txt"));
        CHECK(session.repo.git({"rev-parse", ":notes.txt"}).trimmed() == session.repo.notesObject);
    });
    suite.run("leave refuses unrecovered input and a running resolution and finish waits for a clean handoff", [] {
        Session session;
        SessionEnd end;
        watchSessionEnd(session.controller, end);
        auto &controller = session.controller;
        const QString pendingBlocker =
            "Pending input is not in recovery yet. Retry recovery before leaving so the draft is kept.";
        const QString runningBlocker = "Wait for the running resolution to finish before leaving.";
        const QString unclean = "Work remains outstanding, so this session cannot finish cleanly. "
                                "Open an outstanding file or leave deliberately.";
        const auto before = session.repo.snapshot();

        auto edited = openedFile(controller).current().draft;
        edited.text = "pending input before leaving\n";
        edited.unresolvedMarkers = 0;
        {
            test_support::ArmedFault fault(*session.files, test_support::SyncFaults::Point::JournalSync);
            CHECK(
                controller.store()->replaceDraft(session.id.toStdString(), openedFile(controller).current().id, edited)
                == Transition::Blocked);
            CHECK(session.files->failures == 1);
        }
        CHECK(controller.files()->hasPendingRecovery());
        auto summary = controller.completion()->handoffData();
        CHECK(!summary.value("canLeave").toBool() && summary.value("leaveBlocker").toString() == pendingBlocker);
        CHECK(!summary.value("clean").toBool() && summary.value("sessionBlockers").toStringList().isEmpty());
        CHECK(handoffEntry(summary, session.id).value("action").toString() == "retry-recovery");
        CHECK(!controller.completion()->leave());
        CHECK(controller.feedback()->message() == pendingBlocker);
        CHECK(end.count == 0 && controller.completion()->outcome().isEmpty());
        CHECK(!controller.completion()->finish());
        CHECK(controller.feedback()->message() == unclean && end.count == 0);
        CHECK(controller.files()->hasPendingRecovery());
        expectUnchanged(session.repo, before);

        CHECK(controller.commands()->retryPendingRecovery(session.id));
        CHECK(openedFile(controller).current().draft == edited);
        summary = controller.completion()->handoffData();
        CHECK(summary.value("canLeave").toBool() && summary.value("leaveBlocker").toString().isEmpty());
        CHECK(handoffEntry(summary, session.id).value("category").toString() != "failed");

        {
            PublishStall stall;
            CHECK(controller.commands()->markResolved(session.id, session.revision(), true));
            CHECK(stall.waitForExchange());
            CHECK(controller.commands()->resolutionBusy());
            summary = controller.completion()->handoffData();
            CHECK(!summary.value("canLeave").toBool() && summary.value("leaveBlocker").toString() == runningBlocker);
            CHECK(summary.value("sessionBlockers").toStringList()
                  == QStringList{"A resolution is still running; its outcome is not known yet."});
            CHECK(!summary.value("clean").toBool());
            CHECK(!controller.completion()->leave());
            CHECK(controller.feedback()->message() == runningBlocker);
            CHECK(!controller.completion()->finish());
            CHECK(controller.feedback()->message() == unclean);
            CHECK(end.count == 0 && controller.completion()->outcome().isEmpty());
            stall.releaseWorker();
            waitFor(controller, [&] { return !controller.commands()->resolutionBusy(); });
        }
        CHECK(openedFile(controller).facts().resolved);

        // One file stays outstanding, so finishing is still refused.
        CHECK(!controller.completion()->handoffData().value("clean").toBool());
        CHECK(!controller.completion()->finish() && controller.feedback()->message() == unclean && end.count == 0);
        const auto excluded = QString::fromLatin1(QByteArray("excluded.txt").toHex());
        CHECK(controller.selection()->select(excluded));
        auto resolved = openedFile(controller, "excluded.txt").current().draft;
        resolved.text = "excluded resolved\n";
        resolved.unresolvedMarkers = 0;
        CHECK(controller.store()->replaceDraft(excluded.toStdString(),
                                               openedFile(controller, "excluded.txt").current().id, resolved)
              == Transition::Applied);
        CHECK(controller.commands()->markResolved(excluded, revisionOf(openedFile(controller, "excluded.txt")), true));
        waitFor(controller, [&] {
            return !controller.commands()->resolutionBusy() && openedFile(controller, "excluded.txt").facts().resolved;
        });
        CHECK(unmerged(session.repo).isEmpty());

        summary = controller.completion()->handoffData();
        CHECK(summary.value("clean").toBool() && summary.value("canLeave").toBool());
        CHECK(summary.value("outstandingFiles").toInt() == 0 && summary.value("totalFiles").toInt() == 2);
        const auto store = controller.store()->recoveryDirectory();
        CHECK(QFileInfo::exists(store + "/journal"));
        CHECK(controller.completion()->finish());
        CHECK(end.count == 1 && end.clean);
        // Nothing in the finished session's recovery store is needed any more.
        CHECK(!QFileInfo::exists(store));
        CHECK(end.outcome == summary.value("outcome").toString() && controller.completion()->outcome() == end.outcome);
        CHECK(end.outcome.startsWith("All 2 files in this session are written and resolved in Git."));
        CHECK(!controller.completion()->handoffData().value("canLeave").toBool());
        CHECK(!controller.completion()->finish() && !controller.completion()->leave());
        CHECK(end.count == 1 && controller.completion()->outcome() == end.outcome);
    });
    suite.run("a marker refusal names the markers and the first line and moves the cursor there", [] {
        Session session;
        const auto before = session.repo.snapshot();
        const auto replace = [&](std::string text) {
            auto draft = openedFile(session.controller).current().draft;
            draft.text = std::move(text);
            CHECK(session.controller.store()->replaceDraft(session.id.toStdString(),
                                                           openedFile(session.controller).current().id, draft)
                  == Transition::Applied);
        };
        // "caf\xc3\xa9 \xf0\x9f\x98\x80\n" is 8 UTF-16 code units and "kept\n" 5 more.
        replace("caf\xc3\xa9 \xf0\x9f\x98\x80\nkept\n<<<<<<< ours\nleft\n=======\nright\n>>>>>>> theirs\n=======\n");
        QObject editor;
        editor.setProperty("cursorPosition", 0);
        session.controller.editor()->attach(&editor);
        CHECK(session.controller.feedback()->markerBlocker().isEmpty()
              && !session.controller.commands()->showFirstMarker());
        CHECK(!session.controller.commands()->markResolved(session.id, session.revision(), true));
        CHECK(session.controller.feedback()->message()
              == QStringLiteral(
                  "Mark resolved is blocked: 1 conflict-marker block and 1 stray conflict-marker line remain; "
                  "first at line 3. The repository file is unchanged."));
        auto blocker = session.controller.feedback()->markerBlocker();
        CHECK(blocker.value("fileId").toString() == session.id
              && blocker.value("revision").toString() == session.revision());
        CHECK(blocker.value("count").toULongLong() == 2 && blocker.value("line").toULongLong() == 3);
        CHECK(session.controller.commands()->showFirstMarker());
        CHECK(editor.property("cursorPosition").toInt() == 13);
        CHECK(!openedFile(session.controller).publication().reviewed()
              && !openedFile(session.controller).facts().resolved);
        replace("top\n<<<<<<< ours\na\n=======\nb\n>>>>>>> theirs\nmid\n<<<<<<< ours\nc\n=======\nd\n>>>>>>> theirs\n");
        // A newer revision retires the action until Mark resolved is refused again.
        CHECK(session.controller.feedback()->markerBlocker().isEmpty()
              && !session.controller.commands()->showFirstMarker());
        CHECK(!session.controller.commands()->markResolved(session.id, session.revision(), true));
        CHECK(session.controller.feedback()->message()
              == QStringLiteral("Mark resolved is blocked: 2 conflict-marker blocks remain; first at line 2. The "
                                "repository file is unchanged."));
        blocker = session.controller.feedback()->markerBlocker();
        CHECK(blocker.value("count").toULongLong() == 2 && blocker.value("line").toULongLong() == 2);
        editor.setProperty("cursorPosition", 0);
        CHECK(session.controller.commands()->showFirstMarker() && editor.property("cursorPosition").toInt() == 4);
        replace("only\n>>>>>>> theirs\n");
        CHECK(!session.controller.commands()->markResolved(session.id, session.revision(), true));
        CHECK(session.controller.feedback()->message()
              == QStringLiteral("Mark resolved is blocked: 1 stray conflict-marker line remains; first at line 2. The "
                                "repository file is unchanged."));
        // The handoff keeps its label for the same blocker.
        CHECK(engine_text::blocker(Blocker::UnresolvedMarkers) == "Known conflict markers remain");
        const auto blockers = openedFile(session.controller).facts().blockers;
        CHECK(std::find(blockers.begin(), blockers.end(), Blocker::UnresolvedMarkers) != blockers.end());
        session.controller.editor()->attach(nullptr);
        CHECK(readFile(session.repo.path("tracked.txt")) == session.repo.manual);
        CHECK(unmerged(session.repo).contains("tracked.txt"));
        expectUnchanged(session.repo, before);
    });
    suite.run("discard to staged restores the staged result behind the stale and blocked checks", [] {
        Session session;
        CHECK(session.controller.commands()->markResolved(session.id, session.revision(), true));
        waitFor(session.controller, [&] { return openedFile(session.controller).facts().resolved; });
        const auto stagedText = openedFile(session.controller).current().draft.text;
        const auto stagedRevision = session.revision();
        const auto before = session.repo.snapshot();
        // With no newer draft there is nothing to discard.
        CHECK(session.controller.commands()->discardToStaged(session.id, stagedRevision));
        CHECK(session.controller.feedback()->message() == QStringLiteral("No change."));
        auto edited = openedFile(session.controller).current().draft;
        edited.text += "later edit\n";
        CHECK(session.controller.store()->replaceDraft(session.id.toStdString(),
                                                       openedFile(session.controller).current().id, edited)
              == Transition::Applied);
        const auto draftRevision = session.revision();
        CHECK(openedFile(session.controller).facts().status == FileStatus::EarlierStaged);
        CHECK(session.controller.selectedFile()->progress().value("earlierResolved").toBool());

        // A stale revision is refused like every other command and keeps the draft.
        CHECK(!session.controller.commands()->discardToStaged(session.id, stagedRevision));
        CHECK(session.controller.feedback()->message().contains(QStringLiteral("changed")));
        CHECK(openedFile(session.controller).current().draft.text == edited.text);

        // A journal failure keeps the acknowledged draft, and the pending discard blocks other commands.
        {
            test_support::ArmedFault fault(*session.files, test_support::SyncFaults::Point::JournalSync);
            CHECK(!session.controller.commands()->discardToStaged(session.id, draftRevision));
            CHECK(session.files->failures == 1);
        }
        CHECK(session.controller.files()->hasPendingRecovery());
        CHECK(session.controller.feedback()->message()
              == QStringLiteral("Discard was not completed. The draft is kept until retry finishes the discard."));
        CHECK(openedFile(session.controller).current().draft.text == edited.text);
        CHECK(session.revision() == draftRevision);
        CHECK(!session.controller.commands()->discardToStaged(session.id, draftRevision));
        CHECK(session.controller.feedback()->message().contains(QStringLiteral("Command not completed")));
        CHECK(!session.controller.commands()->saveDraft(session.id, draftRevision));
        CHECK(openedFile(session.controller).current().draft.text == edited.text);

        // Retry commits the same discard: the staged revision is current and resolved again.
        CHECK(session.controller.commands()->retryPendingRecovery(session.id));
        CHECK(!session.controller.files()->hasPendingRecovery());
        CHECK(session.controller.feedback()->message()
              == QStringLiteral(
                  "Discarded the draft. The result staged in Git is current again; the repository file is unchanged."));
        CHECK(session.revision() == stagedRevision);
        CHECK(openedFile(session.controller).current().draft.text == stagedText);
        CHECK(openedFile(session.controller).facts().resolved);
        CHECK(!openedFile(session.controller).facts().earlierStaged);
        CHECK(session.controller.selectedFile()->status() == "Written and resolved");
        CHECK(handoffEntry(session.controller.completion()->handoffData(), session.id).value("category").toString()
              == "published");
        expectUnchanged(session.repo, before);
    });
    suite.run("discard to staged without a staged result refuses and keeps the draft", [] {
        Session unstaged;
        const auto text = openedFile(unstaged.controller).current().draft.text;
        CHECK(!unstaged.controller.commands()->discardToStaged(unstaged.id, unstaged.revision()));
        CHECK(unstaged.controller.feedback()->message()
              == QStringLiteral(
                  "Discard was not completed. No verified result staged in Git to return to, so the draft is kept."));
        CHECK(openedFile(unstaged.controller).current().draft.text == text);
    });
    // The messages are the ones each command showed before one guard replaced
    // its own copy of the checks. The previews never checked the current
    // revision or support themselves, so the preview reports those.
    suite.run("every result command refuses a stale revision, a blocked file and an unsupported file as before", [] {
        Session session(true);
        auto &controller = session.controller;
        const QString stale = QStringLiteral("The result changed. Select the scope again.");
        const QString blocked = QStringLiteral("Command not completed. Check pending recovery and the selected scope.");
        struct Command {
            const char *name;
            std::function<bool(const QString &id, const QString &revision)> run;
            QString staleMessage;
            QString unsupportedMessage;
        };
        const std::vector<Command> commands{
            {"previewChoice",
             [&](const QString &id, const QString &revision) {
                 return controller.choices()->previewChoice(id, revision, QStringLiteral("unknown"),
                                                            ChoicePreviewFlow::Action::Accept);
             },
             stale, QStringLiteral("This file cannot be edited in Branch Truce.")},
            {"previewNonConflicting",
             [&](const QString &id, const QString &revision) {
                 return controller.choices()->previewNonConflicting(id, revision,
                                                                    ChoicePreviewFlow::NonConflicting::BothChanges);
             },
             QStringLiteral("The result changed. Preview the batch again."),
             QStringLiteral("This file cannot apply non-conflicting changes.")},
            {"previewWholeFile",
             [&](const QString &id, const QString &revision) {
                 return controller.choices()->previewWholeFile(id, revision, ChoicePreviewFlow::Side::LeftSide);
             },
             QStringLiteral("The result changed. Inspect a new whole-file preview."),
             QStringLiteral("This file has no editable captured source.")},
            {"saveDraft",
             [&](const QString &id, const QString &revision) { return controller.commands()->saveDraft(id, revision); },
             stale, blocked},
            {"markResolved",
             [&](const QString &id, const QString &revision) {
                 return controller.commands()->markResolved(id, revision, true);
             },
             stale, blocked},
            {"discardUnsaved",
             [&](const QString &id, const QString &revision) {
                 return controller.commands()->discardUnsaved(id, revision);
             },
             stale, blocked},
            {"discardToStaged",
             [&](const QString &id, const QString &revision) {
                 return controller.commands()->discardToStaged(id, revision);
             },
             stale, blocked},
        };
        const auto before = session.repo.snapshot();
        const auto refuses = [&](const Command &command, const char *path, const QString &revision,
                                 const QString &message) {
            const auto draft = openedFile(controller, path).current();
            if (command.run(QString::fromLatin1(QByteArray(path).toHex()), revision))
                throw std::runtime_error(std::string(command.name) + " ran on " + path + " at revision "
                                         + revision.toStdString());
            if (controller.feedback()->message() != message)
                throw std::runtime_error(std::string(command.name)
                                         + " showed: " + controller.feedback()->message().toStdString());
            CHECK(controller.choices()->choicePreview().isEmpty()
                  && controller.choices()->nonConflictingPreview().isEmpty()
                  && controller.choices()->wholeFilePreview().isEmpty());
            CHECK(!controller.commands()->resolutionBusy() && !controller.commands()->resolutionConfirmation());
            CHECK(openedFile(controller, path).current().id == draft.id
                  && openedFile(controller, path).current().draft == draft.draft);
        };

        const auto current = openedFile(controller).current().id;
        for (const auto &command : commands) {
            // A revision that is not the file's current one, and one that does not parse.
            refuses(command, "tracked.txt", QString::number(static_cast<qulonglong>(current + 1)),
                    command.staleMessage);
            refuses(command, "tracked.txt", QStringLiteral("not a revision"), stale);
            // A file other than the selected one.
            refuses(command, "excluded.txt", revisionOf(openedFile(controller, "excluded.txt")), stale);
        }

        // An unsupported file, selected at its current revision.
        CHECK(openedFile(controller, "excluded.txt").support() != Support::Supported);
        CHECK(controller.selection()->select(QString::fromLatin1(QByteArray("excluded.txt").toHex())));
        for (const auto &command : commands)
            refuses(command, "excluded.txt", revisionOf(openedFile(controller, "excluded.txt")),
                    command.unsupportedMessage);

        // Pending recovery input blocks the file at its current revision.
        CHECK(controller.selection()->select(session.id));
        auto edited = openedFile(controller).current().draft;
        edited.text += "pending edit\n";
        {
            test_support::ArmedFault fault(*session.files, test_support::SyncFaults::Point::JournalSync);
            CHECK(controller.store()->replaceDraft(session.id.toStdString(), current, edited) != Transition::Applied);
            CHECK(session.files->failures == 1);
        }
        CHECK(controller.files()->hasPendingRecovery());
        CHECK(openedFile(controller).current().id == current);
        for (const auto &command : commands) refuses(command, "tracked.txt", session.revision(), blocked);
        expectUnchanged(session.repo, before);
    });
    suite.run("each concern notifies through its own signal and a new message notifies nothing else", [] {
        Session session;
        auto &controller = session.controller;
        const auto notifies = [](const QObject *object, const char *name, QMetaMethod signal) {
            const auto *meta = object->metaObject();
            if (meta->property(meta->indexOfProperty(name)).notifySignal() != signal)
                throw std::runtime_error(std::string(name) + " notifies through another signal");
        };
        const auto feedback = QMetaMethod::fromSignal(&CommandFeedback::changed);
        notifies(controller.feedback(), "message", feedback);
        notifies(controller.feedback(), "markerBlocker", feedback);
        notifies(controller.choices(), "choicePreview",
                 QMetaMethod::fromSignal(&ChoicePreviewFlow::choicePreviewChanged));
        notifies(controller.choices(), "nonConflictingPreview",
                 QMetaMethod::fromSignal(&ChoicePreviewFlow::nonConflictingPreviewChanged));
        notifies(controller.choices(), "wholeFilePreview",
                 QMetaMethod::fromSignal(&ChoicePreviewFlow::wholeFilePreviewChanged));
        const auto resolution = QMetaMethod::fromSignal(&FileCommands::resolutionChanged);
        for (const auto *name : {"resolutionConfirmation", "resolutionFileId", "resolutionRevision", "resolutionBusy"})
            notifies(controller.commands(), name, resolution);
        const auto mergetool = QMetaMethod::fromSignal(&MergetoolStatus::changed);
        for (const auto *name : {"parentStagingPath", "waiting", "parentSessionActive", "canEndParentSession"})
            notifies(controller.mergetool(), name, mergetool);
        notifies(controller.completion(), "handoff", QMetaMethod::fromSignal(&SessionCompletion::changed));

        // A pending preview, so a preview notification would have something to say.
        CHECK(
            controller.choices()->previewWholeFile(session.id, session.revision(), ChoicePreviewFlow::Side::LeftSide));
        CHECK(!controller.choices()->wholeFilePreview().isEmpty());
        std::map<std::string, int> emitted;
        const auto count = [&](const char *name, auto *sender, auto signal) {
            QObject::connect(sender, signal, &controller, [&emitted, name] { ++emitted[name]; });
        };
        count("message", controller.feedback(), &CommandFeedback::changed);
        count("commandPreview", controller.choices(), &ChoicePreviewFlow::choicePreviewChanged);
        count("nonConflictingPreview", controller.choices(), &ChoicePreviewFlow::nonConflictingPreviewChanged);
        count("wholeFilePreview", controller.choices(), &ChoicePreviewFlow::wholeFilePreviewChanged);
        count("resolution", controller.commands(), &FileCommands::resolutionChanged);
        count("mergetool", controller.mergetool(), &MergetoolStatus::changed);
        count("handoff", controller.completion(), &SessionCompletion::changed);
        count("selection", controller.selection(), &Selection::changed);

        // A refused Save changes only the command message.
        CHECK(!controller.commands()->saveDraft(session.id, QStringLiteral("not a revision")));
        CHECK(controller.feedback()->message() == QStringLiteral("The result changed. Select the scope again."));
        CHECK(emitted == (std::map<std::string, int>{{"message", 1}}));
        CHECK(!controller.choices()->wholeFilePreview().isEmpty());

        // Cancelling notifies the pending preview and the cleared message only.
        emitted.clear();
        controller.choices()->cancel();
        CHECK(controller.choices()->wholeFilePreview().isEmpty() && controller.feedback()->message().isEmpty());
        CHECK(emitted == (std::map<std::string, int>{{"message", 1}, {"wholeFilePreview", 1}}));
    });
    return suite.report();
}
