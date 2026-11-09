#include "TestHarness.h"
#include "app/AppPaths.h"
#include "app/Application.h"
#include "app/EngineText.h"
#include "app/Translations.h"
#include "fixtures/GitOnPath.h"
#include "fixtures/RecoveryFaults.h"
#include "mergetool/MergetoolProtocol.h"
#include <QCoreApplication>
#include <QDirIterator>
#include <QElapsedTimer>
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

extern "C" int __real_renameat2(int olddirfd, const char *oldpath, int newdirfd, const char *newpath,
                                unsigned int flags);
extern "C" int __wrap_renameat2(int olddirfd, const char *oldpath, int newdirfd, const char *newpath,
                                unsigned int flags)
{
    const int result = __real_renameat2(olddirfd, oldpath, newdirfd, newpath, flags);
    if (result != 0 || (flags & RENAME_EXCHANGE) == 0 || !publish_stall::arm.exchange(false)) return result;
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
        CHECK(directory.isValid() && executables.isValid() && runtime.isValid());
        CHECK(QFile::setPermissions(runtime.path(),
                                    QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        root = QFileInfo(directory.path()).canonicalFilePath();
        environment.insert("PATH", "/usr/bin:/bin");
        environment.insert("HOME", executables.path());
        environment.insert("XDG_CONFIG_HOME", executables.path() + "/config");
        environment.insert("XDG_CACHE_HOME", executables.path() + "/cache");
        environment.insert("XDG_DATA_HOME", executables.path() + "/data");
        environment.insert("XDG_RUNTIME_DIR", runtime.path());
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
    // Keep session sockets private even when TMPDIR exceeds the Unix socket path limit.
    QTemporaryDir runtime{QStringLiteral("/tmp/bt-run-XXXXXX")};
    QString root;
    QProcessEnvironment environment;
    QByteArray notesObject;
    const QByteArray manual = "resolved without a prior save\n";
};

class ScopedEnvironment {
public:
    explicit ScopedEnvironment(const QProcessEnvironment &environment)
    {
        for (const auto name : {"HOME", "PATH", "LC_ALL", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME",
                                "XDG_RUNTIME_DIR", "GIT_CONFIG_NOSYSTEM", "GIT_CONFIG_GLOBAL", "GIT_TERMINAL_PROMPT"}) {
            previous.insert(name,
                            qEnvironmentVariableIsSet(name) ? std::optional<QByteArray>(qgetenv(name)) : std::nullopt);
            qputenv(name, environment.value(QString::fromLatin1(name)).toUtf8());
        }
    }
    ~ScopedEnvironment()
    {
        for (auto entry = previous.cbegin(); entry != previous.cend(); ++entry) {
            if (entry.value())
                qputenv(entry.key().constData(), *entry.value());
            else
                qunsetenv(entry.key().constData());
        }
    }

private:
    QMap<QByteArray, std::optional<QByteArray>> previous;
};

void openController(Application &controller, const Repository &repo)
{
    const auto before = repo.snapshot();
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(controller.loader(), &SessionLoader::launchChanged, &loop, [&] {
        if (!controller.loader()->opening()) loop.quit();
    });
    CHECK(controller.loader()->openRepository(repo.root));
    timeout.start(15000);
    if (controller.loader()->opening()) loop.exec();
    CHECK(!controller.loader()->opening());
    if (controller.loader()->startupState() != "ready")
        throw std::runtime_error("controller startup: " + controller.loader()->detail().toStdString());
    CHECK(repo.snapshot() == before);
}

const FileSession &openedFile(Application &controller, const char *path = "tracked.txt")
{
    const auto *selected = controller.store()->file(QByteArray(path).toHex().toStdString());
    CHECK(selected != nullptr);
    return *selected;
}

QString selectTracked(Application &controller)
{
    const auto id = QString::fromLatin1(QByteArray("tracked.txt").toHex());
    CHECK(controller.selection()->select(id));
    return id;
}

QString revisionOf(const FileSession &file) { return QString::number(static_cast<qulonglong>(file.current().id)); }

void waitFor(Application &controller, const std::function<bool()> &ready)
{
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 25000) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    if (!ready())
        throw std::runtime_error((controller.feedback()->message() + " | " + controller.selectedFile()->status()
                                  + " | busy=" + QString::number(controller.commands()->resolutionBusy()))
                                     .toStdString());
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
    const auto skip = [&](const QString &path) {
        return ignoreObjects && path.startsWith(QStringLiteral(".git/objects/"));
    };
    for (auto it = after.cbegin(); it != after.cend(); ++it)
        if (!skip(it.key()) && (!before.contains(it.key()) || before.value(it.key()) != it.value()))
            changed << it.key();
    for (auto it = before.cbegin(); it != before.cend(); ++it)
        if (!skip(it.key()) && !after.contains(it.key())) changed << ("missing " + it.key());
    if (!changed.isEmpty()) throw std::runtime_error(changed.join(", ").toStdString());
    CHECK(changed.isEmpty());
}

struct Session {
    Repository repo;
    ScopedEnvironment environment;
    Application controller{Application::Config{gitOnPath(), newRecoveryDirectory(), {}}};
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
    bool waitForExchange()
    {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 20000) {
            pollfd input{};
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
                return !first.commands()->resolutionBusy() && file.writeOutcome()
                    && file.writeOutcome()->outcome == Outcome::Succeeded && file.stageOutcome();
            });
            CHECK(openedFile(first).stageOutcome()->outcome != Outcome::Succeeded);
            CHECK(!openedFile(first).facts().resolved);
            CHECK(readFile(repo.path("tracked.txt")) == edited);
        }
        CHECK(QFile::remove(hook));
        controller = std::make_unique<Application>(Application::Config{gitOnPath(), newRecoveryDirectory(), {}});
        openController(*controller, repo);
        CHECK(controller->selection()->select(id));
        const auto &restored = openedFile(*controller);
        CHECK(restored.written() && restored.writeOutcome() && restored.writeOutcome()->outcome == Outcome::Succeeded);
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
    test_support::Suite suite("resolution");
    suite.run("controller fixtures contain and remove their session sockets and restore the runtime environment", [] {
        const auto previous = qgetenv("XDG_RUNTIME_DIR");
        const auto wasSet = qEnvironmentVariableIsSet("XDG_RUNTIME_DIR");
        QString runtime;
        {
            Session session;
            runtime = session.repo.environment.value(QStringLiteral("XDG_RUNTIME_DIR"));
            CHECK(QDir::isAbsolutePath(runtime));
            CHECK(qEnvironmentVariable("XDG_RUNTIME_DIR") == runtime);
            CHECK(mergetoolSocketDirectory() == runtime + QStringLiteral("/branch-truce"));
            CHECK(QFileInfo::exists(mergetoolSocketDirectory()));
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
        CHECK(!openedFile(session.controller).reviewed());
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
        recovery_faults::Failure fault(recovery_faults::Point::JournalSync);
        CHECK(!session.controller.commands()->markResolved(session.id, session.revision(), true));
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
        CHECK(session.controller.commands()->markResolved(session.id, session.revision(), true));
        waitFor(session.controller, [&] {
            return !session.controller.commands()->resolutionBusy() && openedFile(session.controller).writeOutcome()
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
        CHECK(session.controller.commands()->markResolved(session.id, session.revision(), true));
        waitFor(session.controller, [&] {
            const auto &file = openedFile(session.controller);
            return !session.controller.commands()->resolutionBusy() && file.writeOutcome()
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
        CHECK(session.controller.feedback()->message().contains(QStringLiteral("unresolved"))
              || session.controller.selectedFile()->status().contains(QStringLiteral("unresolved"))
              || session.controller.selectedFile()->status().contains(QStringLiteral("uncertain")));
    });
    suite.run("Mark resolved publishes one unsaved path and leaves the other conflict", [] {
        Session session;
        CHECK(!openedFile(session.controller).checkpoint());
        CHECK(session.controller.commands()->markResolved(session.id, session.revision(), true));
        waitFor(session.controller, [&] { return openedFile(session.controller).facts().resolved; });
        CHECK(openedFile(session.controller).checkpoint());
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
        CHECK(openedFile(session.controller).writeOutcome()
              && openedFile(session.controller).writeOutcome()->outcome == Outcome::Succeeded);
        CHECK(!openedFile(session.controller).stageOutcome());
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
        CHECK(file.writeOutcome() && file.writeOutcome()->outcome == Outcome::Succeeded
              && file.writeOutcome()->revision == file.current().id);
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
        CHECK(file.writeOutcome() && file.writeOutcome()->outcome == Outcome::Failed);
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
        CHECK(written.writeOutcome() && written.writeOutcome()->outcome == Outcome::Succeeded);
        CHECK(written.writeOutcome()->revision != written.current().id);
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
            return !session.controller.commands()->resolutionBusy() && file.writeOutcome()
                && file.writeOutcome()->outcome == Outcome::Failed
                && file.writeOutcome()->revision == file.current().id;
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
        CHECK(openedFile(session.controller).writeOutcome()
              && openedFile(session.controller).writeOutcome()->outcome == Outcome::Succeeded);
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
            return !session.controller.commands()->resolutionBusy() && file.writeOutcome()
                && file.writeOutcome()->outcome == Outcome::Failed;
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
            return !session.controller.commands()->resolutionBusy() && file.writeOutcome()
                && file.writeOutcome()->outcome == Outcome::Succeeded && file.stageOutcome();
        });
        CHECK(openedFile(session.controller).stageOutcome()->outcome == Outcome::Failed);
        CHECK(readFile(merged) == reviewed && unmerged(session.repo).contains("tracked.txt"));
        // Staging would now succeed, so any git add would reach stage 0.
        CHECK(QFile::remove(hook));
        const QByteArray external("edited outside the app after the write\n");
        writeFile(merged, external);
        CHECK(session.controller.commands()->markResolved(session.id, revision, true));
        waitFor(session.controller, [&] {
            const auto &file = openedFile(session.controller);
            return !session.controller.commands()->resolutionBusy() && file.writeOutcome()
                && file.writeOutcome()->outcome == Outcome::Failed;
        });
        const auto &file = openedFile(session.controller);
        CHECK(file.writeOutcome()->revision == file.current().id && !file.facts().resolved);
        CHECK(session.controller.feedback()->message().contains(
            QStringLiteral("no longer matches the written revision")));
        CHECK(file.stageOutcome() && file.stageOutcome()->outcome == Outcome::Failed);
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
            recovery_faults::Failure fault(recovery_faults::Point::JournalSync);
            CHECK(
                controller.store()->replaceDraft(session.id.toStdString(), openedFile(controller).current().id, edited)
                == Transition::Blocked);
            CHECK(recovery_faults::failures == 1);
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
        CHECK(controller.completion()->finish());
        CHECK(end.count == 1 && end.clean);
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
        CHECK(!openedFile(session.controller).reviewed() && !openedFile(session.controller).facts().resolved);
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
            recovery_faults::Failure fault(recovery_faults::Point::JournalSync);
            CHECK(!session.controller.commands()->discardToStaged(session.id, draftRevision));
            CHECK(recovery_faults::failures == 1);
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
            recovery_faults::Failure fault(recovery_faults::Point::JournalSync);
            CHECK(controller.store()->replaceDraft(session.id.toStdString(), current, edited) != Transition::Applied);
            CHECK(recovery_faults::failures == 1);
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
