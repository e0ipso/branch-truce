#include "mergetool/MergetoolSession.h"
#include "mergetool/MergetoolProtocol.h"
#include "recovery/RecoveryRestore.h"
#include "app/SessionModel.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QHash>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>
#include <algorithm>

namespace branch_truce {
MergetoolSession::MergetoolSession(ResolutionPublisher &publisher, SessionModel &files, SessionCoordinator &coordinator,
                                   std::function<git::RepositoryContext()> loadedRepository, QString gitExecutable,
                                   QObject *parent)
    : QObject(parent), publisher_(publisher), files_(files), coordinator_(coordinator),
      loadedRepository_(std::move(loadedRepository)), gitExecutable_(std::move(gitExecutable))
{
    acknowledgment_ = new QTimer(this);
    acknowledgment_->setInterval(200);
    connect(acknowledgment_, &QTimer::timeout, this, &MergetoolSession::pollAcknowledgment);
    // A fenced revalidation never completes; the next attach may check again.
    connect(&coordinator_, &SessionCoordinator::taskFinished, this, [this](const TaskCompletion &completion) {
        if (completion.disposition == TaskDisposition::Current) return;
        std::erase_if(copiedLaunchChecks_, [&](const auto &check) { return check.second == completion.stamp.id; });
    });
}

MergetoolSession::Listen MergetoolSession::listen(const git::RepositoryContext &repository, QString *error)
{
    const auto unavailable = [error](const QString &reason) {
        if (error) *error = reason;
        return Listen::Unavailable;
    };
    if (listening_) return Listen::Listening;
    const auto name = mergetoolServerName(repository);
    sessionKey_ = mergetoolSessionKey(repository);
    if (name.isEmpty()) return unavailable(tr("This operation has no mergetool session identity."));
    repository_ = repository;
    QString reason;
    if (!prepareMergetoolSocketDirectory(QFileInfo(name).path(), &reason)) return unavailable(reason);
    QLocalSocket probe;
    probe.connectToServer(name);
    if (probe.waitForConnected(50)) {
        probe.disconnectFromServer();
        return Listen::Elsewhere;
    }
    QLocalServer::removeServer(name);
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server_->listen(name)) {
        reason = tr("The session socket %1 could not listen: %2").arg(name, server_->errorString());
        delete server_;
        server_ = nullptr;
        return unavailable(reason);
    }
    connect(server_, &QLocalServer::newConnection, this, &MergetoolSession::acceptConnection);
    listening_ = true;
    return Listen::Listening;
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

QByteArray MergetoolSession::waitingMergedSha256(const QByteArray &path) const
{
    const auto index = indexOfPath(path, true);
    return index < 0 ? QByteArray{} : waiters_.at(index).mergedSha256;
}

bool MergetoolSession::leave(const QByteArray &path)
{
    if (publisher_.busy() || offerHeld_) return false;
    const auto index = indexOfPath(path, true);
    if (index < 0) return false;
    auto &waiter = waiters_[index];
    const QJsonObject message{{QStringLiteral("v"), 1}, {QStringLiteral("kind"), QStringLiteral("leave")},
                              {QStringLiteral("invocation"), waiter.invocation}};
    if (!send(waiter.socket, message)) return false;
    waiter.released = true;
    if (waiter.assigned && !offerHeld_) {
        assigning_ = true;
        publisher_.assignParentStaging({});
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
        publisher_.assignParentStaging({});
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
    publisher_.assignParentStaging({});
    assigning_ = false;
    Q_EMIT notice(tr("Parent Git is finished. Remaining files can be resolved here."));
    Q_EMIT changed();
    return true;
}

void MergetoolSession::reassign()
{
    // A held offer already went to its launcher; its acknowledgment poll
    // assigns the next waiter once the offer is gone.
    if (offerHeld_) return;
    bool dropped = false;
    for (auto &waiter : waiters_) {
        if (waiter.released || !waiter.assigned) continue;
        waiter.assigned = false;
        dropped = true;
    }
    // A path the reloaded files cannot stage stays unassigned, so Mark
    // resolved waits instead of staging it in the app.
    tryAssign();
    if (dropped) Q_EMIT changed();
}

void MergetoolSession::onPublisherChanged()
{
    if (assigning_) return;
    if (offerHeld_) {
        if (!publisher_.parentStagingOffer(acknowledgmentFile_)) {
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
    const auto offer = publisher_.parentStagingOffer(path.toHex().toStdString());
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
    acknowledgmentFile_ = path.toHex().toStdString();
    acknowledgmentRevision_ = offer->revision;
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
    const auto *file = files_.file(attached->path.toHex().toStdString());
    if (!file || file->support() != Support::Supported) {
        reject(socket, invocation, tr("This path is not an editable conflict in the open session."));
        return;
    }
    buffers_.remove(socket);
    Waiter waiter;
    waiter.invocation = invocation;
    waiter.path = attached->path;
    waiter.intake = QFileInfo(message.value(QStringLiteral("intake")).toString()).canonicalFilePath();
    waiter.mergedSha256 = attached->mergedSha256;
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
    acceptCopiedLaunches();
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
        publisher_.assignParentStaging({});
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
    if (assigning_ || offerHeld_ || publisher_.busy() || !waitingPath().isEmpty()) return;
    for (int index = 0; index < waiters_.size(); ++index) {
        auto &waiter = waiters_[index];
        if (waiter.released || waiter.assigned) continue;
        assigning_ = true;
        const bool assigned = publisher_.assignParentStaging(waiter.path);
        assigning_ = false;
        if (!assigned) return;
        waiter.assigned = true;
        Q_EMIT changed();
        return;
    }
}

void MergetoolSession::pollAcknowledgment()
{
    if (!offerHeld_ || publisher_.busy()) return;
    if (!publisher_.parentStagingOffer(acknowledgmentFile_)) {
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
    publisher_.verifyParentAcknowledgment(acknowledgmentFile_, acknowledgmentRevision_);
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

void MergetoolSession::adoptCopiedLaunches(std::map<std::string, SnapshotFileIdentity> copiedLaunches)
{
    copiedLaunches_ = std::move(copiedLaunches);
}

void MergetoolSession::clearCopiedLaunches()
{
    copiedLaunches_.clear();
    copiedLaunchChecks_.clear();
}

bool MergetoolSession::launchedWithCapturedBytes(const FileSession &file) const
{
    const auto launch = QCryptographicHash::hash(QByteArray::fromStdString(file.snapshots()->launchResult.text.bytes),
        QCryptographicHash::Sha256).toHex();
    return waitingMergedSha256(QByteArray::fromStdString(file.path())) == launch;
}

void MergetoolSession::acceptCopiedLaunches()
{
    for (auto entry = copiedLaunches_.begin(); entry != copiedLaunches_.end();) {
        const auto *file = files_.file(entry->first);
        if (!file || !file->snapshots() || !files_.hasRecoveryFailure(entry->first)) {
            copiedLaunchChecks_.erase(entry->first);
            entry = copiedLaunches_.erase(entry);
            continue;
        }
        if (copiedLaunchChecks_.contains(entry->first) || !launchedWithCapturedBytes(*file)) {
            ++entry;
            continue;
        }
        // Git launched this path with the launch bytes. The index and the
        // conversion policy may have changed since the restore validated them,
        // so Git inspects both again off the GUI thread before the error clears.
        const auto fileId = entry->first;
        const auto identity = entry->second;
        const auto task = coordinator_.submit(fileId, file->current().id,
            [snapshots = *file->snapshots(), identity, repository = loadedRepository_(),
             executable = gitExecutable_](const TaskInput &, const TaskControl &) {
                TaskResult result;
                result.diagnostic = revalidateCopiedLaunch(snapshots, identity, repository, executable);
                return result;
            },
            [this, fileId, identity](const TaskCompletion &completion) {
                copiedLaunchChecks_.erase(fileId);
                settleCopiedLaunch(fileId, identity, completion.result.diagnostic);
            });
        if (task != 0) copiedLaunchChecks_.insert_or_assign(fileId, task);
        ++entry;
    }
}

void MergetoolSession::settleCopiedLaunch(const std::string &fileId, const SnapshotFileIdentity &identity,
    const QString &failure)
{
    const auto entry = copiedLaunches_.find(fileId);
    if (entry == copiedLaunches_.end() || entry->second != identity) return;
    const auto *file = files_.file(fileId);
    if (!file || !file->snapshots() || !files_.hasRecoveryFailure(fileId)) {
        copiedLaunches_.erase(entry);
        return;
    }
    // The launcher left while Git inspected the path; a later attach checks again.
    if (!launchedWithCapturedBytes(*file)) return;
    // The copy must still be the file recovery validated; any change keeps the path failed.
    const bool settled = failure.isEmpty() && publisher_.adoptCopiedLaunch(fileId, identity)
        ? files_.settleRecoveryFailure(fileId, copiedLaunchFailure())
        : files_.settleRecoveryFailure(fileId, copiedLaunchFailure(), !failure.isEmpty() ? failure
            : tr("Recovered draft retained, but the repository file changed after recovery validation. "
                 "Work remains outstanding; no publication is authorized."));
    copiedLaunches_.erase(entry);
    if (settled) Q_EMIT changed();
}
}
