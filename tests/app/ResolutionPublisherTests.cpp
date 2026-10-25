#include "app/ApplicationController.h"
#include "TestHarness.h"
#include "fixtures/RecoveryFaults.h"
#include <QCoreApplication>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QMap>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
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

extern "C" int __real_renameat2(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, unsigned int flags);
extern "C" int __wrap_renameat2(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, unsigned int flags)
{
    const int result = __real_renameat2(olddirfd, oldpath, newdirfd, newpath, flags);
    if (result != 0 || (flags & RENAME_EXCHANGE) == 0 || !publish_stall::arm.exchange(false)) return result;
    const int ready = publish_stall::readyWrite.load();
    if (ready >= 0) {
        const char token = 1;
        ssize_t sent = 0;
        do { sent = ::send(ready, &token, 1, MSG_NOSIGNAL); } while (sent < 0 && errno == EINTR);
    }
    const int release = publish_stall::releaseRead.load();
    if (release >= 0) {
        char ack = 0;
        ssize_t received = 0;
        do { received = ::recv(release, &ack, 1, 0); } while (received < 0 && errno == EINTR);
    }
    return result;
}

namespace {
void writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size());
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly));
    const auto bytes = file.readAll();
    CHECK(file.error() == QFileDevice::NoError);
    return bytes;
}

class Repository {
public:
    Repository()
    {
        CHECK(directory.isValid() && executables.isValid());
        root = QFileInfo(directory.path()).canonicalFilePath();
        environment.insert("PATH", "/usr/bin:/bin");
        environment.insert("HOME", executables.path());
        environment.insert("XDG_CONFIG_HOME", executables.path() + "/config");
        environment.insert("XDG_CACHE_HOME", executables.path() + "/cache");
        environment.insert("XDG_DATA_HOME", executables.path() + "/data");
        environment.insert("LC_ALL", "C");
        environment.insert("GIT_CONFIG_NOSYSTEM", "1");
        environment.insert("GIT_CONFIG_GLOBAL", "/dev/null");
        environment.insert("GIT_TERMINAL_PROMPT", "0");
        git({"init", "--quiet", "--initial-branch=main"});
        git({"config", "user.name", "Branch Truce Tests"});
        git({"config", "user.email", "tests@example.invalid"});
        git({"config", "core.autocrlf", "false"});
        writeFile(path("tracked.txt"), "base\n");
        writeFile(path("excluded.txt"), "base\n");
        writeFile(path("notes.txt"), "notes stay staged\n");
        git({"add", "--", "tracked.txt", "excluded.txt", "notes.txt"});
        git({"commit", "--quiet", "-m", "base"});
        git({"checkout", "--quiet", "-b", "side"});
        writeFile(path("tracked.txt"), "side\n");
        writeFile(path("excluded.txt"), "side\n");
        git({"add", "--", "tracked.txt", "excluded.txt"});
        git({"commit", "--quiet", "-m", "side"});
        git({"checkout", "--quiet", "main"});
        writeFile(path("tracked.txt"), "main\n");
        writeFile(path("excluded.txt"), "main\n");
        git({"add", "--", "tracked.txt", "excluded.txt"});
        git({"commit", "--quiet", "-m", "main"});
        git({"merge", "--no-edit", "side"}, 1);
        writeFile(path("tracked.txt"), manual);
        notesObject = git({"rev-parse", ":notes.txt"}).trimmed();
    }
    QString path(const QString &relative) const { return root + '/' + relative; }
    QByteArray git(const QStringList &arguments, int expected = 0) const
    {
        QProcess process;
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(root);
        process.start("/usr/bin/git", QStringList{"--no-pager", "--literal-pathspecs"} + arguments);
        CHECK(process.waitForStarted(5000) && process.waitForFinished(15000));
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != expected)
            throw std::runtime_error("fixture git: " + process.readAllStandardError().toStdString());
        return process.readAllStandardOutput();
    }
    QMap<QString, QByteArray> snapshot() const
    {
        QMap<QString, QByteArray> result;
        QDirIterator files(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const auto selected = files.next();
            result.insert(QDir(root).relativeFilePath(selected), readFile(selected));
        }
        return result;
    }
    QTemporaryDir directory;
    QTemporaryDir executables;
    QString root;
    QProcessEnvironment environment;
    QByteArray notesObject;
    const QByteArray manual = "resolved without a prior save\n";
};

