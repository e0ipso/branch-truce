#pragma once

// Disposable repository driven by the real `git mergetool` parent, built on the shared Git
// fixture. Nothing here reads the user's Git configuration or repositories: HOME, the XDG
// directories and the global Git configuration live in a temporary directory, the system
// configuration is disabled, and PATH is /usr/bin:/bin.

#include "TestHarness.h"
#include "fixtures/EventLoop.h"
#include "fixtures/Files.h"
#include "fixtures/GitRepository.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTimer>

#include <csignal>
#include <sys/types.h>
#include <unistd.h>

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace mergetool_fixture {

using test_support::readFile;
using test_support::shellQuote;
using test_support::writeFile;

// Runs the event loop until ready() holds or 25 s pass. A ready() that fails a CHECK stops the
// wait, and the failure is raised here rather than inside the event loop.
inline void pump(const std::function<bool()> &ready, const QString &detail)
{
    test_support::waitUntil(ready, detail.toStdString(), test_support::kWaitDeadlineMs, 30);
}

// Keeps the event loop running for a fixed time and checks that `holds` stays true throughout.
inline void holdFor(int milliseconds, const std::function<bool()> &holds, const QString &detail)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
        test_support::throwPending();
        if (!holds()) throw test_support::Failure(detail.toStdString());
    }
}

// One conflicted text path: base, ours (main) and theirs (side) bytes.
struct ConflictPath {
    QByteArray path;
    QByteArray base;
    QByteArray ours;
    QByteArray theirs;
    // Written over the conflict markers before the parent starts, so Git launches with these bytes.
    QByteArray launch;
};

class ParentRepository {
public:
    // The documented tool: assign Git's unexported variables, then run the launcher.
    static constexpr const char *Tool = "truce";
    // The same command, then the parent waits for a release file before it stages or restores.
    static constexpr const char *GatedTool = "truce-gated";
    // The same command, then the parent overwrites MERGED with other bytes before Git stages it.
    static constexpr const char *TamperTool = "truce-tamper";
    static constexpr const char *TamperedBytes = "bytes the parent wrote after the launcher\n";

