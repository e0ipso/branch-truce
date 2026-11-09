// Process control for GitAdapter: launching Git with a fixed argv and
// environment, reading its bounded output, and stopping its process group.
#include "GitAdapterState.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStringList>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <pwd.h>
#include <unistd.h>

namespace branch_truce::git {
namespace {
using detail::exactPath;
using detail::inspectionLimit;
using detail::inspectionMs;
using detail::killGraceMs;
using detail::outputLimit;
using detail::secureExecutable;

[[noreturn]] void failNativeLaunch(int fd, int error)
{
    // The parent distinguishes exec failure from a program that exits 127.
    // Only async-signal-safe calls are allowed after QProcess forks.
    while (::write(fd, &error, sizeof(error)) < 0 && errno == EINTR) {}
    ::_exit(127);
}
bool sameExecutable(const struct stat &a, const struct stat &b)
{
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_mode == b.st_mode && a.st_uid == b.st_uid
        && a.st_gid == b.st_gid && a.st_size == b.st_size && a.st_mtim.tv_sec == b.st_mtim.tv_sec
        && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec && a.st_ctim.tv_sec == b.st_ctim.tv_sec
        && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
QProcessEnvironment childEnvironment(const GitEnvironment &selected)
{
    QProcessEnvironment env;
    env.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    env.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    const auto set = [&env](const QString &name, const QString &value) {
        if (!value.isEmpty()) env.insert(name, value);
    };
    set(QStringLiteral("HOME"), selected.home);
    set(QStringLiteral("XDG_CONFIG_HOME"), selected.configHome);
    set(QStringLiteral("GIT_CONFIG_GLOBAL"), selected.globalConfig);
    set(QStringLiteral("GIT_CONFIG_SYSTEM"), selected.systemConfig);
    set(QStringLiteral("GIT_CONFIG_NOSYSTEM"), selected.noSystem);
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    env.insert(QStringLiteral("GCM_INTERACTIVE"), QStringLiteral("never"));
    env.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    env.insert(QStringLiteral("GIT_NO_REPLACE_OBJECTS"), QStringLiteral("1"));
    return env;
}
QList<QByteArray> prefix()
{
    return {"--no-pager", "--literal-pathspecs",     "-c", "core.fsmonitor=false", "-c", "core.untrackedCache=false",
            "-c",         "submodule.recurse=false", "-c", "protocol.allow=never", "-c", "credential.helper="};
}
QString quotedAlternate(const QString &path)
{
    // Git parses C-quoted alternate paths, including colons and line breaks.
    QByteArray quoted = "\"";
    for (const auto value : QFile::encodeName(path)) {
        const auto byte = static_cast<unsigned char>(value);
        if (byte == '"' || byte == '\\' || byte == ':' || byte < 0x20 || byte >= 0x7f) {
            quoted += '\\' + QByteArray::number(byte, 8).rightJustified(3, '0');
        } else
            quoted += value;
    }
    return QString::fromLatin1(quoted + '"');
}
} // namespace

GitEnvironment GitEnvironment::from(const QProcessEnvironment &environment)
{
    const auto path = [&environment](const QString &name) {
        const auto value = environment.value(name);
        return value.startsWith(QLatin1Char('/')) && exactPath(value) ? value : QString{};
    };
    GitEnvironment selected;
    selected.home = path(QStringLiteral("HOME"));
    if (selected.home.isEmpty()) {
        if (const auto *user = ::getpwuid(::geteuid()); user != nullptr)
            selected.home = QFile::decodeName(user->pw_dir);
    }
    selected.configHome = path(QStringLiteral("XDG_CONFIG_HOME"));
    selected.globalConfig = path(QStringLiteral("GIT_CONFIG_GLOBAL"));
    selected.systemConfig = path(QStringLiteral("GIT_CONFIG_SYSTEM"));
    selected.noSystem = environment.value(QStringLiteral("GIT_CONFIG_NOSYSTEM"));
    return selected;
}
GitEnvironment GitEnvironment::fromProcess() { return from(QProcessEnvironment::systemEnvironment()); }

bool detail::exactPath(const QString &path)
{
    return !path.contains(QChar::Null) && QFile::decodeName(QFile::encodeName(path)) == path;
}
bool detail::secureExecutable(const QString &path, struct stat &identity)
{
    if (::stat(QFile::encodeName(path).constData(), &identity) != 0 || !S_ISREG(identity.st_mode)
        || (identity.st_mode & (S_IWGRP | S_IWOTH)) != 0 || (identity.st_uid != 0 && identity.st_uid != ::geteuid())
        || ::access(QFile::encodeName(path).constData(), X_OK) != 0) {
        return false;
    }
    auto directory = QFileInfo(path).absolutePath();
    for (;;) {
        struct stat parent{};
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

GitAdapter::State::State(GitAdapter *owner, GitProgram selected)
    : q(owner), environment(std::move(selected.environment)), deadlineMs(selected.deadlineMs)
{
    if (QDir::isAbsolutePath(selected.executable) && exactPath(selected.executable)) {
        executable = QFileInfo(selected.executable).canonicalFilePath();
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
        if (!nativeLaunchFailed && phase == Phase::Operation && spec->flow == detail::Flow::Stage) {
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
            stop(Outcome::TimedOut, QStringLiteral("Git exceeded the %1 second deadline").arg(deadlineMs / 1000.0));
        }
    });
    QObject::connect(&escalation, &QTimer::timeout, q, [this] {
        signalGroup(SIGKILL);
        process.kill();
    });
}
GitAdapter::State::~State()
{
    QObject::disconnect(&process, nullptr, q, nullptr);
    // Git removes its lock files, such as the index.lock of an interrupted
    // `git add`, when SIGTERM stops it. SIGKILL follows only when it has not
    // exited after the grace period, which bounds this wait.
    if (process.state() != QProcess::NotRunning) {
        signalGroup(SIGTERM);
        process.terminate();
        if (!process.waitForFinished(killGraceMs)) {
            signalGroup(SIGKILL);
            process.kill();
            process.waitForFinished(killGraceMs);
        }
    }
    signalGroup(SIGKILL); // Descendants left in the private session.
    closeLaunchPipe();
}
void GitAdapter::State::signalGroup(int signal)
{
    if (pid > 0) {
        ::kill(-static_cast<pid_t>(pid), signal);
    }
}
void GitAdapter::State::closeLaunchPipe()
{
    for (auto &fd : launchPipe) {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
}
void GitAdapter::State::observeNativeLaunch()
{
    ::close(launchPipe[1]);
    launchPipe[1] = -1;
    int error = 0;
    ssize_t count;
    do {
        count = ::read(launchPipe[0], &error, sizeof(error));
    } while (count < 0 && errno == EINTR);
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
void GitAdapter::State::launch(Phase next, const QList<QByteArray> &arguments, bool configuration,
                               const QByteArray &input)
{
    // Each phase gets the full measured budget. Preflight must not shrink
    // the operation, canonical blob, or checkout read. Stdout and stderr
    // still share the budget of the phase they belong to.
    bytesRead = 0;
    phase = next;
    phaseOutput.clear();
    phaseInput = input;
    struct stat current{};
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
    auto env = childEnvironment(environment);
    if (temporaryObjects
        && (next == Phase::Operation || next == Phase::CanonicalSize || next == Phase::CanonicalObject
            || next == Phase::CanonicalCheckout)) {
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
    // The modifier is the only exec path: it never returns into Qt's own
    // execve, so Qt's argument, directory and environment settings stay unused.
    process.setProgram(executable);
    process.start();
}
void GitAdapter::State::stop(Outcome outcome, const QString &message)
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
void GitAdapter::State::inspectionFailed(const QString &message)
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
void GitAdapter::State::drain(bool error)
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
                inspectionFailed(QStringLiteral("Index reinspection exceeded %1 bytes").arg(inspectionLimit));
            } else {
                stop(Outcome::OutputLimit,
                     QStringLiteral("Git output exceeded %1 bytes; captured prefix is incomplete").arg(limit));
            }
        }
    }
}
void GitAdapter::State::inspectMutation()
{
    deadline.start(inspectionMs);
    stopping = false;
    bytesRead = 0; // A separate budget makes reinspection possible after overflow.
    launch(Phase::Inspection, detail::indexArguments(result.request.path));
}
} // namespace branch_truce::git
