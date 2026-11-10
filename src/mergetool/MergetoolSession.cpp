// The mergetool session connects waiting Git launchers to the window and tracks their published results.
#include "mergetool/MergetoolSession.h"
#include "problems/ProblemText.h"
#include "recovery/RecoveryRestore.h"
#include "storage/OffThread.h"

#include <QCryptographicHash>
#include <QThreadPool>
#include <QTimer>
#include <algorithm>
#include <memory>
#include <utility>

namespace branch_truce {
namespace {
// Acknowledgment polls back off from the first interval to the last, about
// two minutes in all, while the offer is held. Then the outcome is unknown
// until the user checks again.
constexpr int kFirstAcknowledgmentPollMs = 200;
constexpr int kLastAcknowledgmentPollMs = 5000;
constexpr int kAcknowledgmentPolls = 28;
}

MergetoolSession::MergetoolSession(ResolutionPublisher &publisher, SessionStore &files, SessionCoordinator &coordinator,
                                   std::function<git::RepositoryContext()> loadedRepository, Config config,
                                   QObject *parent)
    : QObject(parent),
      publisher_(publisher),
      files_(files),
      coordinator_(coordinator),
      loadedRepository_(std::move(loadedRepository)),
      config_(std::move(config)),
      server_(this)
{
    workers_.setMaxThreadCount(1);
    acknowledgment_ = new QTimer(this);
    acknowledgment_->setSingleShot(true);
    connect(acknowledgment_, &QTimer::timeout, this, &MergetoolSession::pollAcknowledgment);
    connect(&server_, &MergetoolServer::attachReceived, this, &MergetoolSession::attachReceived);
    connect(&server_, &MergetoolServer::closed, this, &MergetoolSession::closed);
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
    if (server_.listening()) return Listen::Listening;
    const auto name = mergetoolServerName(config_.socketDirectory, repository);
    if (name.isEmpty()) return unavailable(tr("This operation has no mergetool session identity."));
    sessionKey_ = mergetoolSessionKey(repository);
    repository_ = repository;
    if (Problem problem; !prepareMergetoolSocketDirectory(config_.socketDirectory, &problem))
        return unavailable(problem_text::text(problem));
    QString reason;
    const auto listening = server_.listen(name, &reason);
    if (listening == Listen::Unavailable) return unavailable(reason);
    // Copies left by launchers that were killed while they waited. Removing
    // them is file I/O, so it runs off the GUI thread, before any intake read.
    if (listening == Listen::Listening)
        workers_.start([root = mergetoolIntakeRoot(config_.socketDirectory), key = sessionKey_] {
            pruneMergetoolIntakes(root, key);
        });
    return listening;
}

bool MergetoolSession::blocks(const QByteArray &path) const
{
    if (!parentSession_) return false;
    const auto index = indexOfPath(path);
    return index < 0 || !waiters_.at(index).assigned;
}

bool MergetoolSession::waitingOn(const QByteArray &path) const
{
    const auto index = indexOfPath(path);
    return index >= 0 && waiters_.at(index).assigned && !offerHeld_;
}

QByteArray MergetoolSession::waitingPath() const
{
    for (const auto &waiter : waiters_)
        if (waiter.assigned) return waiter.path;
    return {};
}

bool MergetoolSession::hasWaiter(const QByteArray &path) const { return indexOfPath(path) >= 0; }

QByteArray MergetoolSession::waitingMergedSha256(const QByteArray &path) const
{
    const auto index = indexOfPath(path);
    return index < 0 ? QByteArray{} : waiters_.at(index).mergedSha256;
}

QString MergetoolSession::intake(const QByteArray &path) const
{
    const auto index = indexOfPath(path);
    return index < 0 ? QString{} : waiters_.at(index).intake;
}

bool MergetoolSession::leave(const QByteArray &path)
{
    if (publisher_.busy() || offerHeld_) return false;
    const auto index = indexOfPath(path);
    if (index < 0) return false;
    const auto &waiter = waiters_.at(index);
    if (!server_.send(waiter.connection, LeaveMessage{waiter.invocation})) return false;
    release(index);
    Q_EMIT changed();
    return true;
}

int MergetoolSession::leaveAll()
{
    int told = 0;
    bool assigned = false;
    // A launcher that cannot be reached restores its file when its socket
    // closes, so the outcome is unresolved either way.
    const auto leaving = std::exchange(waiters_, {});
    for (const auto &waiter : leaving) {
        if (server_.send(waiter.connection, LeaveMessage{waiter.invocation})) ++told;
        assigned = assigned || waiter.assigned;
        server_.close(waiter.connection);
    }
    if (assigned && !offerHeld_) clearAssignment();
    Q_EMIT changed();
    return told;
}

bool MergetoolSession::canEndParentSession() const { return parentSession_ && !offerHeld_ && waiters_.isEmpty(); }

bool MergetoolSession::endParentSession()
{
    if (!canEndParentSession()) return false;
    parentSession_ = false;
    clearAssignment();
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
        dropped = dropped || waiter.assigned;
        waiter.assigned = false;
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
        if (!publisher_.parentStagingOffer(acknowledgmentFile_)) endAcknowledgment();
        return;
    }
    const auto path = waitingPath();
    if (path.isEmpty()) {
        tryAssign();
        return;
    }
    const auto offer = publisher_.parentStagingOffer(path.toHex().toStdString());
    if (!offer || offer->path != path) return;
    const auto index = indexOfPath(path);
    if (index < 0) return;
    const auto &waiter = waiters_.at(index);
    // False means the launcher is already gone and Git restores the path.
    // Once the frame is queued its delivery is unknown: Git may stage the
    // path or not, so the offer is held until the index shows which.
    if (!server_.send(waiter.connection, ResolvedMessage{waiter.invocation, offer->revision, offer->canonicalObjectId}))
        return;
    offerHeld_ = true;
    acknowledgmentFile_ = path.toHex().toStdString();
    acknowledgmentRevision_ = offer->revision;
    startAcknowledgmentPolls();
    release(index);
    Q_EMIT changed();
}