class ScopedEnvironment {
public:
    explicit ScopedEnvironment(const QProcessEnvironment &environment)
    {
        for (const auto name : {"HOME", "PATH", "LC_ALL", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
                                "XDG_DATA_HOME", "GIT_CONFIG_NOSYSTEM", "GIT_CONFIG_GLOBAL", "GIT_TERMINAL_PROMPT"}) {
            previous.insert(name, qEnvironmentVariableIsSet(name)
                ? std::optional<QByteArray>(qgetenv(name)) : std::nullopt);
            qputenv(name, environment.value(QString::fromLatin1(name)).toUtf8());
        }
    }
    ~ScopedEnvironment()
    {
        for (auto entry = previous.cbegin(); entry != previous.cend(); ++entry) {
            if (entry.value()) qputenv(entry.key().constData(), *entry.value());
            else qunsetenv(entry.key().constData());
        }
    }
private:
    QMap<QByteArray, std::optional<QByteArray>> previous;
};

void openController(ApplicationController &controller, const Repository &repo)
{
    const auto before = repo.snapshot();
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&controller, &ApplicationController::launchChanged, &loop, [&] {
        if (!controller.opening()) loop.quit();
    });
    CHECK(controller.openRepository(repo.root));
    timeout.start(15000);
    if (controller.opening()) loop.exec();
    CHECK(!controller.opening());
    if (controller.startupState() != "ready")
        throw std::runtime_error("controller startup: " + controller.detail().toStdString());
    CHECK(repo.snapshot() == before);
}

const FileSession &openedFile(ApplicationController &controller, const char *path = "tracked.txt")
{
    const auto *selected = controller.files()->file(QByteArray(path).toHex().toStdString());
    CHECK(selected != nullptr);
    return *selected;
}

QString selectTracked(ApplicationController &controller)
{
    const auto id = QString::fromLatin1(QByteArray("tracked.txt").toHex());
    CHECK(controller.selectFile(id));
    return id;
}

QString revisionOf(const FileSession &file)
{
    return QString::number(static_cast<qulonglong>(file.current().id));
}

void waitFor(ApplicationController &controller, const std::function<bool()> &ready)
{
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 25000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    if (!ready())
        throw std::runtime_error((controller.commandMessage() + " | "
            + controller.selectedFile().value("status").toString() + " | busy="
            + QString::number(controller.resolutionBusy())).toStdString());
}

QSet<QByteArray> unmerged(const Repository &repo)
{
    QSet<QByteArray> result;
    for (const auto &path : repo.git({"diff", "--name-only", "--diff-filter=U", "-z"}).split('\0'))
        if (!path.isEmpty()) result.insert(path);
    return result;
}

void expectUnchanged(const Repository &repo, const QMap<QString, QByteArray> &before, bool ignoreObjects = false)
{
    const auto after = repo.snapshot();
    QStringList changed;
    const auto skip = [&](const QString &path) { return ignoreObjects && path.startsWith(QStringLiteral(".git/objects/")); };
    for (auto it = after.cbegin(); it != after.cend(); ++it)
        if (!skip(it.key()) && (!before.contains(it.key()) || before.value(it.key()) != it.value())) changed << it.key();
    for (auto it = before.cbegin(); it != before.cend(); ++it)
        if (!skip(it.key()) && !after.contains(it.key())) changed << ("missing " + it.key());
    if (!changed.isEmpty()) throw std::runtime_error(changed.join(", ").toStdString());
    CHECK(changed.isEmpty());
}

