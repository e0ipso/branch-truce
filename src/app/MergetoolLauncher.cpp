#include "MergetoolLauncher.h"
#include "MergetoolProtocol.h"

#include <optional>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QLocalSocket>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>
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

bool connectSession(QLocalSocket *socket, const QString &name, int attempts)
{
    for (int attempt = 0; attempt < attempts; ++attempt) {
        socket->abort();
        socket->connectToServer(name);
        if (socket->waitForConnected(200)) return true;
    }
    return false;
}
}

int runMergetoolLauncher(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Branch Truce"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("branch-truce.local"));
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
        QProcess::startDetached(QCoreApplication::applicationFilePath(), {discovered->repository.worktreeRoot});
        connected = connectSession(&socket, server, 75);
    }
    if (!connected) return fail(QStringLiteral("No Branch Truce session is listening for this repository."));

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