void MergetoolSession::attachReceived(quint64 connection, const AttachMessage &message)
{
    // The parent stages with the configuration its Git reads; the release
    // preflight here must inspect the same files.
    if (message.configuration != config_.gitEnvironment) {
        reject(connection, message.invocation,
               tr("The Git that launched this path reads other configuration files than this window's Git "
                  "(HOME, XDG_CONFIG_HOME or GIT_CONFIG_*). Start git mergetool and Branch Truce from the same "
                  "environment."));
        return;
    }
    // Reading and hashing up to four copies is file I/O, so it runs off the GUI thread.
    runOffThread(
        workers_, this,
        [directory = message.intake, root = mergetoolIntakeRoot(config_.socketDirectory), key = sessionKey_] {
            IntakeCheck check;
            check.intake = readMergetoolIntake(directory, root, key, &check.error);
            return check;
        },
        [this, connection, message](const IntakeCheck &check) { intakeChecked(connection, message, check); });
}

void MergetoolSession::intakeChecked(quint64 connection, const AttachMessage &message, const IntakeCheck &check)
{
    if (!check.intake || check.intake->invocation != message.invocation) {
        reject(connection, message.invocation,
               check.error.isEmpty() ? tr("The mergetool inputs could not be kept.") : problem_text::text(check.error));
        return;
    }
    if (indexOfPath(check.intake->path) >= 0) {
        reject(connection, message.invocation, tr("Git is already waiting on this path."));
        return;
    }
    const auto *file = files_.file(check.intake->path.toHex().toStdString());
    if (!file || file->support() != Support::Supported) {
        reject(connection, message.invocation, tr("This path is not an editable conflict in the open session."));
        return;
    }
    // Reply before taking the waiter: a launcher that left meanwhile changes nothing.
    if (!server_.send(connection, AttachedMessage{message.invocation})) return;
    waiters_.append(
        Waiter{message.invocation, check.intake->path, message.intake, check.intake->mergedSha256, connection});
    parentSession_ = true;
    acceptCopiedLaunches();
    tryAssign();
    Q_EMIT notice(tr("Git is waiting. Mark resolved hands this path back, or leave it unresolved."));
    Q_EMIT changed();
}

void MergetoolSession::closed(quint64 connection)
{
    const auto index = indexOfConnection(connection);
    if (index < 0) return;
    const bool assigned = waiters_.at(index).assigned;
    waiters_.removeAt(index);
    if (assigned && !offerHeld_) {
        clearAssignment();
        tryAssign();
    }
    Q_EMIT notice(tr("Git's launcher closed before resolution. The draft is still in Branch Truce."));
    Q_EMIT changed();
}

void MergetoolSession::reject(quint64 connection, const QString &invocation, const QString &reason)
{
    server_.send(connection, RejectedMessage{invocation, reason});
    server_.close(connection);
}

void MergetoolSession::release(qsizetype index)
{
    // Out of the list before the close, so the close is not taken for a drop.
    const auto waiter = waiters_.takeAt(index);
    server_.close(waiter.connection);
    if (waiter.assigned && !offerHeld_) {
        clearAssignment();
        tryAssign();
    }
}

