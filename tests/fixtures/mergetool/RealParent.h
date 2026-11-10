#pragma once

// Disposable repository driven by the real `git mergetool` parent, built on the shared Git
// fixture. Nothing here reads the user's Git configuration or repositories: HOME, the XDG
// directories and the global Git configuration live in a temporary directory, the system
// configuration is disabled, and PATH is /usr/bin:/bin.

#include "TestHarness.h"
#include "fixtures/ConflictedRepository.h"
#include "fixtures/EventLoop.h"
#include "fixtures/Files.h"
#include "fixtures/ScopedEnvironment.h"
#include "fixtures/ScriptGate.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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

// One conflicted text path: base, ours (main) and theirs (side) bytes.
struct ConflictPath {
    QByteArray path;
    QByteArray base;
    QByteArray ours;
    QByteArray theirs;
    // Written over the conflict markers before the parent starts, so Git launches with these bytes.
    QByteArray launch;
};

class ParentRepository : public test_support::ConflictedRepository {
public:
    // The documented tool: assign Git's unexported variables, then run the launcher. The session
    // runs in the test process, so the launcher gets --no-window and never starts another one.
    static constexpr const char *Tool = "truce";
    // The same command, then the parent waits for release() before it stages or restores.
    static constexpr const char *GatedTool = "truce-gated";
    // The same command, then the parent overwrites MERGED with other bytes before Git stages it.
    static constexpr const char *TamperTool = "truce-tamper";
    static constexpr const char *TamperedBytes = "bytes the parent wrote after the launcher\n";

    ParentRepository(const QString &launcher, const QList<ConflictPath> &conflicts)
    {
        CHECK(probe_.isValid());
        probe = QFileInfo(probe_.path()).canonicalFilePath();
        globalConfig = home().globalConfig();
        releaseGate_ = std::make_unique<test_support::ScriptGate>(releaseFile());
        configureGlobal(launcher);
        QMap<QString, QByteArray> base{{"notes.txt", "notes stay staged\n"}};
        QMap<QString, QByteArray> ours;
        QMap<QString, QByteArray> theirs;
        for (const auto &conflict : conflicts) {
            base.insert(QString::fromLatin1(conflict.path), conflict.base);
            ours.insert(QString::fromLatin1(conflict.path), conflict.ours);
            theirs.insert(QString::fromLatin1(conflict.path), conflict.theirs);
        }
        commit(base, "base");
        ConflictedRepository::conflict(ours, theirs);
        for (const auto &conflict : conflicts) writeFile(path(QString::fromLatin1(conflict.path)), conflict.launch);
        notesObject = stagedObject("notes.txt");
    }

    // The full index listing, mode, object id and stage for every entry. It does not refresh or write the index.
    QByteArray index() const { return git({"ls-files", "--stage", "-z"}); }
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
    QString runtimeDirectory() const { return runtime.path(); }

    QString releaseFile() const { return probe + QStringLiteral("/release"); }
    // The gated tool appends "<MERGED> <launcher status>" here after each launcher exit.
    QString exitsFile() const { return probe + QStringLiteral("/exits"); }
    QByteArray exits() const { return QFile::exists(exitsFile()) ? readFile(exitsFile()) : QByteArray(); }
    // Lets one gated tool command finish, now or when it next waits.
    void release() const { releaseGate_->pass(); }

    QString probe;
    QString globalConfig;
    QByteArray notesObject;

private:
    void configureGlobal(const QString &launcher)
    {
        const auto invoke = QStringLiteral("MERGED=\"$MERGED\" BASE=\"$BASE\" LOCAL=\"$LOCAL\" REMOTE=\"$REMOTE\" ")
            + shellQuote(launcher) + QStringLiteral(" --mergetool --no-window");
        const auto gated = invoke + QStringLiteral("; status=$?; printf '%s %s\\n' \"$MERGED\" \"$status\" >> ")
            + shellQuote(exitsFile()) + QStringLiteral("; read -r line < ") + shellQuote(releaseFile())
            + QStringLiteral("; exit $status");
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

    QTemporaryDir probe_;
    // The FIFO the gated tool reads before it lets Git stage or restore.
    std::unique_ptr<test_support::ScriptGate> releaseGate_;
};

// Applies the fixture environment to this process, which hosts the application session.
using test_support::ScopedEnvironment;

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
