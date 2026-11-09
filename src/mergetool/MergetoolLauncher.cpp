// The Git mergetool launcher waits for the reviewed result to be handed back to its parent process.
#include "mergetool/MergetoolLauncher.h"
#include "mergetool/MergetoolMessages.h"
#include "mergetool/MergetoolProtocol.h"

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
    return QStringLiteral("The Branch Truce window speaks mergetool protocol %1 and this launcher speaks %2. "
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
    if (!decoded.message) return {std::nullopt, decoded.error};
    if (const auto *unsupported = std::get_if<UnsupportedMessage>(&*decoded.message))
        return {std::nullopt, otherVersion(unsupported->version)};
    return {decoded.message, {}};
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
    *error =
        QStringLiteral("%1 was removed after this launcher started, so it cannot start Branch Truce.").arg(installed);
    return std::nullopt;
}

// The GUI opens on the launched path. Its standard streams go to the null
// device, so it never writes into the git mergetool terminal.
bool startSession(const QString &program, const QString &worktreeRoot, const QByteArray &path)
{
    QProcess gui;
    gui.setProgram(program);
    gui.setArguments({QStringLiteral("--mergetool-path"), QString::fromLatin1(path.toHex()), worktreeRoot});
    gui.setStandardInputFile(QProcess::nullDevice());
    gui.setStandardOutputFile(QProcess::nullDevice());
    gui.setStandardErrorFile(QProcess::nullDevice());
    return gui.startDetached();
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
// How long the session may take to accept or reject the attach.
constexpr int kReplyWaitMs = 15000;

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

int runMergetoolLauncher(int argc, char **argv, const git::GitProgram &git)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Branch Truce"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("canpicasoft.com"));
    QCommandLineParser parser;
    parser.addOption({QStringLiteral("mergetool"), QStringLiteral("Run as the launcher for git mergetool.")});
    const QCommandLineOption sessionWait(
        QStringLiteral("session-wait"),
        QStringLiteral("Seconds to wait for a started Branch Truce window to listen; the default is %1.")
            .arg(kSessionWaitMs / 1000),
        QStringLiteral("seconds"));
    parser.addOption(sessionWait);
    if (!parser.parse(QCoreApplication::arguments()) || !parser.positionalArguments().isEmpty())
        return fail(parser.errorText().isEmpty() ? QStringLiteral("The launcher takes no arguments.")
                                                 : parser.errorText());
    qint64 waitMs = kSessionWaitMs;
    if (parser.isSet(sessionWait)) {
        bool number = false;
        const auto seconds = parser.value(sessionWait).toDouble(&number);
        if (!number || seconds < 0 || seconds > 3600)
            return fail(QStringLiteral("--session-wait takes a number of seconds from 0 to 3600."));
        waitMs = static_cast<qint64>(seconds * 1000);
    }
    const auto mergedPath = qgetenv("MERGED");
    const auto repositoryPath = mergetoolRepositoryPath(mergedPath);
    if (!repositoryPath) return fail(QStringLiteral("MERGED is missing or is not a repository path."));

    git::Request request;
    request.operation = git::Operation::DiscoverRepository;
    request.directory = QDir::currentPath();
    // The adapter's deadline bounds discovery, and a timeout reports itself.
    const auto discovered = git::runBlocking(git, request);
    if (discovered.outcome != git::Outcome::Success || !discovered.repository.supported) {
        const auto reason = !discovered.diagnostic.isEmpty() ? discovered.diagnostic : discovered.repository.diagnostic;
        return fail(!reason.isEmpty() ? reason
                                      : QStringLiteral("This repository operation cannot join a mergetool session."));
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
    if (intake.isEmpty())
        return fail(error.isEmpty() ? QStringLiteral("The mergetool inputs could not be kept.") : error);
    // The session reads the copies when it accepts the attach; nothing needs them after this launcher.
    const auto removeIntake = qScopeGuard([&intake] { removeMergetoolIntake(intake); });

    const auto server = mergetoolServerName(directory, discovered.repository);
    const bool noSpawn = qEnvironmentVariableIsSet("BRANCH_TRUCE_MERGETOOL_NO_SPAWN");
    QLocalSocket socket;
    bool connected = connectSession(&socket, server, 5);
    if (!connected && !noSpawn) {
        const auto program = guiProgram(&error);
        if (!program) return fail(error);
        if (!startSession(*program, discovered.repository.worktreeRoot, sources.path))
            return fail(QStringLiteral("%1 could not be started.").arg(*program));
        connected = connectUntilDeadline(&socket, server, waitMs);
    }
    if (!connected) return fail(QStringLiteral("No Branch Truce session is listening for this repository."));
    // The /tmp fallback directory is shared, so check who answered before sending anything.
    if (!mergetoolPeerIsCurrentUser(socket.socketDescriptor()))
        return fail(QStringLiteral("The session socket does not belong to this user."));

    if (!sendMessage(socket, AttachMessage{invocation, intake, git.environment}))
        return fail(QStringLiteral("The mergetool attach could not be sent."));
    QByteArray buffer;
    const auto reply = readReply(socket, buffer, kReplyWaitMs);
    const auto *attached = reply.message ? std::get_if<AttachedMessage>(&*reply.message) : nullptr;
    if (!attached || attached->invocation != invocation) {
        const auto *rejected = reply.message ? std::get_if<RejectedMessage>(&*reply.message) : nullptr;
        return fail(rejected                       ? rejected->reason
                        : !reply.failure.isEmpty() ? reply.failure
                                                   : QStringLiteral("The session rejected this path."));
    }

    const auto decision = readReply(socket, buffer, -1);
    const auto *resolved = decision.message ? std::get_if<ResolvedMessage>(&*decision.message) : nullptr;
    if (resolved && resolved->invocation == invocation) return 0;
    if (!restoreMergetoolFile(mergedPath, sources.merged, &error)) return fail(error);
    const auto *leave = decision.message ? std::get_if<LeaveMessage>(&*decision.message) : nullptr;
    if (leave && leave->invocation == invocation) return 1;
    return fail(!decision.failure.isEmpty() ? decision.failure
                                            : QStringLiteral("The launcher closed before an explicit decision."));
}
}