void MergetoolSession::tryAssign()
{
    if (assigning_ || offerHeld_ || publisher_.busy() || !waitingPath().isEmpty()) return;
    for (auto &waiter : waiters_) {
        if (waiter.assigned) continue;
        assigning_ = true;
        const bool assigned = publisher_.assignParentStaging(waiter.path);
        assigning_ = false;
        if (!assigned) return;
        waiter.assigned = true;
        Q_EMIT changed();
        return;
    }
}

void MergetoolSession::clearAssignment()
{
    assigning_ = true;
    publisher_.assignParentStaging({});
    assigning_ = false;
}

void MergetoolSession::startAcknowledgmentPolls()
{
    acknowledgmentUnknown_ = false;
    acknowledgmentPolls_ = 0;
    acknowledgmentInterval_ = kFirstAcknowledgmentPollMs;
    acknowledgment_->start(acknowledgmentInterval_);
}

void MergetoolSession::endAcknowledgment()
{
    offerHeld_ = false;
    acknowledgmentUnknown_ = false;
    acknowledgment_->stop();
    tryAssign();
    Q_EMIT changed();
}

void MergetoolSession::pollAcknowledgment()
{
    if (!offerHeld_) return;
    if (!publisher_.parentStagingOffer(acknowledgmentFile_)) {
        endAcknowledgment();
        return;
    }
    if (!publisher_.busy()) publisher_.verifyParentAcknowledgment(acknowledgmentFile_, acknowledgmentRevision_);
    if (!offerHeld_) return;
    if (++acknowledgmentPolls_ >= kAcknowledgmentPolls) {
        acknowledgmentUnknown_ = true;
        Q_EMIT notice(tr("Git has not staged the written result yet. Check Git staging once git mergetool is done."));
        Q_EMIT changed();
        return;
    }
    acknowledgmentInterval_ = std::min(acknowledgmentInterval_ * 2, kLastAcknowledgmentPollMs);
    acknowledgment_->start(acknowledgmentInterval_);
}

bool MergetoolSession::recheckAcknowledgment(const std::string &fileId)
{
    if (offerHeld_ && acknowledgmentFile_ == fileId) {
        const bool unknown = std::exchange(acknowledgmentUnknown_, false);
        acknowledgmentPolls_ = 0;
        acknowledgmentInterval_ = kFirstAcknowledgmentPollMs;
        acknowledgment_->stop();
        pollAcknowledgment();
        if (unknown && !acknowledgmentUnknown_) Q_EMIT changed();
        return true;
    }
    const auto offer = publisher_.parentStagingOffer(fileId);
    return offer && publisher_.verifyParentAcknowledgment(fileId, offer->revision);
}

qsizetype MergetoolSession::indexOfConnection(quint64 connection) const
{
    for (qsizetype index = 0; index < waiters_.size(); ++index)
        if (waiters_.at(index).connection == connection) return index;
    return -1;
}

qsizetype MergetoolSession::indexOfPath(const QByteArray &path) const
{
    for (qsizetype index = 0; index < waiters_.size(); ++index)
        if (waiters_.at(index).path == path) return index;
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
                                                 QCryptographicHash::Sha256)
                            .toHex();
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
        auto live = std::make_shared<storage::DestinationCapture>();
        const auto task = coordinator_.submit(
            fileId, file->current().id,
            [snapshots = *file->snapshots(), identity, repository = loadedRepository_(), git = config_.git, live,
             path = QByteArray::fromStdString(file->path()),
             files = publisher_.destinations()](const TaskInput &, const TaskControl &) {
                TaskResult result;
                result.problem = revalidateCopiedLaunch(snapshots, identity, repository, git, files);
                *live = storage::captureDestination(repository.worktreeRoot, path, files);
                return result;
            },
            [this, fileId, identity, live](const TaskCompletion &completion) {
                copiedLaunchChecks_.erase(fileId);
                settleCopiedLaunch(fileId, identity, completion.result.problem, *live);
            });
        if (task != 0) copiedLaunchChecks_.insert_or_assign(fileId, task);
        ++entry;
    }
}

void MergetoolSession::settleCopiedLaunch(const std::string &fileId, const SnapshotFileIdentity &identity,
                                          const Problem &failure, const storage::DestinationCapture &live)
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
    const bool settled = failure.isEmpty() && publisher_.adoptCopiedLaunch(fileId, identity, live)
        ? files_.settleRecoveryFailure(fileId, ProblemCode::RecoveryCopiedLaunch)
        : files_.settleRecoveryFailure(fileId, ProblemCode::RecoveryCopiedLaunch,
                                       !failure.isEmpty() ? failure
                                                          : Problem{ProblemCode::RecoveryChangedAfterValidation});
    copiedLaunches_.erase(entry);
    if (settled) Q_EMIT changed();
}
}
