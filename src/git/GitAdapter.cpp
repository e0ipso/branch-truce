#include "GitAdapter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <csignal>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace branch_truce::git {
namespace {
// Conservative process guards, pending the measured limits work.
constexpr qsizetype outputLimit = 8 * 1024 * 1024;
constexpr qsizetype inspectionLimit = 1024 * 1024;
constexpr int deadlineMs = 30000;
constexpr int inspectionMs = 5000;
constexpr int killGraceMs = 200;
QMutex mutationMutex;
QSet<QString> mutatingGitDirectories;

bool exactPath(const QString &path)
{
    return !path.contains(QChar::Null) && QFile::decodeName(QFile::encodeName(path)) == path;
}
bool relativeFile(const QString &path)
{
    // Git consumes Linux paths. QDir also treats a leading ':' as an absolute
    // Qt resource path, but ':' is an ordinary filename byte for Git.
    if (path.isEmpty() || path.startsWith(QLatin1Char('/')) || !exactPath(path)) {
        return false;
    }
    for (const auto &part : path.split(QLatin1Char('/'))) {
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..")) {
            return false;
        }
    }
    return true;
}
bool objectId(const QByteArray &id)
{
    return (id.size() == 40 || id.size() == 64)
        && std::all_of(id.cbegin(), id.cend(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        });
}
bool falseValue(const QByteArray &value)
{
    const auto lower = value.toLower();
    return lower == "false" || lower == "no" || lower == "off" || lower == "0";
}
bool sameExecutable(const struct stat &a, const struct stat &b)
{
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_mode == b.st_mode
        && a.st_uid == b.st_uid && a.st_gid == b.st_gid && a.st_size == b.st_size
        && a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
        && a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
bool secureExecutable(const QString &path, struct stat &identity)
{
    if (::stat(QFile::encodeName(path).constData(), &identity) != 0
        || !S_ISREG(identity.st_mode) || (identity.st_mode & (S_IWGRP | S_IWOTH)) != 0
        || (identity.st_uid != 0 && identity.st_uid != ::geteuid())
        || ::access(QFile::encodeName(path).constData(), X_OK) != 0) {
        return false;
    }
    auto directory = QFileInfo(path).absolutePath();
    for (;;) {
        struct stat parent {};
        if (::stat(QFile::encodeName(directory).constData(), &parent) != 0
            || (parent.st_uid != 0 && parent.st_uid != ::geteuid())
            || ((parent.st_mode & (S_IWGRP | S_IWOTH)) != 0
                && !(parent.st_uid == 0 && (parent.st_mode & S_ISVTX) != 0))) {
            return false;
        }
        if (directory == QLatin1String("/")) {
            return true;
        }
        directory = QFileInfo(directory).absolutePath();
    }
}
QProcessEnvironment environment()
{
    QProcessEnvironment env;
    env.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    env.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    if (const auto *user = ::getpwuid(::geteuid()); user != nullptr) {
        const auto home = QFile::decodeName(user->pw_dir);
        env.insert(QStringLiteral("HOME"), home);
        env.insert(QStringLiteral("XDG_CONFIG_HOME"), home + QStringLiteral("/.config"));
    }
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    env.insert(QStringLiteral("GCM_INTERACTIVE"), QStringLiteral("never"));
    env.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    env.insert(QStringLiteral("GIT_NO_REPLACE_OBJECTS"), QStringLiteral("1"));
    return env;
}
QStringList prefix()
{
    return {QStringLiteral("--no-pager"), QStringLiteral("--literal-pathspecs"),
            QStringLiteral("-c"), QStringLiteral("core.fsmonitor=false"),
            QStringLiteral("-c"), QStringLiteral("core.untrackedCache=false"),
            QStringLiteral("-c"), QStringLiteral("submodule.recurse=false"),
            QStringLiteral("-c"), QStringLiteral("protocol.allow=never"),
            QStringLiteral("-c"), QStringLiteral("credential.helper=")};
}
} // namespace

