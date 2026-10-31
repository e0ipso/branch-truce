// The recovery journal keeps acknowledged edits durable and retains failed input for retry.
#include "recovery/RecoveryJournal.h"
#include "recovery/RecoveryRecord.h"
#include "recovery/RecoveryRestore.h"
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QUuid>
#include <algorithm>
#include <unistd.h>

namespace branch_truce {
RecoveryJournal::RecoveryJournal(QString directory) : store_(std::move(directory)) {}

QString RecoveryJournal::ensureLease()
{
    if (lease_) return {};
    const auto &directory = store_.directory();
    const QFileInfo before(directory);
    const auto permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    if (!QDir::isAbsolutePath(directory) || before.isSymLink()
        || (before.exists() && (!before.isDir() || before.ownerId() != ::geteuid()))
        || !QDir().mkpath(directory, permissions)) return tr("Recovery storage is unavailable or unsafe.");
    auto lease = std::make_shared<QLockFile>(directory + QStringLiteral("/session.lock"));
    if (!lease->tryLock(0)) return tr("Recovery session is already open or unavailable; close its other window before retrying.");
    lease_ = std::move(lease);
    return {};
}

storage::RecoveryCommit RecoveryJournal::commit(const storage::RecoveryTransaction &transaction)
{
    const auto leaseError = ensureLease();
    if (!leaseError.isEmpty()) return storage::RecoveryCommit{false, leaseError};
    auto result = store_.commit(transaction);
    if (result.succeeded && transaction.state.value("action").toString() != QLatin1String("open")) userWork_ = true;
    return result;
}

bool RecoveryJournal::commitCandidate(const FileSession &candidate, const QString &action)
{
    const auto transactionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto result = commit(recoveryTransaction(candidate, action, transactionId));
    if (result.succeeded) return true;
    pending_.insert_or_assign(candidate.id(), Pending{candidate, action, transactionId, result.error});
    return false;
}

std::optional<FileSession> RecoveryJournal::retryPending(const std::string &id)
{
    const auto pending = pending_.find(id);
    if (pending == pending_.end()) return {};
    const auto result = commit(recoveryTransaction(pending->second.candidate,
        pending->second.action, pending->second.transactionId));
    if (!result.succeeded) {
        pending->second.error = result.error;
        return {};
    }
    auto committed = std::move(pending->second.candidate);
    pending_.erase(pending);
    return committed;
}

bool RecoveryJournal::startNewStore()
{
    if (hasPending()) return false;
    store_ = storage::RecoveryStore(QFileInfo(store_.directory()).absolutePath() + QLatin1Char('/')
        + QUuid::createUuid().toString(QUuid::WithoutBraces));
    lease_.reset();
    userWork_ = false;
    return true;
}

void RecoveryJournal::reset(RecoveryRestore *restore)
{
    pending_.clear();
    failures_.clear();
    if (!restore) return;
    lease_ = std::move(restore->lease);
    store_ = storage::RecoveryStore(restore->directory);
    failures_ = std::move(restore->failures);
    userWork_ = restore->userWork;
}

const RecoveryJournal::Pending *RecoveryJournal::pending(const std::string &id) const
{
    const auto found = pending_.find(id);
    return found == pending_.end() ? nullptr : &found->second;
}

const QString *RecoveryJournal::failure(const std::string &id) const
{
    const auto found = failures_.find(id);
    return found == failures_.end() ? nullptr : &found->second;
}

void RecoveryJournal::recordFailure(const std::string &id, const QString &error)
{
    failures_.insert_or_assign(id, error);
}

bool RecoveryJournal::settleFailure(const std::string &id, const QString &expected, const QString &replacement)
{
    const auto failure = failures_.find(id);
    if (failure == failures_.end() || failure->second != expected) return false;
    if (replacement.isEmpty()) failures_.erase(failure);
    else failure->second = replacement;
    return true;
}

bool RecoveryJournal::canCompleteLoad(const std::vector<FileSession> &current,
                                      const std::vector<FileSession> &loaded) const
{
    if (loaded.size() != current.size()) return false;
    for (std::size_t row = 0; row < current.size(); ++row) {
        const auto &file = current[row];
        if (file.id() != loaded[row].id() || file.path() != loaded[row].path()
            || file.support() != Support::Unreadable || file.snapshots()
            || file.current().id != 1 || file.current().draft != Draft{}
            || file.checkpoint() || file.reviewed() || file.written() || file.staged()
            || file.checkpointOutcome() || file.writeOutcome() || file.stageOutcome()) return false;
    }
    return std::all_of(pending_.cbegin(), pending_.cend(), [](const auto &pending) {
        return pending.second.action == QLatin1String("open");
    });
}
}
