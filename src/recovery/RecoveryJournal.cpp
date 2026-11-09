// The recovery journal keeps acknowledged edits durable and retains failed input for retry.
#include "recovery/RecoveryJournal.h"
#include "recovery/RecoveryRecord.h"
#include <QFileInfo>
#include <QUuid>
#include <algorithm>

namespace branch_truce {
RecoveryJournal::RecoveryJournal(QString directory, std::shared_ptr<const storage::FileSystem> files)
    : directory_(std::move(directory)), files_(std::move(files))
{}

QString RecoveryJournal::ensureLease()
{
    if (lease_) return {};
    auto failure = storage::RecoveryLease::Failure::None;
    lease_ = storage::RecoveryLease::acquire(directory_, files_, &failure);
    if (lease_) return {};
    return failure == storage::RecoveryLease::Failure::Held
        ? tr("Recovery session is already open or unavailable; close its other window before retrying.")
        : tr("Recovery storage is unavailable or unsafe.");
}

storage::RecoveryCommit RecoveryJournal::commitUnderLease(const storage::RecoveryTransaction &transaction)
{
    const auto leaseError = ensureLease();
    if (!leaseError.isEmpty()) return storage::RecoveryCommit{false, leaseError};
    return lease_->commit(transaction);
}

storage::RecoveryCommit RecoveryJournal::commit(const storage::RecoveryTransaction &transaction)
{
    auto result = commitUnderLease(transaction);
    if (result.succeeded) userWork_ = true;
    return result;
}

bool RecoveryJournal::commitCandidate(const FileSession &candidate, RecoveryAction action,
                                      std::string_view previousText)
{
    auto transaction =
        recoveryTransaction(candidate, action, QUuid::createUuid().toString(QUuid::WithoutBraces), previousText);
    const auto result = commitUnderLease(transaction);
    if (result.succeeded) {
        if (action != RecoveryAction::Open) userWork_ = true;
        return true;
    }
    pending_.insert_or_assign(candidate.id(), Pending{candidate, action, std::move(transaction), result.error});
    return false;
}

std::optional<FileSession> RecoveryJournal::retryPending(const std::string &id)
{
    const auto pending = pending_.find(id);
    if (pending == pending_.end()) return {};
    const auto result = commitUnderLease(pending->second.transaction);
    if (!result.succeeded) {
        pending->second.error = result.error;
        return {};
    }
    if (pending->second.action != RecoveryAction::Open) userWork_ = true;
    auto committed = std::move(pending->second.candidate);
    pending_.erase(pending);
    return committed;
}

bool RecoveryJournal::startNewStore()
{
    if (hasPending()) return false;
    directory_ =
        QFileInfo(directory_).absolutePath() + QLatin1Char('/') + QUuid::createUuid().toString(QUuid::WithoutBraces);
    lease_.reset();
    userWork_ = false;
    return true;
}

void RecoveryJournal::reset(std::optional<AdoptedStore> adopted)
{
    pending_.clear();
    failures_.clear();
    if (!adopted || !adopted->lease) return;
    lease_ = std::move(adopted->lease);
    directory_ = lease_->directory();
    failures_ = std::move(adopted->failures);
    userWork_ = adopted->userWork;
}

bool RecoveryJournal::discardStore()
{
    if (hasPending() || hasFailures() || !ensureLease().isEmpty()) return false;
    const bool removed = lease_->remove();
    lease_.reset();
    userWork_ = false;
    // Nothing commits after this: a later command stays pending with an error.
    directory_.clear();
    return removed;
}

const RecoveryJournal::Pending *RecoveryJournal::pending(const std::string &id) const
{
    const auto found = pending_.find(id);
    return found == pending_.end() ? nullptr : &found->second;
}

bool RecoveryJournal::onlyOpenCapturesPending() const
{
    return std::all_of(pending_.cbegin(), pending_.cend(),
                       [](const auto &pending) { return pending.second.action == RecoveryAction::Open; });
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
    if (replacement.isEmpty())
        failures_.erase(failure);
    else
        failure->second = replacement;
    return true;
}
}