    ParentRepository(const QString &launcher, const QList<ConflictPath> &conflicts) : conflicts_(conflicts)
    {
        CHECK(probe_.isValid() && runtime_.isValid());
        root = repository_.root();
        probe = QFileInfo(probe_.path()).canonicalFilePath();
        globalConfig = repository_.home().globalConfig();
        // The shared Git sandbox sets PATH, LC_ALL, HOME, XDG_CONFIG_HOME and the Git configuration.
        environment = repository_.environment();
        environment.insert("XDG_CACHE_HOME", repository_.home().path() + "/cache");
        environment.insert("XDG_DATA_HOME", repository_.home().path() + "/data");
        // The session socket lives here, so the in-process session and every launcher meet.
        CHECK(QFile::setPermissions(runtime_.path(),
                                    QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        environment.insert("XDG_RUNTIME_DIR", runtime_.path());
        environment.insert("BRANCH_TRUCE_MERGETOOL_NO_SPAWN", "1");
        configureGlobal(launcher);
        createConflict();
    }

    QString path(const QByteArray &relative) const { return root + QLatin1Char('/') + QString::fromLatin1(relative); }

    QByteArray git(const QStringList &arguments, int expected = 0, const QByteArray &input = {}) const
    {
        return test_support::runGit(environment, root, arguments, input, expected);
    }

    // The full index listing, mode, object id and stage for every entry. It does not refresh or write the index.
    QByteArray index() const { return git({"ls-files", "--stage", "-z"}); }
    bool unmerged(const QByteArray &relative) const
    {
        return !git({"ls-files", "--unmerged", "-z", "--", QString::fromLatin1(relative)}).isEmpty();
    }
    QByteArray stagedObject(const QByteArray &relative) const
    {
        return git({"rev-parse", "--verify", "--quiet", QString::fromLatin1(":0:" + relative)}).trimmed();
    }
    QByteArray objectOf(const QByteArray &bytes) const { return git({"hash-object", "--stdin"}, 0, bytes).trimmed(); }

    // Git 2.43 writes LOCAL, REMOTE, BASE and BACKUP next to MERGED as <stem>_<KIND>_<pid><ext>.
    QStringList gitTemporaries(const QByteArray &relative) const
    {
        const QFileInfo info(QString::fromLatin1(relative));
        QStringList found;
        const auto names = QDir(root).entryList({info.completeBaseName() + QStringLiteral("_*_*.") + info.suffix()},
                                                QDir::Files | QDir::Hidden);
        for (const auto &name : names) found.append(root + QLatin1Char('/') + name);
        return found;
    }

    // The private XDG_RUNTIME_DIR; the session's socket directory belongs directly inside it.
    QString runtimeDirectory() const { return runtime_.path(); }

    QString releaseFile() const { return probe + QStringLiteral("/release"); }
    // The gated tool appends "<MERGED> <launcher status>" here after each launcher exit.
    QString exitsFile() const { return probe + QStringLiteral("/exits"); }
    QByteArray exits() const { return QFile::exists(exitsFile()) ? readFile(exitsFile()) : QByteArray(); }
    void release() const { writeFile(releaseFile(), "go\n"); }

    QString root;
    QString probe;
    QString globalConfig;
    QProcessEnvironment environment;

private:
    void configureGlobal(const QString &launcher)
    {
        const auto invoke = QStringLiteral("MERGED=\"$MERGED\" BASE=\"$BASE\" LOCAL=\"$LOCAL\" REMOTE=\"$REMOTE\" ")
            + shellQuote(launcher) + QStringLiteral(" --mergetool");
        const auto gated = invoke + QStringLiteral("; status=$?; printf '%s %s\\n' \"$MERGED\" \"$status\" >> ")
            + shellQuote(exitsFile()) + QStringLiteral("; waited=0; while [ ! -e ") + shellQuote(releaseFile())
            + QStringLiteral(" ] && [ $waited -lt 600 ]; do sleep 0.05; waited=$((waited + 1)); done; rm -f ")
            + shellQuote(releaseFile()) + QStringLiteral("; exit $status");
        // Git word-splits the configured command before eval, so it carries no literal newline.
        const auto tamper = invoke + QStringLiteral("; status=$?; if [ $status -eq 0 ]; then printf '%s\\n' ")
            + shellQuote(QString::fromLatin1(TamperedBytes).chopped(1))
            + QStringLiteral(" > \"$MERGED\"; fi; exit $status");
        global({"user.name", "Branch Truce Tests"});
        global({"user.email", "tests@example.invalid"});
        global({"core.autocrlf", "false"});
        global({"init.defaultBranch", "main"});
        global({"mergetool.keepBackup", "false"});
        global({"mergetool.keepTemporaries", "false"});
        for (const auto &[tool, command] :
             {std::pair{Tool, invoke}, std::pair{GatedTool, gated}, std::pair{TamperTool, tamper}}) {
            global({QStringLiteral("mergetool.%1.cmd").arg(QLatin1String(tool)), command});
            global({QStringLiteral("mergetool.%1.trustExitCode").arg(QLatin1String(tool)), QStringLiteral("true")});
        }
    }

    void global(const QStringList &pair) const { git(QStringList{"config", "--global"} + pair); }

    void createConflict()
    {
        writeFile(path("notes.txt"), "notes stay staged\n");
        QStringList paths{"notes.txt"};
        for (const auto &conflict : conflicts_) {
            writeFile(path(conflict.path), conflict.base);
            paths.append(QString::fromLatin1(conflict.path));
        }
        git(QStringList{"add", "--"} + paths);
        git({"commit", "--quiet", "-m", "base"});
        git({"checkout", "--quiet", "-b", "side"});
        commitSide([](const ConflictPath &conflict) { return conflict.theirs; }, "side");
        git({"checkout", "--quiet", "main"});
        commitSide([](const ConflictPath &conflict) { return conflict.ours; }, "main");
        git({"merge", "--no-edit", "side"}, 1);
        for (const auto &conflict : conflicts_) writeFile(path(conflict.path), conflict.launch);
        notesObject = stagedObject("notes.txt");
    }

    void commitSide(const std::function<QByteArray(const ConflictPath &)> &bytes, const QString &message)
    {
        QStringList paths;
        for (const auto &conflict : conflicts_) {
            writeFile(path(conflict.path), bytes(conflict));
            paths.append(QString::fromLatin1(conflict.path));
        }
        git(QStringList{"add", "--"} + paths);
        git({"commit", "--quiet", "-m", message});
    }

    QList<ConflictPath> conflicts_;
    // The worktree, the private home and the global configuration fixture Git and the parent read.
    test_support::GitRepository repository_;
    QTemporaryDir probe_;
    // Under a long TMPDIR a QTemporaryDir would push the socket path past its
    // length limit, and the session would fall back to the shared
    // /tmp/branch-truce-<uid>. A short base under /tmp keeps it private.
    QTemporaryDir runtime_{QStringLiteral("/tmp/bt-run-XXXXXX")};

public:
    QByteArray notesObject;
};

// Applies the fixture environment to this process, which hosts the application session.
class ScopedEnvironment {
public:
    explicit ScopedEnvironment(const QProcessEnvironment &environment)
    {
        for (const auto name : {"HOME", "PATH", "LC_ALL", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME",
                                "XDG_RUNTIME_DIR", "GIT_CONFIG_NOSYSTEM", "GIT_CONFIG_GLOBAL", "GIT_TERMINAL_PROMPT"}) {
            previous_.insert(name,
                             qEnvironmentVariableIsSet(name) ? std::optional<QByteArray>(qgetenv(name)) : std::nullopt);
            qputenv(name, environment.value(QString::fromLatin1(name)).toUtf8());
        }
    }
    ~ScopedEnvironment()
    {
        for (auto entry = previous_.cbegin(); entry != previous_.cend(); ++entry) {
            if (entry.value())
                qputenv(entry.key().constData(), *entry.value());
            else
                qunsetenv(entry.key().constData());
        }
    }
    ScopedEnvironment(const ScopedEnvironment &) = delete;
    ScopedEnvironment &operator=(const ScopedEnvironment &) = delete;

private:
    QMap<QByteArray, std::optional<QByteArray>> previous_;
};

// `git mergetool` runs as the leader of its own session and process group, as
// under a terminal. Destroying it kills the whole group, so a failed case cannot
// leave the tool command's shell or launcher behind.
class ParentProcess : public QProcess {
public:
    ~ParentProcess() override
    {
        if (state() == QProcess::NotRunning) return;
        ::kill(-static_cast<pid_t>(processId()), SIGKILL);
        waitForFinished(5000);
    }
    // Delivers a signal to every process in the parent's group, like a terminal interrupt.
    void signalGroup(int signal) const
    {
        const auto pid = static_cast<pid_t>(processId());
        CHECK(::getpgid(pid) == pid);
        CHECK(::kill(-pid, signal) == 0);
    }
};

// `git mergetool --no-prompt --tool=<tool> -- <paths>` in the fixture repository.
inline std::unique_ptr<ParentProcess> startParent(const ParentRepository &repo, const char *tool,
                                                  const QStringList &paths)
{
    auto process = std::make_unique<ParentProcess>();
    process->setProcessEnvironment(repo.environment);
    process->setWorkingDirectory(repo.root);
    // setsid replaces itself with Git when, as here, its caller is not a group leader.
    process->start("/usr/bin/setsid",
                   QStringList{"--wait", test_support::gitExecutable(), "--no-pager", "--literal-pathspecs",
                               "mergetool", "--no-prompt", QStringLiteral("--tool=%1").arg(QLatin1String(tool)), "--"}
                       + paths);
    CHECK(process->waitForStarted(5000));
    return process;
}

inline void waitParent(QProcess &process, int expected, const QString &detail)
{
    pump([&] { return process.state() == QProcess::NotRunning; }, detail + " | git mergetool still running");
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != expected)
        throw std::runtime_error("git mergetool exit " + std::to_string(process.exitCode()) + "\n"
                                 + process.readAllStandardError().toStdString()
                                 + process.readAllStandardOutput().toStdString());
}

}
