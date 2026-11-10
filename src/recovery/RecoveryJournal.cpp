// The recovery journal keeps acknowledged edits durable and retains failed input for retry.
#include "recovery/RecoveryJournal.h"
#include "recovery/RecoveryRecord.h"
#include <QFileInfo>
#include <QMetaObject>
#include <QPromise>
#include <QUuid>
#include <algorithm>
#include <exception>

namespace branch_truce {
RecoveryJournal::RecoveryJournal(QString directory, std::shared_ptr<const storage::FileSystem> files)
    : directory_(std::move(directory)), files_(std::move(files))
{
    // One worker writes records behind in queue order.
    worker_.setMaxThreadCount(1);
}

// The worker goes first and finishes the record it is writing; its outcome
// then has no context left to reach.
RecoveryJournal::~RecoveryJournal() = default;

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

void RecoveryJournal::committed(RecoveryAction action)
{
    if (action != RecoveryAction::Open) userWork_ = true;
}

storage::RecoveryCommit RecoveryJournal::commit(const storage::RecoveryTransaction &transaction)
{
    finishBehind();
    auto result = commitUnderLease(transaction);
    if (result.succeeded) userWork_ = true;
    return result;
}

bool RecoveryJournal::commitCandidate(const FileSession &candidate, RecoveryAction action,
                                      std::string_view previousText)
{
    finishBehind();
    auto transaction =
        recoveryTransaction(candidate, action, QUuid::createUuid().toString(QUuid::WithoutBraces), previousText);
    const auto result = commitUnderLease(transaction);
    if (result.succeeded) {
        committed(action);
        return true;
    }
    pending_[candidate.id()].push_back(Pending{candidate, action, std::move(transaction), result.error});
    return false;
}

void RecoveryJournal::commitBehind(const FileSession &candidate, RecoveryAction action, std::string_view previousText,
                                   Callback written)
{
    auto transaction =
        recoveryTransaction(candidate, action, QUuid::createUuid().toString(QUuid::WithoutBraces), previousText);
    behind_.push_back({Pending{candidate, action, std::move(transaction), {}}, std::move(written)});
    writeNext();
}

void RecoveryJournal::holdBehind()
{
    auto next = std::move(behind_.front());
    behind_.pop_front();
    pending_[next.record.candidate.id()].push_back(std::move(next.record));
    if (next.written) next.written(Written::Held);
}

void RecoveryJournal::settleBehind(const storage::RecoveryCommit &result)
{
    auto next = std::move(behind_.front());
    behind_.pop_front();
    const auto action = next.record.action;
    if (result.succeeded) {
        committed(action);
    } else {
        next.record.error = result.error;
        pending_[next.record.candidate.id()].push_back(std::move(next.record));
    }
    if (next.written) next.written(result.succeeded ? Written::Committed : Written::Failed);
}

void RecoveryJournal::writeNext()
{
    while (!writing_ && !finishing_ && !behind_.empty()) {
        if (pending_.contains(behind_.front().record.candidate.id())) {
            holdBehind();
            continue;
        }
        if (const auto leaseError = ensureLease(); !leaseError.isEmpty()) {
            settleBehind({false, leaseError});
            continue;
        }
        const auto ticket = nextTicket_++;
        auto promise = std::make_shared<QPromise<storage::RecoveryCommit>>();
        writing_ = Writing{ticket, promise->future()};
        promise->start();
        worker_.start([this, ticket, promise, lease = lease_, transaction = behind_.front().record.transaction] {
            storage::RecoveryCommit result;
            try {
                result = lease->commit(transaction);
            } catch (const std::exception &) {
                result = {false, tr("Recovery commit failed; pending input is retained for retry.")};
            }
            promise->addResult(result);
            promise->finish();
            // Dropped if the journal, and with it the context, is gone.
            QMetaObject::invokeMethod(&context_, [this, ticket] { wroteBehind(ticket); }, Qt::QueuedConnection);
        });
    }
}

void RecoveryJournal::wroteBehind(quint64 ticket)
{
    // finishBehind() may already have settled it.
    if (!writing_ || writing_->ticket != ticket) return;
    const auto result = writing_->result.result();
    writing_.reset();
    settleBehind(result);
    writeNext();
}

void RecoveryJournal::finishBehind()
{
    if (finishing_) return;
    finishing_ = true;
    if (writing_) {
        writing_->result.waitForFinished();
        const auto result = writing_->result.result();
        writing_.reset();
        settleBehind(result);
    }
    while (!behind_.empty()) {
        if (pending_.contains(behind_.front().record.candidate.id()))
            holdBehind();
        else
            settleBehind(commitUnderLease(behind_.front().record.transaction));
    }
    finishing_ = false;
}

std::optional<FileSession> RecoveryJournal::retryPending(const std::string &id)
{
    finishBehind();
    const auto queue = pending_.find(id);
    if (queue == pending_.end()) return {};
    std::optional<FileSession> last;
    auto &records = queue->second;
    while (!records.empty()) {
        auto &next = records.front();
        const auto result = commitUnderLease(next.transaction);
        if (!result.succeeded) {
            next.error = result.error;
            return last;
        }
        committed(next.action);
        last = std::move(next.candidate);
        records.pop_front();
    }
    pending_.erase(queue);
    return last;
}

bool RecoveryJournal::startNewStore()
{
    finishBehind();
    if (hasPending()) return false;
    directory_ =
        QFileInfo(directory_).absolutePath() + QLatin1Char('/') + QUuid::createUuid().toString(QUuid::WithoutBraces);
    lease_.reset();
    userWork_ = false;
    return true;
}

void RecoveryJournal::reset(std::optional<AdoptedStore> adopted)
{
    // Records the caller queued belong in the store it is leaving.
    finishBehind();
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
    finishBehind();
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
    return found == pending_.end() ? nullptr : &found->second.front();
}

const FileSession *RecoveryJournal::pendingCandidate(const std::string &id) const
{
    const auto found = pending_.find(id);
    return found == pending_.end() ? nullptr : &found->second.back().candidate;
}

bool RecoveryJournal::onlyOpenCapturesPending() const
{
    return std::all_of(pending_.cbegin(), pending_.cend(), [](const auto &queue) {
        return std::all_of(queue.second.cbegin(), queue.second.cend(),
                           [](const auto &pending) { return pending.action == RecoveryAction::Open; });
    });
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
