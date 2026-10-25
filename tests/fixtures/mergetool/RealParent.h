#pragma once

// Disposable repository driven by the installed `git mergetool` parent.
// Nothing here reads the user's Git configuration or repositories: HOME, the
// XDG directories and the global Git configuration live in a temporary
// directory, the system configuration is disabled, and PATH is /usr/bin:/bin.

#include "TestHarness.h"

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

inline void writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size());
}

inline QByteArray readFile(const QString &path)
{
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly));
    const auto bytes = file.readAll();
    CHECK(file.error() == QFileDevice::NoError);
    return bytes;
}

inline QString shellQuote(const QString &value)
{
    return QLatin1Char('\'') + QString(value).replace(QLatin1Char('\''), QStringLiteral("'\\''")) + QLatin1Char('\'');
}

// Runs the event loop until ready() holds or 25 s pass.
inline void pump(const std::function<bool()> &ready, const QString &detail)
{
    if (ready()) return;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QTimer pulse;
    pulse.setInterval(30);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&pulse, &QTimer::timeout, &loop, [&] {
        if (ready()) loop.quit();
    });
    timeout.start(25000);
    pulse.start();
    loop.exec();
    if (!ready()) throw std::runtime_error(detail.toStdString());
}

// Keeps the event loop running for a fixed time and checks that `holds` stays true throughout.
inline void holdFor(int milliseconds, const std::function<bool()> &holds, const QString &detail)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
        if (!holds()) throw std::runtime_error(detail.toStdString());
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

    ParentRepository(const QString &launcher, const QList<ConflictPath> &conflicts)
        : conflicts_(conflicts)
    {
        CHECK(directory_.isValid() && home_.isValid() && probe_.isValid());
        root = QFileInfo(directory_.path()).canonicalFilePath();
        probe = QFileInfo(probe_.path()).canonicalFilePath();
        globalConfig = home_.path() + QStringLiteral("/gitconfig");
        writeFile(globalConfig, {});
        environment.insert("PATH", "/usr/bin:/bin");
        environment.insert("HOME", home_.path());
        environment.insert("XDG_CONFIG_HOME", home_.path() + "/config");
        environment.insert("XDG_CACHE_HOME", home_.path() + "/cache");
        environment.insert("XDG_DATA_HOME", home_.path() + "/data");
        // The session socket lives here, so the in-process session and every launcher meet.
        CHECK(QDir(home_.path()).mkdir(QStringLiteral("run"))
              && QFile::setPermissions(home_.path() + QStringLiteral("/run"),
                                       QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        environment.insert("XDG_RUNTIME_DIR", home_.path() + "/run");
        environment.insert("LC_ALL", "C");
        environment.insert("GIT_CONFIG_NOSYSTEM", "1");
        environment.insert("GIT_CONFIG_GLOBAL", globalConfig);
        environment.insert("GIT_TERMINAL_PROMPT", "0");
        environment.insert("BRANCH_TRUCE_MERGETOOL_NO_SPAWN", "1");
        configureGlobal(launcher);
        createConflict();
    }

    QString path(const QByteArray &relative) const { return root + QLatin1Char('/') + QString::fromLatin1(relative); }

    QByteArray git(const QStringList &arguments, int expected = 0, const QByteArray &input = {}) const
    {
        QProcess process;
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(root);
        process.start("/usr/bin/git", QStringList{"--no-pager", "--literal-pathspecs"} + arguments);
        CHECK(process.waitForStarted(5000));
        if (!input.isEmpty()) process.write(input);
        process.closeWriteChannel();
        CHECK(process.waitForFinished(15000));
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != expected)
            throw std::runtime_error("fixture git " + arguments.join(' ').toStdString() + ": "
                + process.readAllStandardError().toStdString());
        return process.readAllStandardOutput();
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
            + QStringLiteral(" ] && [ $waited -lt 600 ]; do sleep 0.05; waited=$((waited + 1)); done; rm -f ") + shellQuote(releaseFile())
            + QStringLiteral("; exit $status");
        // Git word-splits the configured command before eval, so it carries no literal newline.
        const auto tamper = invoke + QStringLiteral("; status=$?; if [ $status -eq 0 ]; then printf '%s\\n' ")
            + shellQuote(QString::fromLatin1(TamperedBytes).chopped(1)) + QStringLiteral(" > \"$MERGED\"; fi; exit $status");
        global({"user.name", "Branch Truce Tests"});
        global({"user.email", "tests@example.invalid"});
        global({"core.autocrlf", "false"});
        global({"init.defaultBranch", "main"});
        global({"mergetool.keepBackup", "false"});
        global({"mergetool.keepTemporaries", "false"});
        for (const auto &[tool, command] : {std::pair{Tool, invoke}, std::pair{GatedTool, gated}, std::pair{TamperTool, tamper}}) {
            global({QStringLiteral("mergetool.%1.cmd").arg(QLatin1String(tool)), command});
            global({QStringLiteral("mergetool.%1.trustExitCode").arg(QLatin1String(tool)), QStringLiteral("true")});
        }
    }

    void global(const QStringList &pair) const
    {
        QProcess process;
        process.setProcessEnvironment(environment);
        process.start("/usr/bin/git", QStringList{"config", "--global"} + pair);
        CHECK(process.waitForStarted(5000) && process.waitForFinished(15000));
        CHECK(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0);
    }

    void createConflict()
    {
        git({"init", "--quiet"});
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
    QTemporaryDir directory_;
    QTemporaryDir home_;
    QTemporaryDir probe_;

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
            previous_.insert(name, qEnvironmentVariableIsSet(name)
                ? std::optional<QByteArray>(qgetenv(name)) : std::nullopt);
            qputenv(name, environment.value(QString::fromLatin1(name)).toUtf8());
        }
    }
    ~ScopedEnvironment()
    {
        for (auto entry = previous_.cbegin(); entry != previous_.cend(); ++entry) {
            if (entry.value()) qputenv(entry.key().constData(), *entry.value());
            else qunsetenv(entry.key().constData());
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
    process->start("/usr/bin/setsid", QStringList{"--wait", "/usr/bin/git", "--no-pager", "--literal-pathspecs",
        "mergetool", "--no-prompt", QStringLiteral("--tool=%1").arg(QLatin1String(tool)), "--"} + paths);
    CHECK(process->waitForStarted(5000));
    return process;
}

inline void waitParent(QProcess &process, int expected, const QString &detail)
{
    pump([&] { return process.state() == QProcess::NotRunning; }, detail + " | git mergetool still running");
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != expected)
        throw std::runtime_error("git mergetool exit " + std::to_string(process.exitCode()) + "\n"
            + process.readAllStandardError().toStdString() + process.readAllStandardOutput().toStdString());
}

}
