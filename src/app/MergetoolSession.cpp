#include "MergetoolSession.h"
#include "ApplicationController.h"
#include "MergetoolProtocol.h"

#include <QFileInfo>
#include <QHash>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>
#include <algorithm>

namespace branch_truce {
MergetoolSession::MergetoolSession(ApplicationController *controller) : QObject(controller), controller_(controller)
{
    acknowledgment_ = new QTimer(this);
    acknowledgment_->setInterval(200);
    connect(acknowledgment_, &QTimer::timeout, this, &MergetoolSession::pollAcknowledgment);
}

bool MergetoolSession::listen(const git::RepositoryContext &repository)
{
    const auto name = mergetoolServerName(repository);
    sessionKey_ = mergetoolSessionKey(repository);
    if (name.isEmpty() || listening_) return false;
    repository_ = repository;
    QLocalSocket probe;
    probe.connectToServer(name);
    if (probe.waitForConnected(50)) {
        probe.disconnectFromServer();
        return false;
    }
    QLocalServer::removeServer(name);
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server_->listen(name)) return false;
    connect(server_, &QLocalServer::newConnection, this, &MergetoolSession::acceptConnection);
    listening_ = true;
    return true;
}

bool MergetoolSession::blocks(const QByteArray &path) const
{
    if (!parentSession_) return false;
    const auto index = indexOfPath(path, true);
    return index < 0 || !waiters_.at(index).assigned;
}

bool MergetoolSession::waitingOn(const QByteArray &path) const
{
    const auto index = indexOfPath(path, true);
    return index >= 0 && waiters_.at(index).assigned && !offerHeld_;
}

QByteArray MergetoolSession::waitingPath() const
{
    for (const auto &waiter : waiters_)
        if (!waiter.released && waiter.assigned) return waiter.path;
    return {};
}

bool MergetoolSession::hasWaiter(const QByteArray &path) const
{
    return indexOfPath(path, true) >= 0;
}

bool MergetoolSession::leave(const QByteArray &path)
{
    if (controller_->resolutionBusy() || offerHeld_) return false;
    const auto index = indexOfPath(path, true);
    if (index < 0) return false;
    auto &waiter = waiters_[index];
    const QJsonObject message{{QStringLiteral("v"), 1}, {QStringLiteral("kind"), QStringLiteral("leave")},
                              {QStringLiteral("invocation"), waiter.invocation}};
    if (!send(waiter.socket, message)) return false;
    waiter.released = true;
    if (waiter.assigned && !offerHeld_) {
        assigning_ = true;
        controller_->assignParentStaging({});
        assigning_ = false;
        waiter.assigned = false;
        tryAssign();
    }
    Q_EMIT changed();
    return true;
}

int MergetoolSession::unreleasedLaunches() const
{
    return static_cast<int>(std::count_if(waiters_.begin(), waiters_.end(),
                                          [](const Waiter &waiter) { return !waiter.released; }));
}

int MergetoolSession::leaveAll()
{
    int told = 0;
    bool clearAssignment = false;
    for (auto &waiter : waiters_) {
        if (waiter.released) continue;
        const QJsonObject message{{QStringLiteral("v"), 1}, {QStringLiteral("kind"), QStringLiteral("leave")},
                                  {QStringLiteral("invocation"), waiter.invocation}};
        // A launcher that cannot be reached restores its file when its socket
        // closes, so the outcome is unresolved either way.
        if (send(waiter.socket, message)) ++told;
        waiter.released = true;
        if (waiter.assigned && !offerHeld_) {
            waiter.assigned = false;
            clearAssignment = true;
        }
    }
    if (clearAssignment) {
        assigning_ = true;
        controller_->assignParentStaging({});
        assigning_ = false;
    }
    Q_EMIT changed();
    return told;
}

bool MergetoolSession::canEndParentSession() const
{
    if (!parentSession_ || offerHeld_) return false;
    for (const auto &waiter : waiters_)
        if (!waiter.released) return false;
    return true;
}

bool MergetoolSession::endParentSession()
{
    if (!canEndParentSession()) return false;
    parentSession_ = false;
    assigning_ = true;
    controller_->assignParentStaging({});
    assigning_ = false;
    Q_EMIT notice(tr("Parent Git is finished. Remaining files can be resolved here."));
    Q_EMIT changed();
    return true;
}

void MergetoolSession::onPublisherChanged()
{
    if (assigning_) return;
    if (offerHeld_) {
        if (!controller_->parentStagingOffer(acknowledgmentFile_)) {
            offerHeld_ = false;
            acknowledgment_->stop();
            tryAssign();
            Q_EMIT changed();
        }
        return;
    }
    const auto path = waitingPath();
    if (path.isEmpty()) {
        tryAssign();
        return;
    }
    const auto offer = controller_->parentStagingOffer(QString::fromLatin1(path.toHex()));
    if (!offer || offer->path != path) return;
    const auto index = indexOfPath(path, true);
    if (index < 0) return;
    auto &waiter = waiters_[index];
    const QJsonObject message{{QStringLiteral("v"), 1}, {QStringLiteral("kind"), QStringLiteral("resolved")},
        {QStringLiteral("invocation"), waiter.invocation},
        {QStringLiteral("revision"), QString::number(offer->revision)},
        {QStringLiteral("object"), QString::fromLatin1(offer->canonicalObjectId)}};
    if (!send(waiter.socket, message)) return;
    waiter.released = true;
    offerHeld_ = true;
    acknowledgmentFile_ = QString::fromLatin1(path.toHex());
    acknowledgmentRevision_ = QString::number(offer->revision);
    acknowledgmentAttempts_ = 0;
    acknowledgment_->start();
    Q_EMIT changed();
}

