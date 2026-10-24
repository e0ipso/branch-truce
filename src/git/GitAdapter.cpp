#include "GitAdapter.h"
#include "GitParsing.h"
#include "RepositoryDiscovery.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QStringList>
#include <QThread>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace branch_truce::git {
namespace {
using detail::objectId;
using detail::parseIndex;
using detail::relativeFile;
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
bool regularFile(const QString &directory, const QByteArray &path)
{
    // QFile's QString boundary cannot address arbitrary Linux filename bytes.
    // Inspect each raw component so a display conversion cannot select a decoy.
    QByteArray current = QFile::encodeName(directory);
    const auto parts = path.split('/');
    for (qsizetype i = 0; i < parts.size(); ++i) {
        current += '/' + parts[i];
        struct stat info {};
        if (::lstat(current.constData(), &info) != 0
            || (i + 1 == parts.size() ? !S_ISREG(info.st_mode) : !S_ISDIR(info.st_mode))) {
            return false;
        }
    }
    return true;
}
[[noreturn]] void failNativeLaunch(int fd, int error)
{
    // The parent distinguishes exec failure from a program that exits 127.
    // Only async-signal-safe calls are allowed after QProcess forks.
    while (::write(fd, &error, sizeof(error)) < 0 && errno == EINTR) {}
    ::_exit(127);
}
bool falseValue(const QByteArray &value)
{
    const auto lower = value.toLower();
    return lower == "false" || lower == "no" || lower == "off" || lower == "0";
}
std::optional<QString> responsePath(const QByteArray &output)
{
    if (!output.endsWith('\n')) return std::nullopt;
    const auto bytes = output.first(output.size() - 1);
    const auto path = QFile::decodeName(bytes);
    if (bytes.isEmpty() || bytes.contains('\0') || QFile::encodeName(path) != bytes
        || !QDir::isAbsolutePath(path)) return std::nullopt;
    return path;
}
bool localObjectStores(const QString &objects, QString &diagnostic)
{
    // Git 2.43 has no reliable no-lazy-fetch switch. Inspect the common object
    // store and its alternates without asking Git to open any object first.
    QStringList pending{objects};
    QSet<QString> seen;
    qsizetype alternateBytes = 0;
    while (!pending.isEmpty()) {
        const auto path = QFileInfo(pending.takeLast()).canonicalFilePath();
        if (path.isEmpty() || !QFileInfo(path).isDir()
            || ::access(QFile::encodeName(path).constData(), R_OK | X_OK) != 0) {
            diagnostic = QStringLiteral("Outside app: object storage cannot be inspected offline");
            return false;
        }
        if (seen.contains(path)) continue;
        seen.insert(path);
        if (seen.size() > 256) {
            diagnostic = QStringLiteral("Outside app: too many alternate object stores to inspect");
            return false;
        }
        const auto packPath = path + QStringLiteral("/pack");
        const QFileInfo packInfo(packPath);
        if (packInfo.exists() || packInfo.isSymLink()) {
            if (!packInfo.isDir() || ::access(QFile::encodeName(packPath).constData(), R_OK | X_OK) != 0) {
                diagnostic = QStringLiteral("Outside app: object packs cannot be inspected offline");
                return false;
            }
            if (!QDir(packPath).entryList({QStringLiteral("*.promisor")},
                                         QDir::Files | QDir::Hidden | QDir::System).isEmpty()) {
                diagnostic = QStringLiteral("Outside app: promisor object packs require unsupported lazy fetching");
                return false;
            }
        }
        QFile alternates(path + QStringLiteral("/info/alternates"));
        const QFileInfo alternateInfo(alternates.fileName());
        if (!alternateInfo.exists() && !alternateInfo.isSymLink()) continue;
        if (!alternates.open(QIODevice::ReadOnly)) {
            diagnostic = QStringLiteral("Outside app: alternate object stores cannot be inspected");
            return false;
        }
        const auto bytes = alternates.read(inspectionLimit + 1);
        alternateBytes += bytes.size();
        if (alternates.error() != QFileDevice::NoError || alternateBytes > inspectionLimit) {
            diagnostic = QStringLiteral("Outside app: alternate object store list is unreadable or oversized");
            return false;
        }
        for (const auto &line : bytes.split('\n')) {
            if (line.isEmpty() || line.startsWith('#')) continue;
            const auto decoded = QFile::decodeName(line);
            // Fail closed on Git's quoted alternate syntax rather than inspect
            // a different directory. Ordinary relative/absolute alternates work.
            if (line.startsWith('"') || line.contains('\0') || QFile::encodeName(decoded) != line) {
                diagnostic = QStringLiteral("Outside app: alternate object path cannot be inspected losslessly");
                return false;
            }
            pending.append(QDir::isAbsolutePath(decoded) ? decoded : path + QLatin1Char('/') + decoded);
        }
    }
    return true;
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
QList<QByteArray> prefix()
{
    return {"--no-pager", "--literal-pathspecs", "-c", "core.fsmonitor=false",
            "-c", "core.untrackedCache=false", "-c", "submodule.recurse=false",
            "-c", "protocol.allow=never", "-c", "credential.helper="};
}
QString quotedAlternate(const QString &path)
{
    // Git parses C-quoted alternate paths, including colons and line breaks.
    QByteArray quoted = "\"";
    for (const auto value : QFile::encodeName(path)) {
        const auto byte = static_cast<unsigned char>(value);
        if (byte == '"' || byte == '\\' || byte == ':' || byte < 0x20 || byte >= 0x7f) {
            quoted += '\\' + QByteArray::number(byte, 8).rightJustified(3, '0');
        } else quoted += value;
    }
    return QString::fromLatin1(quoted + '"');
}
} // namespace

QString displayPath(const QByteArray &path)
{
    const auto decoded = QString::fromUtf8(path);
    const bool utf8 = decoded.toUtf8() == path;
    QByteArray label;
    for (const char value : path) {
        const auto byte = static_cast<unsigned char>(value);
        if (byte == '\\') label += "\\\\";
        else if (byte == '\n') label += "\\n";
        else if (byte == '\r') label += "\\r";
        else if (byte == '\t') label += "\\t";
        else if (byte < 0x20 || byte == 0x7f || (!utf8 && byte >= 0x80)) {
            label += "\\x" + QByteArray(1, value).toHex();
        } else label += value;
    }
    return QString::fromUtf8(label);
}

class GitAdapter::State {
public:
    enum class Phase { Version, Configuration, PolicyRoot, ObjectDirectory, TrackedPaths, Attributes,
                       GitDirectory, Hooks, Baseline, Operation, Discovery, Inspection,
                       CanonicalObject, CanonicalCheckout };
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
        deadline.setSingleShot(true);
        escalation.setSingleShot(true);
        QObject::connect(&process, &QProcess::started, q, [this] {
            pid = process.processId();
            observeNativeLaunch();
            if (!nativeLaunchFailed && !phaseInput.isEmpty() && process.write(phaseInput) != phaseInput.size()) {
                stop(Outcome::ProcessFailure, QStringLiteral("Could not send Git operation input"));
            }
            process.closeWriteChannel();
            if (!nativeLaunchFailed && phase == Phase::Operation && isMutation()) {
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
        closeLaunchPipe();
        releaseMutation();
    }
    bool isMutation() const { return result.request.operation == Operation::StageFile; }
    bool stagingPolicy() const { return isMutation() || result.request.operation == Operation::CheckParentStaging; }
    bool objectPolicy() const
    {
        const auto operation = result.request.operation;
        return operation == Operation::DiscoverRepository
            || operation == Operation::ReadObject || operation == Operation::ReadAttributes
            || operation == Operation::ListUnmerged || operation == Operation::InspectIndex
            || operation == Operation::Status || operation == Operation::MaterializeSource
            || operation == Operation::CanonicalizeText || stagingPolicy();
    }
    bool filterPolicy() const
    {
        return result.request.operation == Operation::Status
            || result.request.operation == Operation::MaterializeSource
            || result.request.operation == Operation::CanonicalizeText || stagingPolicy();
    }
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
        if (temporaryObjects) {
            if (!temporaryObjects->remove()) {
                if (result.outcome == Outcome::Success) result.outcome = Outcome::ProcessFailure;
                result.diagnostic += QStringLiteral("; could not remove private conversion objects: ") + temporaryObjects->path();
            }
            temporaryObjects.reset();
        }
        if (result.request.operation == Operation::DiscoverRepository && result.outcome != Outcome::Success) {
            result.repository.operation = RepositoryOperation::Unsupported;
            result.repository.supported = false;
            if (result.repository.diagnostic.isEmpty()) result.repository.diagnostic = result.diagnostic;
        }
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
    void closeLaunchPipe()
    {
        for (auto &fd : launchPipe) {
            if (fd >= 0) ::close(fd);
            fd = -1;
        }
    }
    void observeNativeLaunch()
    {
        ::close(launchPipe[1]);
        launchPipe[1] = -1;
        int error = 0;
        ssize_t count;
        do { count = ::read(launchPipe[0], &error, sizeof(error)); } while (count < 0 && errno == EINTR);
        closeLaunchPipe();
        if (count == static_cast<ssize_t>(sizeof(error))) {
            nativeLaunchFailed = true;
            if (!stopping) {
                result.outcome = Outcome::LaunchFailure;
                result.diagnostic = QStringLiteral("Could not start Git: ") + QString::fromLocal8Bit(std::strerror(error));
            }
            stopping = true;
        }
    }
    void launch(Phase next, const QList<QByteArray> &arguments, bool configuration = false,
                const QByteArray &input = {})
    {
        // Bound each canonical representation independently of preflight and
        // the other read, while keeping stdout and stderr in the same budget.
        if (next == Phase::CanonicalObject || next == Phase::CanonicalCheckout) bytesRead = 0;
        phase = next;
        phaseOutput.clear();
        phaseInput = input;
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
        nativeArguments = {QFile::encodeName(executable)};
        nativeArguments.append(configuration ? QList<QByteArray>{"--no-pager"} : prefix());
        nativeArguments.append(arguments);
        auto env = environment();
        if (temporaryObjects && (next == Phase::Operation || next == Phase::CanonicalObject || next == Phase::CanonicalCheckout)) {
            env.insert(QStringLiteral("GIT_OBJECT_DIRECTORY"), temporaryObjects->path());
            env.insert(QStringLiteral("GIT_ALTERNATE_OBJECT_DIRECTORIES"), quotedAlternate(originalObjectDirectory));
        }
        nativeEnvironment.clear();
        for (const auto &entry : env.toStringList()) nativeEnvironment.append(entry.toUtf8());
        nativeArgv.clear();
        nativeEnvp.clear();
        for (auto &argument : nativeArguments) nativeArgv.push_back(argument.data());
        for (auto &entry : nativeEnvironment) nativeEnvp.push_back(entry.data());
        nativeArgv.push_back(nullptr);
        nativeEnvp.push_back(nullptr);
        nativeDirectory = QFile::encodeName(directory);
        nativeLaunchFailed = false;
        closeLaunchPipe();
        if (::pipe2(launchPipe, O_CLOEXEC | O_NONBLOCK) != 0) {
            result.outcome = Outcome::LaunchFailure;
            result.diagnostic = QStringLiteral("Could not create the Git launch error pipe");
            afterProcess(false, -1);
            return;
        }
        // QString arguments cannot round-trip non-UTF-8 filenames. Prepare all
        // byte arrays/pointers in the parent, then exec the fixed argv directly.
        // QProcess still owns channels, child lifetime, signals and deadlines.
        // The CLOEXEC error pipe also works on the Qt 6.6 development floor.
        const auto program = nativeArgv.front();
        const auto argv = nativeArgv.data();
        const auto envp = nativeEnvp.data();
        const auto cwd = nativeDirectory.constData();
        const int readFd = launchPipe[0];
        const int errorFd = launchPipe[1];
        process.setChildProcessModifier([program, argv, envp, cwd, readFd, errorFd] {
            ::close(readFd);
            if (::setsid() == -1 || ::chdir(cwd) != 0) failNativeLaunch(errorFd, errno);
            ::execve(program, argv, envp);
            failNativeLaunch(errorFd, errno);
        });
        process.setProgram(executable);
        process.setArguments({});
        process.setWorkingDirectory(directory);
        process.setProcessEnvironment(env);
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
            if (!error && phase == Phase::Attributes && result.request.operation == Operation::Status) {
                // Status policy can cover the entire index. Consume records as
                // they arrive without charging them to the operation's output.
                if (!stopping && !consumeStatusAttributes(chunk)) stop(Outcome::Rejected, result.diagnostic);
                continue;
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
        configuration.clear();
        for (const auto &entry : phaseOutput.split('\0')) {
            if (entry.isEmpty()) {
                continue;
            }
            const auto split = entry.indexOf('\n');
            // Git normalizes section/key case, but driver/remote subsections
            // are case-sensitive. The last value is the effective setting.
            const auto key = entry.left(split);
            const auto value = split < 0 ? QByteArray{} : entry.mid(split + 1);
            configuration.insert(key, value);
        }
        for (auto entry = configuration.cbegin(); entry != configuration.cend(); ++entry) {
            const auto &key = entry.key();
            const auto &value = entry.value();
            if (key == "core.autocrlf" || key == "core.eol" || key == "core.safecrlf"
                || key == "core.checkroundtripencoding" || key == "core.filemode" || key == "core.attributesfile"
                || (key.startsWith("filter.") && (key.endsWith(".clean") || key.endsWith(".smudge")
                    || key.endsWith(".process") || key.endsWith(".required")))) {
                result.conversionPolicy.configuration.insert(key, value);
            }
            if ((objectPolicy() && (key == "extensions.partialclone"
                             || (key.startsWith("remote.") && key.endsWith(".promisor") && !falseValue(value))))
                || (stagingPolicy() && key == "core.fsmonitor" && !falseValue(value))) {
                result.diagnostic = QStringLiteral("Outside app: unsupported partial repository or filesystem monitor setting: ")
                    + QString::fromUtf8(key);
                return false;
            }
        }
        return true;
    }
    bool hasFilterConfiguration() const
    {
        for (auto entry = result.conversionPolicy.configuration.cbegin();
             entry != result.conversionPolicy.configuration.cend(); ++entry) {
            if (entry.key().startsWith("filter.")) return true;
        }
        return false;
    }
    bool checkFilter(const QByteArray &path, const QByteArray &driver)
    {
        // set/unset can be literal driver values, so a matching helper is denied.
        const auto prefix = "filter." + driver;
        if (!configuration.value(prefix + ".clean").isEmpty()
            || !configuration.value(prefix + ".smudge").isEmpty()
            || !configuration.value(prefix + ".process").isEmpty()
            || (configuration.contains(prefix + ".required") && !falseValue(configuration.value(prefix + ".required")))) {
            result.diagnostic = QStringLiteral("Outside app: custom filter '%1' is unsupported for %2")
                .arg(displayPath(driver), displayPath(path));
            return false;
        }
        return true;
    }
    bool consumeStatusAttributes(const QByteArray &chunk)
    {
        phaseOutput += chunk;
        qsizetype consumed = 0;
        for (;;) {
            const auto pathEnd = phaseOutput.indexOf('\0', consumed);
            if (pathEnd < 0) break;
            const auto nameEnd = phaseOutput.indexOf('\0', pathEnd + 1);
            if (nameEnd < 0) break;
            const auto valueEnd = phaseOutput.indexOf('\0', nameEnd + 1);
            if (valueEnd < 0) break;
            const auto path = phaseOutput.mid(consumed, pathEnd - consumed);
            const auto name = phaseOutput.mid(pathEnd + 1, nameEnd - pathEnd - 1);
            if (!relativeFile(path) || name.isEmpty()) {
                result.diagnostic = QStringLiteral("Outside app: invalid filter attribute inspection");
                return false;
            }
            if (name == "filter" && !checkFilter(path, phaseOutput.mid(nameEnd + 1, valueEnd - nameEnd - 1))) return false;
            consumed = valueEnd + 1;
        }
        phaseOutput.remove(0, consumed);
        // Bound an unfinished record, rather than the complete policy listing.
        if (phaseOutput.size() > inspectionLimit) {
            result.diagnostic = QStringLiteral("Outside app: oversized filter attribute record");
            return false;
        }
        return true;
    }
    bool captureAttributes()
    {
        const auto fields = phaseOutput.split('\0');
        if (fields.isEmpty() || !fields.back().isEmpty() || (fields.size() - 1) % 3 != 0) return false;
        result.conversionPolicy.attributes.clear();
        for (qsizetype i = 0; i + 2 < fields.size(); i += 3) {
            if (fields[i] != result.request.path || fields[i + 1].isEmpty()) return false;
            const auto &name = fields[i + 1];
            if (name == "text" || name == "eol" || name == "ident" || name == "working-tree-encoding" || name == "filter") {
                if (result.conversionPolicy.attributes.contains(name)) return false;
                result.conversionPolicy.attributes.insert(name, fields[i + 2]);
            }
        }
        return true;
    }
    bool checkFilters()
    {
        if (result.request.operation == Operation::Status) {
            if (phaseOutput.isEmpty()) return true;
            result.diagnostic = QStringLiteral("Outside app: invalid filter attribute inspection");
            return false;
        }
        if (!captureAttributes()) {
            result.diagnostic = QStringLiteral("Outside app: invalid conversion attribute inspection");
            return false;
        }
        const auto fields = phaseOutput.split('\0');
        if (fields.isEmpty() || !fields.back().isEmpty() || (fields.size() - 1) % 3 != 0) {
            result.diagnostic = QStringLiteral("Outside app: invalid filter attribute inspection");
            return false;
        }
        for (qsizetype i = 0; i + 2 < fields.size(); i += 3) {
            if (!relativeFile(fields[i]) || fields[i] != result.request.path) {
                result.diagnostic = QStringLiteral("Outside app: filter attributes do not match the requested path");
                return false;
            }
            if (fields[i + 1] != "filter") continue;
            if (!checkFilter(fields[i], fields[i + 2])) return false;
        }
        return true;
    }
    void afterPolicy()
    {
        if (result.request.operation == Operation::DiscoverRepository) {
            discovery = std::make_unique<detail::RepositoryDiscovery>(result, directory);
            launch(Phase::Discovery, discovery->arguments());
        } else if (isMutation()) {
            launch(Phase::GitDirectory, {"rev-parse", "--absolute-git-dir"});
        } else if (stagingPolicy()) {
            launch(Phase::Hooks, {"rev-parse", "--path-format=absolute", "--git-path", "hooks"});
        } else if (result.request.operation == Operation::CanonicalizeText) {
            temporaryObjects = std::make_unique<QTemporaryDir>(QStringLiteral("/tmp/branch-truce-conversion-XXXXXX"));
            if (!temporaryObjects->isValid()) {
                reject(QStringLiteral("Could not create private conversion object storage"));
                return;
            }
            launch(Phase::Operation, operationArguments(), false, result.request.input);
        } else {
            launch(Phase::Operation, operationArguments());
        }
    }
    QList<QByteArray> indexArguments() const
    {
        return {"ls-files", "--stage", "-z", "--", result.request.path};
    }
    QList<QByteArray> operationArguments() const
    {
        const auto &r = result.request;
        switch (r.operation) {
        case Operation::DiscoverRepository:
            return {"rev-parse", "--path-format=absolute", "--show-toplevel", "--absolute-git-dir"};
        case Operation::ListUnmerged:
            return {"ls-files", "--unmerged", "-z"};
        case Operation::Status:
            return {"status", "--porcelain=v2", "-z", "--untracked-files=no", "--ignore-submodules=all"};
        case Operation::InspectIndex:
            return indexArguments();
        case Operation::ReadObject:
            return {"cat-file", "blob", r.objectId};
        case Operation::ReadConfiguration:
            return {"config", "--null", "--list", "--includes"};
        case Operation::ReadAttributes:
            return {"check-attr", "-z", "--all", "--", r.path};
        case Operation::MaterializeSource:
            return {"cat-file", "--filters", "--path=" + r.path, r.objectId};
        case Operation::StageFile:
            return {"add", "--", r.path};
        case Operation::CheckParentStaging:
            return {"check-attr", "-z", "--all", "--", r.path};
        case Operation::CanonicalizeText:
            return {"hash-object", "-w", "--path=" + r.path, "--stdin"};
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
        const auto entries = parseIndex(bytes);
        if (!entries) return false;
        for (const auto &entry : *entries) {
            if ((entry.mode != "100644" && entry.mode != "100755")
                || (resolved && entry.stage != 0) || entry.path != result.request.path) {
                return false;
            }
        }
        return !resolved || entries->size() == 1;
    }
    void afterProcess(bool normal, int code)
    {
        if (!active) {
            return;
        }
        closeLaunchPipe();
        if (nativeLaunchFailed) normal = false;
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
        if (phase == Phase::Discovery && normal && !stopping) {
            result.exitCode = code;
            if (discovery->consume(phaseOutput, code)) {
                launch(Phase::Discovery, discovery->arguments());
            } else {
                complete();
            }
            return;
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
                launch(Phase::Configuration, {"config", "--null", "--list", "--includes"}, true);
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
            } else if (objectPolicy()) {
                launch(Phase::PolicyRoot, {"rev-parse", "--path-format=absolute", "--show-toplevel"});
            } else {
                afterPolicy();
            }
            break;
        case Phase::PolicyRoot: {
            const auto root = responsePath(phaseOutput);
            if (!root || !QFileInfo(*root).isDir()) {
                reject(QStringLiteral("Outside app: invalid worktree root response"));
                break;
            }
            const auto prefix = *root == QLatin1String("/") ? *root : *root + QLatin1Char('/');
            if (executable.startsWith(prefix)) {
                reject(QStringLiteral("The trusted Git executable must be outside the repository"));
                break;
            }
            // Request paths are repository-relative. Status also examines the
            // whole worktree even when Git is launched from a subdirectory.
            directory = *root;
            result.worktreeRoot = *root;
            if (isMutation() && !regularFile(directory, result.request.path)) {
                reject(QStringLiteral("Staging requires one regular file without symlink traversal"));
            } else {
                launch(Phase::ObjectDirectory, {"rev-parse", "--path-format=absolute", "--git-path", "objects"});
            }
            break;
        }
        case Phase::ObjectDirectory: {
            const auto objects = responsePath(phaseOutput);
            if (!objects) {
                reject(QStringLiteral("Outside app: invalid object directory response"));
            } else if (!localObjectStores(*objects, result.diagnostic)) {
                reject(result.diagnostic);
            } else {
                originalObjectDirectory = *objects;
                if (result.request.operation == Operation::Status) {
                    if (hasFilterConfiguration()) launch(Phase::TrackedPaths, {"ls-files", "--cached", "-z"});
                    else afterPolicy();
                } else if (filterPolicy()) {
                    launch(Phase::Attributes, {"check-attr", "-z", "--all", "--", result.request.path});
                } else {
                    afterPolicy();
                }
            }
            break;
        }
        case Phase::TrackedPaths:
            if (!phaseOutput.isEmpty() && !phaseOutput.endsWith('\0')) {
                reject(QStringLiteral("Outside app: invalid tracked path inspection"));
            } else if (phaseOutput.isEmpty()) {
                afterPolicy();
            } else {
                const auto paths = phaseOutput;
                // --all distinguishes an absent filter from filter=unspecified.
                launch(Phase::Attributes, {"check-attr", "-z", "--all", "--stdin"}, false, paths);
            }
            break;
        case Phase::Attributes:
            if (!checkFilters()) reject(result.diagnostic);
            else afterPolicy();
            break;
        case Phase::GitDirectory: {
            // Remove Git's delimiter, preserving newlines in the directory name.
            if (!phaseOutput.endsWith('\n')) {
                reject(QStringLiteral("Invalid Git directory response"));
                break;
            }
            phaseOutput.chop(1);
            const auto decoded = QFile::decodeName(phaseOutput);
            if (QFile::encodeName(decoded) != phaseOutput) {
                reject(QStringLiteral("Git directory cannot round-trip through Qt's directory API"));
                break;
            }
            const auto key = QFileInfo(decoded).canonicalFilePath();
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
                launch(Phase::Hooks, {"rev-parse", "--path-format=absolute", "--git-path", "hooks"});
            }
            break;
        }
        case Phase::Hooks: {
            if (!phaseOutput.endsWith('\n')) {
                reject(QStringLiteral("Outside app: invalid hooks directory response"));
                break;
            }
            phaseOutput.chop(1);
            const auto decoded = QFile::decodeName(phaseOutput);
            if (QFile::encodeName(decoded) != phaseOutput) {
                reject(QStringLiteral("Outside app: hooks directory cannot round-trip through Qt's directory API"));
                break;
            }
            const QFileInfo info(decoded);
            const QDir hooks(info.isAbsolute() ? info.filePath() : directory + QLatin1Char('/') + info.filePath());
            if (hooks.exists()) {
                if (!QFileInfo(hooks.path()).isReadable()) {
                    reject(QStringLiteral("Outside app: hooks directory cannot be inspected"));
                    break;
                }
                // git add and its mergetool parent run post-index-change, not
                // commit/checkout hooks. Inspect the effective core.hooksPath.
                const QFileInfo hook(hooks.filePath(QStringLiteral("post-index-change")));
                if (hook.isExecutable()) {
                    reject(QStringLiteral("Outside app: executable post-index-change hook is unsupported"));
                    return;
                }
            } else if ((info.exists() || info.isSymLink()) && info.canonicalFilePath() != QLatin1String("/dev/null")) {
                reject(QStringLiteral("Outside app: hooks path is not an inspectable directory"));
                break;
            }
            if (isMutation()) {
                launch(Phase::Baseline, indexArguments());
            } else {
                result.standardOutput.clear();
                result.outcome = Outcome::Success;
                result.exitCode = 0;
                complete();
            }
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
            if (result.request.operation == Operation::ReadAttributes && !captureAttributes()) {
                reject(QStringLiteral("Outside app: invalid conversion attribute inspection"));
                break;
            }
            if (result.request.operation == Operation::CanonicalizeText) {
                if (!phaseOutput.endsWith('\n') || !objectId(phaseOutput.first(phaseOutput.size() - 1))) {
                    reject(QStringLiteral("Invalid canonical object response"));
                } else {
                    result.canonicalObjectId = phaseOutput.first(phaseOutput.size() - 1);
                    launch(Phase::CanonicalObject, {"cat-file", "blob", result.canonicalObjectId});
                }
                break;
            }
            if (result.request.operation == Operation::ListUnmerged || result.request.operation == Operation::InspectIndex) {
                const auto entries = parseIndex(phaseOutput);
                if (!entries) {
                    result.outcome = Outcome::InspectionFailure;
                    result.diagnostic = QStringLiteral("Invalid NUL-delimited index records");
                } else {
                    result.indexEntries = *entries;
                }
            }
            if (mutationStarted) {
                inspectMutation();
            } else {
                complete();
            }
            break;
        case Phase::CanonicalObject:
            result.canonicalBytes = phaseOutput;
            launch(Phase::CanonicalCheckout, {"cat-file", "--filters", "--path=" + result.request.path, result.canonicalObjectId});
            break;
        case Phase::CanonicalCheckout:
            result.outcome = Outcome::Success;
            complete();
            break;
        case Phase::Inspection:
        case Phase::Discovery:
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
            || r.operation == Operation::MaterializeSource || r.operation == Operation::CanonicalizeText || stagingPolicy();
        const bool objectRequired = r.operation == Operation::ReadObject || r.operation == Operation::MaterializeSource;
        if ((pathRequired && !relativeFile(r.path)) || (!pathRequired && !r.path.isEmpty())
            || (objectRequired && !objectId(r.objectId)) || (!objectRequired && !r.objectId.isEmpty())
            || (r.operation != Operation::CanonicalizeText && !r.input.isEmpty()) || r.input.size() > outputLimit) {
            reject(QStringLiteral("Invalid operands for the named Git operation"));
            return;
        }
        deadline.start(deadlineMs);
        launch(Phase::Version, {"--version"});
    }
    GitAdapter *q;
    QProcess process;
    QTimer deadline;
    QTimer escalation;
    QString executable;
    QString directory;
    QString mutationKey;
    QString originalObjectDirectory;
    struct stat identity {};
    Result result;
    std::unique_ptr<detail::RepositoryDiscovery> discovery;
    std::unique_ptr<QTemporaryDir> temporaryObjects;
    QByteArray phaseOutput;
    QByteArray phaseInput;
    QHash<QByteArray, QByteArray> configuration;
    QList<QByteArray> nativeArguments;
    QList<QByteArray> nativeEnvironment;
    std::vector<char *> nativeArgv;
    std::vector<char *> nativeEnvp;
    QByteArray nativeDirectory;
    int launchPipe[2]{-1, -1};
    Phase phase = Phase::Version;
    qsizetype bytesRead = 0;
    qint64 pid = 0;
    bool executableValid = false;
    bool active = false;
    bool stopping = false;
    bool mutationStarted = false;
    bool nativeLaunchFailed = false;
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
    d->discovery.reset();
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
    d->result.cancellationRequested = true;
    if (d->phase == State::Phase::Inspection && d->mutationStarted) {
        // Keep the completed operation's outcome and diagnostic while bounded
        // reinspection determines the observed index mutation.
        return;
    }
    d->stop(Outcome::Cancelled, QStringLiteral("Git request cancelled"));
}
bool GitAdapter::busy() const { return d->active; }
} // namespace branch_truce::git