class GitAdapter::State {
public:
    enum class Phase { Version, Configuration, GitDirectory, Hooks, Baseline, Operation, Inspection };
    explicit State(GitAdapter *owner, const QString &selected) : q(owner)
    {
        if (QDir::isAbsolutePath(selected) && exactPath(selected)) {
            executable = QFileInfo(selected).canonicalFilePath();
            executableValid = !executable.isEmpty() && secureExecutable(executable, identity);
        }
        process.setParent(q);
        deadline.setParent(q);
        escalation.setParent(q);
        process.setProcessChannelMode(QProcess::SeparateChannels);
        process.setChildProcessModifier([] {
            if (::setsid() == -1) {
                ::_exit(127);
            }
        });
        deadline.setSingleShot(true);
        escalation.setSingleShot(true);
        QObject::connect(&process, &QProcess::started, q, [this] {
            pid = process.processId();
            process.closeWriteChannel();
            if (phase == Phase::Operation && isMutation()) {
                mutationStarted = true;
                result.mutation = Mutation::Uncertain;
            }
            if (stopping) {
                signalGroup(SIGTERM);
            }
        });
        QObject::connect(&process, &QProcess::readyReadStandardOutput, q, [this] { drain(false); });
        QObject::connect(&process, &QProcess::readyReadStandardError, q, [this] { drain(true); });
        QObject::connect(&process, &QProcess::errorOccurred, q, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                escalation.stop();
                if (!stopping) {
                    result.outcome = Outcome::LaunchFailure;
                    result.diagnostic = process.errorString();
                }
                afterProcess(false, -1);
            } else if (error != QProcess::Crashed) {
                stop(Outcome::ProcessFailure, process.errorString());
            }
        });
        QObject::connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), q,
                         [this](int code, QProcess::ExitStatus status) {
            drain(false);
            drain(true);
            escalation.stop();
            signalGroup(SIGKILL); // Terminate descendants in our private session.
            pid = 0;
            afterProcess(status == QProcess::NormalExit, code);
        });
        QObject::connect(&deadline, &QTimer::timeout, q, [this] {
            if (phase == Phase::Inspection) {
                inspectionFailed(QStringLiteral("Index reinspection timed out"));
            } else {
                stop(Outcome::TimedOut, QStringLiteral("Git exceeded the 30 second deadline"));
            }
        });
        QObject::connect(&escalation, &QTimer::timeout, q, [this] {
            signalGroup(SIGKILL);
            process.kill();
        });
    }
    ~State()
    {
        QObject::disconnect(&process, nullptr, q, nullptr);
        signalGroup(SIGKILL);
        process.kill();
        releaseMutation();
    }
    bool isMutation() const { return result.request.operation == Operation::StageFile; }
    void signalGroup(int signal)
    {
        if (pid > 0) {
            ::kill(-static_cast<pid_t>(pid), signal);
        }
    }
    void releaseMutation()
    {
        if (!mutationKey.isEmpty()) {
            const QMutexLocker lock(&mutationMutex);
            mutatingGitDirectories.remove(mutationKey);
            mutationKey.clear();
        }
    }
    void complete()
    {
        deadline.stop();
        escalation.stop();
        releaseMutation();
        const auto completed = result;
        active = false;
        Q_EMIT q->finished(completed);
    }
    void reject(const QString &message)
    {
        result.outcome = Outcome::Rejected;
        result.diagnostic = message;
        complete();
    }
    void launch(Phase next, const QStringList &arguments, bool configuration = false)
    {
        phase = next;
        phaseOutput.clear();
        struct stat current {};
        if (!secureExecutable(executable, current) || !sameExecutable(identity, current)) {
            if (next == Phase::Inspection) {
                inspectionFailed(QStringLiteral("Selected executable changed before reinspection"));
                complete();
            } else {
                reject(QStringLiteral("Selected executable changed or is no longer trusted"));
            }
            return;
        }
        // Inspect effective configuration before overriding helpers for reads.
        auto argv = configuration ? QStringList{QStringLiteral("--no-pager")} : prefix();
        argv.append(arguments);
        process.setProgram(executable);
        process.setArguments(argv);
        process.setWorkingDirectory(directory);
        process.setProcessEnvironment(environment());
        process.start();
    }
    void stop(Outcome outcome, const QString &message)
    {
        if (stopping) {
            return;
        }
        stopping = true;
        result.outcome = outcome;
        result.diagnostic = message;
        signalGroup(SIGTERM);
        if (process.state() != QProcess::NotRunning) {
            process.terminate();
            escalation.start(killGraceMs);
        }
    }
    void inspectionFailed(const QString &message)
    {
        result.mutation = Mutation::Uncertain;
        if (result.outcome == Outcome::Success) {
            result.outcome = Outcome::InspectionFailure;
        }
        result.diagnostic += QStringLiteral("; ") + message;
        stopping = true;
        signalGroup(SIGTERM);
        process.terminate();
        if (process.state() != QProcess::NotRunning) {
            escalation.start(killGraceMs);
        }
    }
    void drain(bool error)
    {
        process.setReadChannel(error ? QProcess::StandardError : QProcess::StandardOutput);
        while (process.bytesAvailable() > 0) {
            const auto chunk = process.read(65536);
            if (chunk.isEmpty()) {
                break;
            }
            const auto limit = phase == Phase::Inspection ? inspectionLimit : outputLimit;
            const auto keep = std::min(chunk.size(), std::max(qsizetype{0}, limit - bytesRead));
            if (error) {
                result.standardError.append(chunk.constData(), keep);
            } else {
                phaseOutput.append(chunk.constData(), keep);
            }
            bytesRead += keep;
            if (keep != chunk.size()) {
                result.outputIncomplete = true;
                if (phase == Phase::Inspection) {
                    inspectionFailed(QStringLiteral("Index reinspection exceeded 1 MiB"));
                } else {
                    stop(Outcome::OutputLimit, QStringLiteral("Git output exceeded 8 MiB; captured prefix is incomplete"));
                }
            }
        }
    }
    bool checkConfiguration()
    {
        const auto operation = result.request.operation;
        const bool helpers = operation == Operation::Status || operation == Operation::MaterializeSource || isMutation();
        const bool objects = helpers || operation == Operation::ReadObject;
        for (const auto &entry : phaseOutput.split('\0')) {
            if (entry.isEmpty()) {
                continue;
            }
            const auto split = entry.indexOf('\n');
            const auto key = entry.left(split).toLower();
            const auto value = split < 0 ? QByteArray{} : entry.mid(split + 1);
            if ((objects && (key == "extensions.partialclone"
                             || (key.startsWith("remote.") && key.endsWith(".promisor") && !falseValue(value))))
                || (helpers && key.startsWith("filter.") && !value.isEmpty()
                    && (key.endsWith(".clean") || key.endsWith(".smudge") || key.endsWith(".process")))
                || (isMutation() && key == "core.fsmonitor" && !falseValue(value))) {
                result.diagnostic = QStringLiteral("Unsupported partial repository or external helper setting: ")
                    + QString::fromUtf8(key);
                return false;
            }
        }
        return true;
    }
    QStringList indexArguments() const
    {
        return {QStringLiteral("ls-files"), QStringLiteral("--stage"), QStringLiteral("-z"),
                QStringLiteral("--"), result.request.path};
    }
    QStringList operationArguments() const
    {
        const auto &r = result.request;
        switch (r.operation) {
        case Operation::DiscoverRepository:
            return {QStringLiteral("rev-parse"), QStringLiteral("--path-format=absolute"),
                    QStringLiteral("--show-toplevel"), QStringLiteral("--absolute-git-dir")};
        case Operation::ListUnmerged:
            return {QStringLiteral("ls-files"), QStringLiteral("--unmerged"), QStringLiteral("-z")};
        case Operation::Status:
            return {QStringLiteral("status"), QStringLiteral("--porcelain=v2"), QStringLiteral("-z"),
                    QStringLiteral("--untracked-files=no"), QStringLiteral("--ignore-submodules=all")};
        case Operation::InspectIndex:
            return indexArguments();
        case Operation::ReadObject:
            return {QStringLiteral("cat-file"), QStringLiteral("blob"), QString::fromLatin1(r.objectId)};
        case Operation::ReadConfiguration:
            return {QStringLiteral("config"), QStringLiteral("--null"), QStringLiteral("--list"), QStringLiteral("--includes")};
        case Operation::ReadAttributes:
            return {QStringLiteral("check-attr"), QStringLiteral("-z"), QStringLiteral("--all"), QStringLiteral("--"), r.path};
        case Operation::MaterializeSource:
            return {QStringLiteral("cat-file"), QStringLiteral("--filters"),
                    QStringLiteral("--path=") + r.path, QString::fromLatin1(r.objectId)};
        case Operation::StageFile:
            return {QStringLiteral("add"), QStringLiteral("--"), r.path};
        }
        return {};
    }
    void inspectMutation()
    {
        deadline.start(inspectionMs);
        stopping = false;
        bytesRead = 0; // A separate budget makes reinspection possible after overflow.
        launch(Phase::Inspection, indexArguments());
    }
    bool validIndex(const QByteArray &bytes, bool resolved) const
    {
        if (!bytes.isEmpty() && !bytes.endsWith('\0')) {
            return false;
        }
        int entries = 0;
        for (const auto &entry : bytes.split('\0')) {
            if (entry.isEmpty()) {
                continue;
            }
            ++entries;
            const auto tab = entry.indexOf('\t');
            const auto fields = entry.left(tab).split(' ');
            if (tab < 0 || fields.size() != 3 || !objectId(fields[1])
                || (fields[0] != "100644" && fields[0] != "100755")
                || fields[2].size() != 1 || fields[2][0] < '0' || fields[2][0] > '3'
                || (resolved && fields[2] != "0")
                || entry.mid(tab + 1) != QFile::encodeName(result.request.path)) {
                return false;
            }
        }
        return !resolved || entries == 1;
    }
    void afterProcess(bool normal, int code)
    {
        if (!active) {
            return;
        }
        if (phase == Phase::Inspection) {
            result.indexAfter = phaseOutput;
            if (!stopping && normal && code == 0 && validIndex(phaseOutput, false)) {
                result.mutation = result.indexBefore == result.indexAfter
                    ? Mutation::ObservedUnchanged : Mutation::ObservedChanged;
                if (result.outcome == Outcome::Success && !validIndex(phaseOutput, true)) {
                    result.outcome = Outcome::InspectionFailure;
                    result.diagnostic = QStringLiteral("Staging exited successfully without a single regular stage-zero entry");
                }
            } else {
                inspectionFailed(QStringLiteral("Index reinspection failed; mutation remains uncertain"));
            }
            complete();
            return;
        }
        // Keep failed preflight output as well as the requested operation's
        // output. Reinspection has its own fields and must not overwrite it.
        result.standardOutput = phaseOutput;
        if (phase == Phase::Operation) {
            result.exitCode = normal ? code : -1;
        }
        if (!stopping && (!normal || code != 0)) {
            if (result.outcome != Outcome::LaunchFailure) {
                result.outcome = normal ? Outcome::ExitFailure : Outcome::ProcessFailure;
                result.diagnostic = normal ? QStringLiteral("Git exited with code %1").arg(code)
                                           : QStringLiteral("Git process crashed");
            }
            stopping = true;
        }
        if (stopping) {
            if (mutationStarted) {
                inspectMutation();
            } else {
                complete();
            }
            return;
        }
        switch (phase) {
        case Phase::Version:
            if (!phaseOutput.startsWith("git version ")) {
                reject(QStringLiteral("Selected executable did not identify itself as Git"));
            } else {
                launch(Phase::Configuration, {QStringLiteral("config"), QStringLiteral("--null"),
                       QStringLiteral("--list"), QStringLiteral("--includes")}, true);
            }
            break;
        case Phase::Configuration:
            if (!checkConfiguration()) {
                reject(result.diagnostic);
            } else if (result.request.operation == Operation::ReadConfiguration) {
                result.standardOutput = phaseOutput;
                result.exitCode = 0;
                result.outcome = Outcome::Success;
                complete();
            } else if (isMutation()) {
                launch(Phase::GitDirectory, {QStringLiteral("rev-parse"), QStringLiteral("--absolute-git-dir")});
            } else {
                launch(Phase::Operation, operationArguments());
            }
            break;
        case Phase::GitDirectory: {
            // Remove Git's delimiter, preserving newlines in the directory name.
            if (!phaseOutput.endsWith('\n')) {
                reject(QStringLiteral("Invalid Git directory response"));
                break;
            }
            phaseOutput.chop(1);
            const auto key = QFileInfo(QFile::decodeName(phaseOutput)).canonicalFilePath();
            if (key.isEmpty()) {
                reject(QStringLiteral("Git directory is unavailable"));
                break;
            }
            {
                const QMutexLocker lock(&mutationMutex);
                if (mutatingGitDirectories.contains(key)) {
                    result.diagnostic = QStringLiteral("Another app-owned mutation is active for this worktree");
                } else {
                    mutationKey = key;
                    mutatingGitDirectories.insert(key);
                }
            }
            if (mutationKey.isEmpty()) {
                reject(result.diagnostic);
            } else {
                launch(Phase::Hooks, {QStringLiteral("rev-parse"), QStringLiteral("--path-format=absolute"),
                       QStringLiteral("--git-path"), QStringLiteral("hooks")});
            }
            break;
        }
        case Phase::Hooks: {
            if (!phaseOutput.endsWith('\n')) {
                reject(QStringLiteral("Invalid hooks directory response"));
                break;
            }
            phaseOutput.chop(1);
            const QFileInfo info(QFile::decodeName(phaseOutput));
            const QDir hooks(info.isAbsolute() ? info.filePath() : directory + QLatin1Char('/') + info.filePath());
            if (hooks.exists()) {
                if (!QFileInfo(hooks.path()).isReadable()) {
                    reject(QStringLiteral("Hooks directory cannot be inspected"));
                    break;
                }
                for (const auto &entry : hooks.entryInfoList(QDir::Files | QDir::Hidden | QDir::System)) {
                    if (entry.isExecutable() && !entry.fileName().endsWith(QLatin1String(".sample"))) {
                        reject(QStringLiteral("Executable repository hooks are unsupported"));
                        return;
                    }
                }
            } else if (info.exists() || info.isSymLink()) {
                reject(QStringLiteral("Hooks path is not an inspectable directory"));
                break;
            }
            launch(Phase::Baseline, indexArguments());
            break;
        }
        case Phase::Baseline:
            if (!validIndex(phaseOutput, false) || phaseOutput.size() > inspectionLimit) {
                reject(QStringLiteral("Unsupported or oversized index baseline"));
            } else {
                result.indexBefore = phaseOutput;
                launch(Phase::Operation, operationArguments());
            }
            break;
        case Phase::Operation:
            result.outcome = Outcome::Success;
            if (mutationStarted) {
                inspectMutation();
            } else {
                complete();
            }
            break;
        case Phase::Inspection:
            break;
        }
    }
    void begin()
    {
        const auto &r = result.request;
        directory = QFileInfo(r.directory).canonicalFilePath();
        if (!executableValid || directory.isEmpty() || !QDir(directory).exists()
            || !QDir::isAbsolutePath(r.directory) || !exactPath(r.directory)
            || executable.startsWith(directory + QLatin1Char('/'))) {
            reject(QStringLiteral("An absolute directory and trusted executable outside it are required"));
            return;
        }
        if (operationArguments().isEmpty()) {
            reject(QStringLiteral("Unknown Git operation"));
            return;
        }
        const bool pathRequired = r.operation == Operation::InspectIndex || r.operation == Operation::ReadAttributes
            || r.operation == Operation::MaterializeSource || isMutation();
        const bool objectRequired = r.operation == Operation::ReadObject || r.operation == Operation::MaterializeSource;
        if ((pathRequired && !relativeFile(r.path)) || (!pathRequired && !r.path.isEmpty())
            || (objectRequired && !objectId(r.objectId)) || (!objectRequired && !r.objectId.isEmpty())) {
            reject(QStringLiteral("Invalid operands for the named Git operation"));
            return;
        }
        if (isMutation()) {
            const QFileInfo file(directory + QLatin1Char('/') + r.path);
            if (!file.isFile() || file.isSymLink() || file.canonicalFilePath() != file.absoluteFilePath()) {
                reject(QStringLiteral("Staging requires one regular file without symlink traversal"));
                return;
            }
        }
        deadline.start(deadlineMs);
        launch(Phase::Version, {QStringLiteral("--version")});
    }
    GitAdapter *q;
    QProcess process;
    QTimer deadline;
    QTimer escalation;
    QString executable;
    QString directory;
    QString mutationKey;
    struct stat identity {};
    Result result;
    QByteArray phaseOutput;
    Phase phase = Phase::Version;
    qsizetype bytesRead = 0;
    qint64 pid = 0;
    bool executableValid = false;
    bool active = false;
    bool stopping = false;
    bool mutationStarted = false;
};
GitAdapter::GitAdapter(const QString &executable, QObject *parent)
    : QObject(parent), d(std::make_unique<State>(this, executable))
{
    qRegisterMetaType<Result>();
}
GitAdapter::~GitAdapter() = default;
bool GitAdapter::start(const Request &request)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (d->active) {
        return false;
    }
    d->result = Result{};
    d->result.request = request;
    d->bytesRead = 0;
    d->stopping = false;
    d->mutationStarted = false;
    d->active = true;
    QTimer::singleShot(0, this, [this] {
        if (d->stopping) {
            d->complete();
        } else {
            d->begin();
        }
    });
    return true;
}
void GitAdapter::cancel()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!d->active) {
        return;
    }
    if (d->phase == State::Phase::Inspection && d->mutationStarted) {
        // Preserve bounded reinspection even if cancellation arrives after exit.
        d->result.outcome = Outcome::Cancelled;
        d->result.diagnostic = QStringLiteral("Cancelled; reinspecting the index");
    } else {
        d->stop(Outcome::Cancelled, QStringLiteral("Git request cancelled"));
    }
}
bool GitAdapter::busy() const { return d->active; }
} // namespace branch_truce::git