struct Session {
    Repository repo;
    ScopedEnvironment environment;
    ApplicationController controller;
    QString id;
    explicit Session() : environment(repo.environment)
    {
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
    bool waitForExchange()
    {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 20000) {
            pollfd input {};
            input.fd = ready[0];
            input.events = POLLIN;
            if (::poll(&input, 1, 0) > 0 && (input.revents & POLLIN) != 0) return true;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
        }
        return false;
    }
    void releaseWorker()
    {
        const char ack = 1;
        ssize_t sent = 0;
        do { sent = ::send(release[1], &ack, 1, MSG_NOSIGNAL); } while (sent < 0 && errno == EINTR);
        CHECK(sent == 1);
    }
private:
    int ready[2] = {-1, -1};
    int release[2] = {-1, -1};
};

void publishWhileDraftChanges(ApplicationController &controller, const QString &id, const std::string &nextText)
{
    PublishStall stall;
    const auto launched = revisionOf(openedFile(controller));
    CHECK(controller.markResolved(id, launched, true));
    CHECK(stall.waitForExchange());
    auto draft = openedFile(controller).current().draft;
    draft.text = nextText;
    CHECK(controller.files()->replaceDraft(id.toStdString(), openedFile(controller).current().id, draft) == Transition::Applied);
    CHECK(revisionOf(openedFile(controller)) != launched);
    stall.releaseWorker();
    waitFor(controller, [&] { return !controller.resolutionBusy(); });
}

