#include "MergetoolLauncher.h"
#include "MergetoolProtocol.h"

#include <climits>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUuid>

namespace branch_truce {
namespace {
int fail(const QString &message)
{
    QTextStream(stderr) << "mergetool: " << message << '\n';
    return 1;
}

bool readFrame(QLocalSocket *socket, QByteArray *buffer, QJsonObject *object, bool untilDecision)
{
    while (true) {
        QString error;
        const auto frame = takeMergetoolFrame(buffer, object, &error);
        if (frame == MergetoolFrame::Invalid) {
            fail(error);
            return false;
        }
        if (frame == MergetoolFrame::Ready) return true;
        if (socket->bytesAvailable() > 0) {
            buffer->append(socket->readAll());
            continue;
        }
        if (socket->state() != QLocalSocket::ConnectedState) return false;
        const int slice = untilDecision ? 1000 : 15000;
        if (!socket->waitForReadyRead(slice)) {
            if (!untilDecision || socket->state() != QLocalSocket::ConnectedState) return false;
        }
        buffer->append(socket->readAll());
    }
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
    struct stat running {};
    struct stat named {};
    if (::stat("/proc/self/exe", &running) == 0 && ::stat(link.constData(), &named) == 0
        && running.st_dev == named.st_dev && running.st_ino == named.st_ino)
        return QFile::decodeName(link);
    const QByteArray deleted(" (deleted)");
    if (!link.endsWith(deleted)) return QFile::decodeName(link);
    link.chop(deleted.size());
    const auto installed = QFile::decodeName(link);
    const QFileInfo program(installed);
    if (program.isFile() && program.isExecutable()) return installed;
    *error = QStringLiteral("%1 was removed after this launcher started, so it cannot start Branch Truce.").arg(installed);
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
// finish in milliseconds. After the GUI starts, spend that 75x200ms budget
// as wall-clock time.
constexpr qint64 kPostSpawnListenDeadlineMs = 75 * 200;
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

int runMergetoolLauncher(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Branch Truce"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("canpicasoft.com"));
    const auto mergedPath = qgetenv("MERGED");
    const auto repositoryPath = mergetoolRepositoryPath(mergedPath);
    if (!repositoryPath) return fail(QStringLiteral("MERGED is missing or is not a repository path."));

    git::GitAdapter adapter(QStandardPaths::findExecutable(QStringLiteral("git")));
    std::optional<git::Result> discovered;
    QEventLoop discovery;
    QTimer discoveryTimeout;
    discoveryTimeout.setSingleShot(true);
    QObject::connect(&adapter, &git::GitAdapter::finished, &discovery, [&](const git::Result &result) {
        discovered = result;
        discovery.quit();
    });
    QObject::connect(&discoveryTimeout, &QTimer::timeout, &discovery, &QEventLoop::quit);
    git::Request request;
    request.operation = git::Operation::DiscoverRepository;
    request.directory = QDir::currentPath();
    if (!adapter.start(request)) return fail(QStringLiteral("Repository discovery could not start."));
    discoveryTimeout.start(20000);
    discovery.exec();
    if (!discovered || discovered->outcome != git::Outcome::Success || !discovered->repository.supported) {
        return fail(discovered && !discovered->diagnostic.isEmpty()
            ? discovered->diagnostic : QStringLiteral("This repository operation cannot join a mergetool session."));
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
        || !readSide("REMOTE", &sources.remote, &sources.remotePresent)) return fail(error);

    const auto key = mergetoolSessionKey(discovered->repository);
    const auto invocation = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto intake = mergetoolIntakeRoot() + QLatin1Char('/') + QString::fromLatin1(key) + QLatin1Char('/') + invocation;
    if (key.isEmpty() || !writeMergetoolIntake(intake, invocation, sources, &error))
        return fail(error.isEmpty() ? QStringLiteral("The mergetool inputs could not be kept.") : error);

    const auto server = mergetoolServerName(discovered->repository);
    const bool noSpawn = qEnvironmentVariableIsSet("BRANCH_TRUCE_MERGETOOL_NO_SPAWN");
    QLocalSocket socket;
    bool connected = connectSession(&socket, server, 5);
    if (!connected && !noSpawn) {
        const auto program = guiProgram(&error);
        if (!program) return fail(error);
        if (!startSession(*program, discovered->repository.worktreeRoot, sources.path))
            return fail(QStringLiteral("%1 could not be started.").arg(*program));
        connected = connectUntilDeadline(&socket, server, kPostSpawnListenDeadlineMs);
    }
    if (!connected) return fail(QStringLiteral("No Branch Truce session is listening for this repository."));
    // The /tmp fallback directory is shared, so check who answered before sending anything.
    if (!mergetoolPeerIsCurrentUser(socket.socketDescriptor()))
        return fail(QStringLiteral("The session socket does not belong to this user."));

    const QJsonObject attach{{QStringLiteral("v"), 1}, {QStringLiteral("kind"), QStringLiteral("attach")},
        {QStringLiteral("invocation"), invocation}, {QStringLiteral("intake"), intake},
        {QStringLiteral("path"), QString::fromLatin1(sources.path.toHex())}};
    if (!writeMergetoolFrame(&socket, attach, &error)) return fail(error);
    QByteArray buffer = socket.readAll();
    QJsonObject reply;
    if (!readFrame(&socket, &buffer, &reply, false))
        return fail(QStringLiteral("The session rejected this path."));
    const bool attached = reply.value(QStringLiteral("kind")).toString() == QLatin1String("attached")
        && reply.value(QStringLiteral("invocation")).toString() == invocation;
    if (!attached) {
        const auto reason = reply.value(QStringLiteral("reason")).toString();
        return fail(reason.isEmpty() ? QStringLiteral("The session rejected this path.") : reason);
    }

    QJsonObject decision;
    const bool decided = readFrame(&socket, &buffer, &decision, true);
    const auto kind = decision.value(QStringLiteral("kind")).toString();
    const bool resolved = decided && kind == QLatin1String("resolved")
        && decision.value(QStringLiteral("invocation")).toString() == invocation
        && !decision.value(QStringLiteral("object")).toString().isEmpty()
        && !decision.value(QStringLiteral("revision")).toString().isEmpty();
    if (resolved) return 0;
    if (!restoreMergetoolFile(mergedPath, sources.merged, &error)) return fail(error);
    if (!decided) return fail(QStringLiteral("The launcher closed before an explicit decision."));
    return 1;
}
}