void MergetoolSession::acceptConnection()
{
    while (auto *socket = server_->nextPendingConnection()) {
        socket->setParent(this);
        buffers_.insert(socket, {});
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { readSocket(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] { dropped(socket); });
    }
}

void MergetoolSession::readSocket(QLocalSocket *socket)
{
    auto buffer = buffers_.find(socket);
    if (buffer == buffers_.end()) return;
    buffer->append(socket->readAll());
    QString error;
    QJsonObject message;
    const auto frame = takeMergetoolFrame(&(*buffer), &message, &error);
    if (frame == MergetoolFrame::NeedMore) return;
    if (frame == MergetoolFrame::Invalid || message.value(QStringLiteral("v")).toInt() != 1
        || message.value(QStringLiteral("kind")).toString() != QLatin1String("attach")) {
        reject(socket, message.value(QStringLiteral("invocation")).toString(),
               error.isEmpty() ? tr("The mergetool attach message is invalid.") : error);
        return;
    }
    const auto invocation = message.value(QStringLiteral("invocation")).toString();
    const auto attached = readMergetoolIntake(message.value(QStringLiteral("intake")).toString(), sessionKey_, &error);
    if (!attached || attached->invocation != invocation) {
        reject(socket, invocation, error.isEmpty() ? tr("The mergetool inputs could not be kept.") : error);
        return;
    }
    if (indexOfPath(attached->path, true) >= 0) {
        reject(socket, invocation, tr("Git is already waiting on this path."));
        return;
    }
    const auto *file = controller_->files()->file(attached->path.toHex().toStdString());
    if (!file || file->support() != Support::Supported) {
        reject(socket, invocation, tr("This path is not an editable conflict in the open session."));
        return;
    }
    buffers_.remove(socket);
    Waiter waiter;
    waiter.invocation = invocation;
    waiter.path = attached->path;
    waiter.intake = QFileInfo(message.value(QStringLiteral("intake")).toString()).canonicalFilePath();
    waiter.socket = socket;
    waiters_.append(waiter);
    intakes_.append(waiter.intake);
    parentSession_ = true;
    const QJsonObject accepted{{QStringLiteral("v"), 1}, {QStringLiteral("kind"), QStringLiteral("attached")},
                               {QStringLiteral("invocation"), invocation}};
    if (!send(socket, accepted)) {
        waiters_.last().released = true;
        return;
    }
    tryAssign();
    Q_EMIT notice(tr("Git is waiting. Mark resolved hands this path back, or leave it unresolved."));
    Q_EMIT changed();
}

void MergetoolSession::dropped(QLocalSocket *socket)
{
    buffers_.remove(socket);
    const auto index = indexOfSocket(socket);
    if (index < 0 || waiters_.at(index).released) return;
    auto &waiter = waiters_[index];
    waiter.released = true;
    if (waiter.assigned && !offerHeld_) {
        assigning_ = true;
        controller_->assignParentStaging({});
        assigning_ = false;
        waiter.assigned = false;
        tryAssign();
    }
    Q_EMIT notice(tr("Git's launcher closed before resolution. The draft is still in Branch Truce."));
    Q_EMIT changed();
}

void MergetoolSession::reject(QLocalSocket *socket, const QString &invocation, const QString &reason)
{
    const QJsonObject message{{QStringLiteral("v"), 1}, {QStringLiteral("kind"), QStringLiteral("rejected")},
                              {QStringLiteral("invocation"), invocation}, {QStringLiteral("reason"), reason}};
    send(socket, message);
    buffers_.remove(socket);
    socket->disconnectFromServer();
}

bool MergetoolSession::send(QLocalSocket *socket, const QJsonObject &object)
{
    if (!socket || socket->state() != QLocalSocket::ConnectedState) return false;
    QString error;
    return writeMergetoolFrame(socket, object, &error);
}

void MergetoolSession::tryAssign()
{
    if (assigning_ || offerHeld_ || controller_->resolutionBusy() || !waitingPath().isEmpty()) return;
    for (int index = 0; index < waiters_.size(); ++index) {
        auto &waiter = waiters_[index];
        if (waiter.released || waiter.assigned) continue;
        assigning_ = true;
        const bool assigned = controller_->assignParentStaging(waiter.path);
        assigning_ = false;
        if (!assigned) return;
        waiter.assigned = true;
        Q_EMIT changed();
        return;
    }
}

void MergetoolSession::pollAcknowledgment()
{
    if (!offerHeld_ || controller_->resolutionBusy()) return;
    if (!controller_->parentStagingOffer(acknowledgmentFile_)) {
        offerHeld_ = false;
        acknowledgment_->stop();
        tryAssign();
        Q_EMIT changed();
        return;
    }
    if (++acknowledgmentAttempts_ > 100) {
        acknowledgment_->stop();
        Q_EMIT notice(tr("Written, unresolved. Git has not staged the reviewed result yet."));
        Q_EMIT changed();
        return;
    }
    controller_->verifyParentAcknowledgment(acknowledgmentFile_, acknowledgmentRevision_);
}

int MergetoolSession::indexOfSocket(const QLocalSocket *socket) const
{
    for (int index = 0; index < waiters_.size(); ++index)
        if (waiters_.at(index).socket == socket) return index;
    return -1;
}

int MergetoolSession::indexOfPath(const QByteArray &path, bool unreleasedOnly) const
{
    for (int index = 0; index < waiters_.size(); ++index) {
        const auto &waiter = waiters_.at(index);
        if (unreleasedOnly && waiter.released) continue;
        if (waiter.path == path) return index;
    }
    return -1;
}
}
