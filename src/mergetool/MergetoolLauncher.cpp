// The Git mergetool launcher waits for the reviewed result to be handed back to its parent process.
#include "mergetool/MergetoolLauncher.h"
#include "mergetool/MergetoolFiles.h"
#include "mergetool/MergetoolIntake.h"
#include "mergetool/MergetoolMessages.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QProcess>
#include <QScopeGuard>
#include <QTextStream>
#include <QThread>
#include <QUuid>
#include <climits>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>

namespace branch_truce {
namespace {
// The translation context of the launcher's messages.
struct Text {
    Q_DECLARE_TR_FUNCTIONS(MergetoolLauncher)
};

int fail(const QString &message)
{
    QTextStream(stderr) << "mergetool: " << message << '\n';
    return 1;
}

// Reads one frame's payload. A negative waitMs waits for as long as the session stays connected.
std::optional<QByteArray> readPayload(QLocalSocket &socket, QByteArray &buffer, int waitMs)
{
    QElapsedTimer clock;
    clock.start();
    for (;;) {
        QByteArray payload;
        const auto frame = takeMergetoolFrame(&buffer, &payload);
        if (frame == MergetoolFrame::Ready) return payload;
        if (frame == MergetoolFrame::Invalid) return std::nullopt;
        if (socket.bytesAvailable() > 0) {
            buffer += socket.read(4 + kMergetoolFrameLimit - buffer.size());
            continue;
        }
        if (socket.state() != QLocalSocket::ConnectedState) return std::nullopt;
        const qint64 left = waitMs < 0 ? 1000 : waitMs - clock.elapsed();
        if (left <= 0) return std::nullopt;
        socket.waitForReadyRead(static_cast<int>(left));
    }
}

bool sendMessage(QLocalSocket &socket, const MergetoolMessage &message)
{
    const auto frame = mergetoolFrame(encodeMergetoolMessage(message));
    return !frame.isEmpty() && socket.write(frame) == frame.size() && socket.waitForBytesWritten(5000);
}

QString otherVersion(int version)
{
    return Text::tr("The Branch Truce window speaks mergetool protocol %1 and this launcher speaks %2. "
                    "Restart Branch Truce so the window and the launcher come from the same installation.")
        .arg(version)
        .arg(kMergetoolProtocolVersion);
}

// The session's reply or decision, or why there is none.
struct Reply {
    std::optional<MergetoolMessage> message;
    QString failure;
};

Reply readReply(QLocalSocket &socket, QByteArray &buffer, int waitMs)
{
    const auto payload = readPayload(socket, buffer, waitMs);
    if (!payload) return {};
    const auto decoded = decodeMergetoolMessage(*payload);
    if (decoded.failure == MergetoolDecodeFailure::OtherVersion) return {std::nullopt, otherVersion(decoded.version)};
    if (!decoded.message) return {std::nullopt, Text::tr("The session sent an invalid mergetool message.")};
    if (const auto *unsupported = std::get_if<UnsupportedMessage>(&*decoded.message))
        return {std::nullopt, otherVersion(unsupported->version)};
    return {decoded.message, {}};
}

// Why the session's answer to the attach does not let this launch wait, or empty.
QString attachRefusal(const Reply &reply, const QString &invocation)
{
    if (!reply.message)
        return reply.failure.isEmpty() ? Text::tr("The session did not answer the mergetool attach.") : reply.failure;
    if (const auto *rejected = std::get_if<RejectedMessage>(&*reply.message)) return rejected->reason;
    const auto *attached = std::get_if<AttachedMessage>(&*reply.message);
    if (!attached) return Text::tr("The session answered the attach with another kind of message.");
    if (attached->invocation != invocation) return Text::tr("The session answered a different mergetool launch.");
    return {};
}

// Git stages whatever MERGED holds once this launcher exits 0, so MERGED must
// still hold the canonical object the session wrote. Git converts the bytes
// the way it will when it stages them.
QString resolvedRefusal(const git::GitProgram &git, const QByteArray &mergedPath, const QByteArray &path,
                        const ResolvedMessage &resolved)
{
    QByteArray bytes;
    QString error;
    if (!readMergetoolFile(mergedPath, &bytes, &error)) return error;
    git::Request request;
    request.operation = git::Operation::CanonicalizeText;
    request.directory = QDir::currentPath();
    request.path = path;
    request.input = bytes;
    const auto canonical = git::runBlocking(git, request);
    if (canonical.outcome != git::Outcome::Success)
        return Text::tr("Git could not check the written result in MERGED, so it is not staged.");
    if (canonical.canonicalObjectId.toLower() != resolved.object)
        return Text::tr("MERGED no longer holds revision %1 that Branch Truce wrote, so it is not staged.")
            .arg(static_cast<qulonglong>(resolved.revision));
    return {};
}

// Once a package upgrade replaces or removes the running file, Linux names the
// running image "<path> (deleted)" in /proc/self/exe, and Qt returns that name
// from applicationFilePath(). Start whatever is installed at the original path
// instead, or fail at once when nothing is.
std::optional<QString> guiProgram(QString *error)
{
    QByteArray link(PATH_MAX, Qt::Uninitialized);
    const auto length = ::readlink("/proc/self/exe", link.data(), static_cast<size_t>(link.size()));
    if (length <= 0 || length >= link.size()) return QCoreApplication::applicationFilePath();
    link.truncate(length);
    // A file really named "... (deleted)" can still be the running image.
    struct stat running{};
    struct stat named{};
    if (::stat("/proc/self/exe", &running) == 0 && ::stat(link.constData(), &named) == 0
        && running.st_dev == named.st_dev && running.st_ino == named.st_ino)
        return QFile::decodeName(link);
    const QByteArray deleted(" (deleted)");
    if (!link.endsWith(deleted)) return QFile::decodeName(link);
    link.chop(deleted.size());
    const auto installed = QFile::decodeName(link);
    const QFileInfo program(installed);
    if (program.isFile() && program.isExecutable()) return installed;
    *error = Text::tr("%1 was removed after this launcher started, so it cannot start Branch Truce.").arg(installed);
    return std::nullopt;
}

bool connectSession(QLocalSocket *socket, const QString &name, int attempts)
{
    for (int attempt = 0; attempt < attempts; ++attempt) {
        socket->abort();
        socket->connectToServer(name);
        if (socket->waitForConnected(200)) return true;
    }
    return false;
}

// waitForConnected returns at once when no server exists, so counted retries
// finish in milliseconds. After the GUI starts, the launcher retries for this
// long in wall-clock time; --session-wait changes it.
constexpr qint64 kSessionWaitMs = 15000;
constexpr qint64 kListenRetrySliceMs = 200;

bool connectUntilDeadline(QLocalSocket *socket, const QString &name, qint64 deadlineMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < deadlineMs) {
        socket->abort();
        socket->connectToServer(name);
        const qint64 remaining = deadlineMs - timer.elapsed();
        if (remaining <= 0) break;
        const qint64 slice = remaining < kListenRetrySliceMs ? remaining : kListenRetrySliceMs;
        const qint64 started = timer.elapsed();
        if (socket->waitForConnected(static_cast<int>(slice))) return true;
        const qint64 spent = timer.elapsed() - started;
        if (spent >= slice) continue;
        const qint64 left = deadlineMs - timer.elapsed();
        const qint64 pause = (slice - spent) < left ? (slice - spent) : left;
        if (pause > 0) QThread::msleep(static_cast<unsigned long>(pause));
    }
    return false;
}
}