struct RestartedWrite {
    Repository repo;
    ScopedEnvironment environment;
    std::unique_ptr<ApplicationController> controller;
    QString id;
    QByteArray edited;
    RestartedWrite()
        : environment(repo.environment), id(QString::fromLatin1(QByteArray("tracked.txt").toHex())),
          edited("edited in app\n")
    {
        const auto hook = repo.path(".git/hooks/post-index-change");
        {
            ApplicationController first;
            openController(first, repo);
            CHECK(first.selectFile(id));
            auto draft = openedFile(first).current().draft;
            draft.text = edited.toStdString();
            draft.unresolvedMarkers = 0;
            CHECK(first.files()->replaceDraft(id.toStdString(), openedFile(first).current().id, draft) == Transition::Applied);
            writeFile(hook, "#!/bin/sh\nexit 1\n");
            CHECK(QFile::setPermissions(hook, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
            CHECK(first.markResolved(id, revisionOf(openedFile(first)), true));
            waitFor(first, [&] {
                const auto &file = openedFile(first);
                return !first.resolutionBusy() && file.writeOutcome()
                    && file.writeOutcome()->outcome == Outcome::Succeeded && file.stageOutcome();
            });
            CHECK(openedFile(first).stageOutcome()->outcome != Outcome::Succeeded);
            CHECK(!openedFile(first).facts().resolved);
            CHECK(readFile(repo.path("tracked.txt")) == edited);
        }
        CHECK(QFile::remove(hook));
        controller = std::make_unique<ApplicationController>();
        openController(*controller, repo);
        CHECK(controller->selectFile(id));
        const auto &restored = openedFile(*controller);
        CHECK(restored.written() && restored.writeOutcome()
            && restored.writeOutcome()->outcome == Outcome::Succeeded);
        CHECK(!restored.facts().resolved);
        CHECK(readFile(repo.path("tracked.txt")) == edited);
    }
};
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("branch-truce"));
    QCoreApplication::setApplicationName(QStringLiteral("branch-truce-resolution-tests"));
    test_support::Suite suite("resolution");
    suite.run("confirmation, cancellation and a stale revision leave the repository unpublished", [] {
        Session session;
        const auto before = session.repo.snapshot();
        const auto text = openedFile(session.controller).current().draft.text;
        CHECK(!session.controller.markResolved(session.id, session.revision(), false));
        CHECK(session.controller.resolutionConfirmation());
        CHECK(session.controller.commandMessage().contains(QStringLiteral("intentional")));
        CHECK(!openedFile(session.controller).reviewed());
        expectUnchanged(session.repo, before);
        session.controller.cancelResolution();
        CHECK(!session.controller.resolutionConfirmation());
        CHECK(openedFile(session.controller).current().draft.text == text);
        const auto stale = session.revision();
        auto edited = openedFile(session.controller).current().draft;
        edited.text += "later edit\n";
        CHECK(session.controller.files()->replaceDraft(session.id.toStdString(), openedFile(session.controller).current().id, edited)
            == Transition::Applied);
        CHECK(!session.controller.markResolved(session.id, stale, true));
        CHECK(session.controller.commandMessage().contains(QStringLiteral("changed")));
        CHECK(readFile(session.repo.path("tracked.txt")) == session.repo.manual);
        expectUnchanged(session.repo, before);
        CHECK(!openedFile(session.controller).facts().resolved);
    });
    suite.run("a failed review persist publishes nothing", [] {
        Session session;
        const auto before = session.repo.snapshot();
        recovery_faults::Failure fault(recovery_faults::Point::JournalSync);
        CHECK(!session.controller.markResolved(session.id, session.revision(), true));
        CHECK(recovery_faults::failures == 1);
        CHECK(session.controller.files()->hasPendingRecovery());
        CHECK(!openedFile(session.controller).reviewed());
        expectUnchanged(session.repo, before);
        CHECK(unmerged(session.repo).contains("tracked.txt"));
    });
    suite.run("a rejected repository write keeps the draft and the conflict", [] {
        Session session;
        CHECK(QFile::setPermissions(session.repo.path("tracked.txt"), QFileDevice::ReadOwner));
        const auto before = session.repo.snapshot();
        CHECK(session.controller.markResolved(session.id, session.revision(), true));
        waitFor(session.controller, [&] {
            return !session.controller.resolutionBusy() && openedFile(session.controller).writeOutcome()
                && openedFile(session.controller).writeOutcome()->outcome != Outcome::Succeeded;
        });
        const auto outcome = openedFile(session.controller).writeOutcome()->outcome;
        CHECK(outcome == Outcome::Failed || outcome == Outcome::Uncertain);
        CHECK(openedFile(session.controller).reviewed() && openedFile(session.controller).checkpoint());
        CHECK(!openedFile(session.controller).facts().resolved && !openedFile(session.controller).staged());
        CHECK(readFile(session.repo.path("tracked.txt")) == session.repo.manual);
        expectUnchanged(session.repo, before, true);
        CHECK(unmerged(session.repo).contains("tracked.txt") && unmerged(session.repo).contains("excluded.txt"));
        CHECK(session.repo.git({"rev-parse", ":notes.txt"}).trimmed() == session.repo.notesObject);
    });
    suite.run("an unsupported staging hook leaves the written file unresolved", [] {
        Session session;
        const auto hook = session.repo.path(".git/hooks/post-index-change");
        writeFile(hook, "#!/bin/sh\nexit 1\n");
        CHECK(QFile::setPermissions(hook, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const auto before = session.repo.snapshot();
        CHECK(session.controller.markResolved(session.id, session.revision(), true));
        waitFor(session.controller, [&] {
            const auto &file = openedFile(session.controller);
            return !session.controller.resolutionBusy() && file.writeOutcome()
                && file.writeOutcome()->outcome == Outcome::Succeeded && file.stageOutcome();
        });
        CHECK(openedFile(session.controller).stageOutcome()->outcome == Outcome::Failed
            || openedFile(session.controller).stageOutcome()->outcome == Outcome::Uncertain);
        CHECK(!openedFile(session.controller).facts().resolved);
        CHECK(openedFile(session.controller).facts().status == FileStatus::StageFailed
            || openedFile(session.controller).facts().status == FileStatus::OutcomeUncertain);
        CHECK(readFile(session.repo.path("tracked.txt")) == session.repo.manual);
        expectUnchanged(session.repo, before, true);
        CHECK(unmerged(session.repo).contains("tracked.txt"));
        CHECK(session.controller.commandMessage().contains(QStringLiteral("unresolved"))
            || session.controller.selectedFile().value("status").toString().contains(QStringLiteral("unresolved"))
            || session.controller.selectedFile().value("status").toString().contains(QStringLiteral("uncertain")));
    });
    suite.run("Mark resolved publishes one unsaved path and leaves the other conflict", [] {
        Session session;
        CHECK(!openedFile(session.controller).checkpoint());
        CHECK(session.controller.markResolved(session.id, session.revision(), true));
        waitFor(session.controller, [&] { return openedFile(session.controller).facts().resolved; });
        CHECK(openedFile(session.controller).checkpoint());
        CHECK(openedFile(session.controller).facts().status == FileStatus::Resolved);
        CHECK(session.controller.selectedFile().value("status").toString() == "Written and resolved");
        CHECK(session.repo.git({"show", ":tracked.txt"}) == session.repo.manual);
        CHECK(readFile(session.repo.path("tracked.txt")) == session.repo.manual);
        const auto remaining = unmerged(session.repo);
        CHECK(!remaining.contains("tracked.txt") && remaining.contains("excluded.txt") && !remaining.contains("notes.txt"));
        CHECK(session.repo.git({"rev-parse", ":notes.txt"}).trimmed() == session.repo.notesObject);
        CHECK(!session.controller.files()->canFinish());
        CHECK(!session.controller.parentStagingOffer(session.id));
    });
    suite.run("parent-owned staging waits for Git and resolves only after that index entry matches", [] {
        Session session;
        CHECK(session.controller.assignParentStaging("tracked.txt"));
        CHECK(session.controller.markResolved(session.id, session.revision(), true));
        waitFor(session.controller, [&] {
            return !session.controller.resolutionBusy() && session.controller.parentStagingOffer(session.id).has_value();
        });
        CHECK(openedFile(session.controller).writeOutcome()
            && openedFile(session.controller).writeOutcome()->outcome == Outcome::Succeeded);
        CHECK(!openedFile(session.controller).stageOutcome());
        CHECK(!openedFile(session.controller).facts().resolved);
        CHECK(readFile(session.repo.path("tracked.txt")) == session.repo.manual);
        CHECK(unmerged(session.repo).contains("tracked.txt") && unmerged(session.repo).contains("excluded.txt"));
        const auto offered = session.controller.parentStagingOffer(session.id);
        CHECK(offered && offered->path == "tracked.txt" && !offered->canonicalObjectId.isEmpty());
        CHECK(session.controller.parentStagingPath().endsWith(QStringLiteral("tracked.txt")));
        CHECK(session.controller.verifyParentAcknowledgment(session.id, session.revision()));
        waitFor(session.controller, [&] { return !session.controller.resolutionBusy(); });
        CHECK(!openedFile(session.controller).facts().resolved);
        CHECK(session.controller.parentStagingOffer(session.id));
        CHECK(unmerged(session.repo).contains("tracked.txt"));
        session.repo.git({"add", "--", "tracked.txt"});
        CHECK(!unmerged(session.repo).contains("tracked.txt"));
        CHECK(session.controller.verifyParentAcknowledgment(session.id, session.revision()));
        waitFor(session.controller, [&] { return openedFile(session.controller).facts().resolved; });
        CHECK(!session.controller.parentStagingOffer(session.id));
        CHECK(unmerged(session.repo).contains("excluded.txt"));
        CHECK(session.repo.git({"rev-parse", ":notes.txt"}).trimmed() == session.repo.notesObject);
        CHECK(session.repo.git({"show", ":tracked.txt"}) == session.repo.manual);
        CHECK(!session.controller.files()->canFinish());
    });
    suite.run("TRUCE-0061 a restarted retry keeps the successful write and resolves it", [] {
        RestartedWrite restarted;
        CHECK(restarted.controller->markResolved(restarted.id, revisionOf(openedFile(*restarted.controller)), true));
        waitFor(*restarted.controller, [&] { return openedFile(*restarted.controller).facts().resolved; });
        const auto &file = openedFile(*restarted.controller);
        CHECK(file.writeOutcome() && file.writeOutcome()->outcome == Outcome::Succeeded
            && file.writeOutcome()->revision == file.current().id);
        CHECK(file.facts().status == FileStatus::Resolved);
        CHECK(!restarted.controller->commandMessage().contains(QStringLiteral("no longer matches")));
        CHECK(restarted.repo.git({"show", ":tracked.txt"}) == restarted.edited);
        CHECK(readFile(restarted.repo.path("tracked.txt")) == restarted.edited);
        const auto remaining = unmerged(restarted.repo);
        CHECK(!remaining.contains("tracked.txt") && remaining.contains("excluded.txt"));
        CHECK(restarted.repo.git({"rev-parse", ":notes.txt"}).trimmed() == restarted.repo.notesObject);
    });
    suite.run("TRUCE-0061 a destination that differs from the written revision fails closed after restart", [] {
        RestartedWrite restarted;
        const auto tampered = QByteArray("tampered\n");
        writeFile(restarted.repo.path("tracked.txt"), tampered);
        CHECK(!restarted.controller->markResolved(restarted.id, revisionOf(openedFile(*restarted.controller)), true));
        CHECK(!restarted.controller->resolutionBusy());
        const auto &file = openedFile(*restarted.controller);
        CHECK(file.writeOutcome() && file.writeOutcome()->outcome == Outcome::Failed);
        CHECK(!file.facts().resolved);
        CHECK(restarted.controller->commandMessage().contains(QStringLiteral("written revision")));
        CHECK(readFile(restarted.repo.path("tracked.txt")) == tampered);
        const auto index = restarted.repo.git({"ls-files", "-s", "--", "tracked.txt"});
        CHECK(index.contains(" 1\ttracked.txt") && !index.contains(" 0\ttracked.txt"));
        CHECK(unmerged(restarted.repo).contains("tracked.txt") && unmerged(restarted.repo).contains("excluded.txt"));
        CHECK(restarted.repo.git({"rev-parse", ":notes.txt"}).trimmed() == restarted.repo.notesObject);
    });
    suite.run("TRUCE-0062 a stale publish still allows a later Mark resolved when the disk matches", [] {
        Session session;
        const auto next = std::string("later resolved revision\n");
        publishWhileDraftChanges(session.controller, session.id, next);
        const auto &written = openedFile(session.controller);
        CHECK(written.writeOutcome() && written.writeOutcome()->outcome == Outcome::Succeeded);
        CHECK(written.writeOutcome()->revision != written.current().id);
        CHECK(written.current().draft.text == next);
        CHECK(!written.facts().resolved);
        CHECK(readFile(session.repo.path("tracked.txt")) == session.repo.manual);
        CHECK(session.controller.commandMessage().contains(QStringLiteral("draft changed")));
        CHECK(unmerged(session.repo).contains("tracked.txt") && unmerged(session.repo).contains("excluded.txt"));
        CHECK(session.controller.markResolved(session.id, revisionOf(written), true));
        waitFor(session.controller, [&] { return openedFile(session.controller).facts().resolved; });
        const auto &resolved = openedFile(session.controller);
        CHECK(resolved.facts().status == FileStatus::Resolved);
        CHECK(readFile(session.repo.path("tracked.txt")) == QByteArray::fromStdString(next));
        CHECK(session.repo.git({"show", ":tracked.txt"}) == QByteArray::fromStdString(next));
        CHECK(!session.controller.commandMessage().contains(QStringLiteral("identity changed")));
        CHECK(session.controller.markResolved(session.id, revisionOf(resolved), true));
        CHECK(session.controller.commandMessage().contains(QStringLiteral("Written and resolved")));
        CHECK(!unmerged(session.repo).contains("tracked.txt") && unmerged(session.repo).contains("excluded.txt"));
        CHECK(session.repo.git({"rev-parse", ":notes.txt"}).trimmed() == session.repo.notesObject);
    });
    suite.run("TRUCE-0062 a destination that differs from the published bytes fails closed", [] {
        Session session;
        const auto next = std::string("later resolved revision\n");
        publishWhileDraftChanges(session.controller, session.id, next);
        const auto tampered = QByteArray("tampered after publish\n");
        writeFile(session.repo.path("tracked.txt"), tampered);
        CHECK(session.controller.markResolved(session.id, revisionOf(openedFile(session.controller)), true));
        waitFor(session.controller, [&] {
            const auto &file = openedFile(session.controller);
            return !session.controller.resolutionBusy() && file.writeOutcome()
                && file.writeOutcome()->outcome == Outcome::Failed && file.writeOutcome()->revision == file.current().id;
        });
        CHECK(!openedFile(session.controller).facts().resolved);
        CHECK(readFile(session.repo.path("tracked.txt")) == tampered);
        CHECK(session.controller.commandMessage().contains(QStringLiteral("changed")));
        CHECK(unmerged(session.repo).contains("tracked.txt") && unmerged(session.repo).contains("excluded.txt"));
        CHECK(session.repo.git({"rev-parse", ":notes.txt"}).trimmed() == session.repo.notesObject);
    });
    suite.run("TRUCE-0067 Mark resolved republishes after Git restores the mergetool backup", [] {
        Session session;
        const auto merged = session.repo.path("tracked.txt");
        const auto backup = session.repo.path("tracked_BACKUP_1.txt");
        const QByteArray reviewed("reviewed in app\n");
        auto draft = openedFile(session.controller).current().draft;
        draft.text = reviewed.toStdString();
        draft.unresolvedMarkers = 0;
        CHECK(session.controller.files()->replaceDraft(session.id.toStdString(), openedFile(session.controller).current().id, draft)
            == Transition::Applied);
        const auto revision = session.revision();
        // git mergetool moves MERGED to BACKUP and copies it back before the tool runs.
        CHECK(QFile::rename(merged, backup) && QFile::copy(backup, merged));
        const auto offered = [&] {
            CHECK(session.controller.assignParentStaging({}) && session.controller.assignParentStaging("tracked.txt"));
            CHECK(session.controller.markResolved(session.id, revision, true));
            waitFor(session.controller, [&] {
                return !session.controller.resolutionBusy() && session.controller.parentStagingOffer(session.id).has_value();
            });
        };
        const auto inode = [&] {
            struct stat entry {};
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
        CHECK(openedFile(session.controller).writeOutcome()
            && openedFile(session.controller).writeOutcome()->outcome == Outcome::Succeeded);
        CHECK(!openedFile(session.controller).facts().resolved && unmerged(session.repo).contains("tracked.txt"));
        const auto written = inode();
        offered();
        CHECK(inode() == written && readFile(merged) == reviewed);
        const QByteArray tampered("changed while Git was waiting\n");
        writeFile(merged, tampered);
        CHECK(session.controller.assignParentStaging({}) && session.controller.assignParentStaging("tracked.txt"));
        CHECK(session.controller.markResolved(session.id, revision, true));
        waitFor(session.controller, [&] {
            const auto &file = openedFile(session.controller);
            return !session.controller.resolutionBusy() && file.writeOutcome() && file.writeOutcome()->outcome == Outcome::Failed;
        });
        CHECK(!session.controller.parentStagingOffer(session.id));
        CHECK(!openedFile(session.controller).facts().resolved);
        CHECK(readFile(merged) == tampered);
        const auto index = session.repo.git({"ls-files", "-s", "--", "tracked.txt"});
        CHECK(index.contains(" 1\ttracked.txt") && !index.contains(" 0\ttracked.txt"));
        CHECK(session.repo.git({"rev-parse", ":notes.txt"}).trimmed() == session.repo.notesObject);
    });
    return suite.report();
}