QString startInstalledWindow(const QString &worktreeRoot, const QByteArray &path)
{
    QString error;
    const auto program = guiProgram(&error);
    if (!program) return error;
    QProcess gui;
    gui.setProgram(*program);
    gui.setArguments({QStringLiteral("--mergetool-path"), QString::fromLatin1(path.toHex()), worktreeRoot});
    gui.setStandardInputFile(QProcess::nullDevice());
    gui.setStandardOutputFile(QProcess::nullDevice());
    gui.setStandardErrorFile(QProcess::nullDevice());
    return gui.startDetached() ? QString{} : Text::tr("%1 could not be started.").arg(*program);
}

int runMergetoolLauncher(const QStringList &arguments, const git::GitProgram &git,
                         const MergetoolLauncherOptions &options)
{
    QCommandLineParser parser;
    parser.addOption({QStringLiteral("mergetool"), Text::tr("Run as the launcher for git mergetool.")});
    const QCommandLineOption sessionWait(
        QStringLiteral("session-wait"),
        Text::tr("Seconds to wait for a started Branch Truce window to listen; the default is %1.")
            .arg(kSessionWaitMs / 1000),
        QStringLiteral("seconds"));
    parser.addOption(sessionWait);
    const QCommandLineOption noWindow(QStringLiteral("no-window"),
                                      Text::tr("Attach only to a Branch Truce window that already listens."));
    parser.addOption(noWindow);
    if (!parser.parse(arguments) || !parser.positionalArguments().isEmpty())
        return fail(parser.errorText().isEmpty() ? Text::tr("The launcher takes no arguments.") : parser.errorText());
    qint64 waitMs = kSessionWaitMs;
    if (parser.isSet(sessionWait)) {
        bool number = false;
        const auto seconds = parser.value(sessionWait).toDouble(&number);
        if (!number || seconds < 0 || seconds > 3600)
            return fail(Text::tr("--session-wait takes a number of seconds from 0 to 3600."));
        waitMs = static_cast<qint64>(seconds * 1000);
    }
    const auto startWindow = parser.isSet(noWindow) ? MergetoolWindowStarter{} : options.startWindow;
    const auto mergedPath = qgetenv("MERGED");
    const auto repositoryPath = mergetoolRepositoryPath(mergedPath);
    if (!repositoryPath) return fail(Text::tr("MERGED is missing or is not a repository path."));

    git::Request request;
    request.operation = git::Operation::DiscoverRepository;
    request.directory = QDir::currentPath();
    // The adapter's deadline bounds discovery, and a timeout reports itself.
    const auto discovered = git::runBlocking(git, request);
    if (discovered.outcome != git::Outcome::Success || !discovered.repository.supported) {
        const auto reason = !discovered.diagnostic.isEmpty() ? discovered.diagnostic : discovered.repository.diagnostic;
        return fail(!reason.isEmpty() ? reason
                                      : Text::tr("This repository operation cannot join a mergetool session."));
    }

    MergetoolSources sources;
    sources.path = *repositoryPath;
    QString error;
    if (!readMergetoolFile(mergedPath, &sources.merged, &error)) return fail(error);
    const auto readSide = [&](const char *name, QByteArray *bytes, bool *present) {
        const auto path = qgetenv(name);
        if (path.isEmpty()) return true;
        *present = true;
        return readMergetoolFile(path, bytes, &error);
    };
    if (!readSide("BASE", &sources.base, &sources.basePresent)
        || !readSide("LOCAL", &sources.local, &sources.localPresent)
        || !readSide("REMOTE", &sources.remote, &sources.remotePresent))
        return fail(error);

    // The window and every launcher of this user meet in one private directory.
    const auto directory = mergetoolSocketDirectory(qgetenv("XDG_RUNTIME_DIR"));
    if (!prepareMergetoolSocketDirectory(directory, &error)) return fail(error);
    const auto key = mergetoolSessionKey(discovered.repository);
    const auto invocation = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto intake = writeMergetoolIntake(mergetoolIntakeRoot(directory), key, invocation, sources, &error);
    if (intake.isEmpty()) return fail(error.isEmpty() ? Text::tr("The mergetool inputs could not be kept.") : error);
    // The session reads the copies when it accepts the attach; nothing needs them after this launcher.
    const auto removeIntake = qScopeGuard([&intake] { removeMergetoolIntake(intake); });

    const auto server = mergetoolServerName(directory, discovered.repository);
    QLocalSocket socket;
    bool connected = connectSession(&socket, server, 5);
    if (!connected && startWindow) {
        if (const auto refusal = startWindow(discovered.repository.worktreeRoot, sources.path); !refusal.isEmpty())
            return fail(refusal);
        connected = connectUntilDeadline(&socket, server, waitMs);
    }
    if (!connected) return fail(Text::tr("No Branch Truce session is listening for this repository."));
    // The /tmp fallback directory is shared, so check who answered before sending anything.
    if (!options.peerIsCurrentUser || !options.peerIsCurrentUser(socket.socketDescriptor()))
        return fail(Text::tr("The session socket does not belong to this user."));

    if (!sendMessage(socket, AttachMessage{invocation, intake, git.environment}))
        return fail(Text::tr("The mergetool attach could not be sent."));
    QByteArray buffer;
    if (const auto refusal = attachRefusal(readReply(socket, buffer, options.replyWaitMs), invocation);
        !refusal.isEmpty())
        return fail(refusal);

    const auto decision = readReply(socket, buffer, -1);
    const auto *resolved = decision.message ? std::get_if<ResolvedMessage>(&*decision.message) : nullptr;
    const auto *leave = decision.message ? std::get_if<LeaveMessage>(&*decision.message) : nullptr;
    QString outcome;
    if (resolved && resolved->invocation == invocation) {
        outcome = resolvedRefusal(git, mergedPath, sources.path, *resolved);
        if (outcome.isEmpty()) return 0;
    } else if (!(leave && leave->invocation == invocation)) {
        outcome = !decision.failure.isEmpty() ? decision.failure
                                              : Text::tr("The launcher closed before an explicit decision.");
    }
    if (!restoreMergetoolFile(mergedPath, sources.merged, &error)) return fail(error);
    return outcome.isEmpty() ? 1 : fail(outcome);
}
}
