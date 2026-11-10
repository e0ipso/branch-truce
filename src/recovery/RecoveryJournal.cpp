// The recovery journal keeps acknowledged edits durable and retains failed input for retry.
#include "recovery/RecoveryJournal.h"
#include "recovery/RecoveryRecord.h"
#include "storage/OffThread.h"
#include <QFileInfo>
#include <QUuid>
#include <algorithm>
#include <exception>
#include <utility>

namespace branch_truce {
RecoveryJournal::RecoveryJournal(QString directory, std::shared_ptr<const storage::FileSystem> files)
    : directory_(std::move(directory)), files_(std::move(files))
{
    // One worker writes records behind in queue order.
    worker_.setMaxThreadCount(1);
}

RecoveryJournal::~RecoveryJournal() { close(); }

void RecoveryJournal::commit(storage::RecoveryTransaction transaction, CommitCallback written)
{
    behind_.push_back(
        {{}, std::move(transaction), [this, written = std::move(written)](storage::RecoveryCommit result) {
             written(Receipt{std::move(result), lease_});
         }});
    writeNext();
}

void RecoveryJournal::commitBehind(const FileSession &candidate, RecoveryAction action, std::string_view previousText,
                                   Callback written)
{
    auto transaction =
        recoveryTransaction(candidate, action, QUuid::createUuid().toString(QUuid::WithoutBraces), previousText);
    behind_.push_back({Pending{candidate, action, transaction, {}}, std::move(transaction),
                       [written = std::move(written)](storage::RecoveryCommit result) {
                           if (written) written(result.succeeded ? Written::Committed : Written::Failed);
                       }});
    writeNext();
}

void RecoveryJournal::settleBehind(Receipt result)
{
    lease_ = std::move(result.lease);
    auto next = std::move(behind_.front());
    behind_.pop_front();
    if (result.commit.succeeded) {
        if (!next.record || next.record->action != RecoveryAction::Open) userWork_ = true;
    } else if (next.record) {
        next.record->problem = result.commit.problem;
        pending_.push_back(std::move(*next.record));
    }
    if (next.written) next.written(std::move(result.commit));
}

void RecoveryJournal::writeNext()
{
    while (!writing_ && !behind_.empty()) {
        // Any failed transaction holds the whole store, including other files.
        if (hasPending()) {
            settleBehind({{false, ProblemCode::TaskRecoveryBlocked}, lease_});
            continue;
        }
        const auto ticket = nextTicket_++;
        writing_ = Writing{ticket,
                           runOffThread(
                               worker_, &context_,
                               [directory = directory_, files = files_, lease = lease_,
                                transaction = behind_.front().transaction]() mutable {
                                   try {
                                       if (!lease) {
                                           auto failure = storage::RecoveryLease::Failure::None;
                                           lease = storage::RecoveryLease::acquire(directory, files, &failure);
                                           if (!lease)
                                               return Receipt{{false,
                                                               failure == storage::RecoveryLease::Failure::Held
                                                                   ? ProblemCode::RecoveryStoreHeld
                                                                   : ProblemCode::RecoveryStoreUnavailable},
                                                              {}};
                                       }
                                       return Receipt{lease->commit(transaction), lease};
                                   } catch (...) {
                                       return Receipt{{false, ProblemCode::RecoveryCommitFailed}, lease};
                                   }
                               },
                               [this, ticket](const Receipt &) { wroteBehind(ticket); })};
    }
}

void RecoveryJournal::wroteBehind(quint64 ticket)
{
    if (!writing_ || writing_->ticket != ticket) return;
    auto result = writing_->result.result();
    writing_.reset();
    settleBehind(std::move(result));
    writeNext();
}

bool RecoveryJournal::recording(const std::string &id) const
{
    return std::any_of(behind_.begin(), behind_.end(),
                       [&](const Behind &entry) { return entry.record && entry.record->candidate.id() == id; });
}

void RecoveryJournal::close()
{
    while (busy()) {
        writeNext();
        if (!writing_) continue;
        writing_->result.waitForFinished();
        wroteBehind(writing_->ticket);
    }
}

void RecoveryJournal::retryPending(const std::string &id, std::function<void(FileSession, RecoveryAction)> acknowledged,
                                   std::function<void()> written)
{
    // Retry the entire dependent queue in its original order, not per-file
    // order. Entries are moved back without re-encoding their transaction IDs.
    if (busy() || !pending(id)) {
        written();
        return;
    }
    auto records = std::exchange(pending_, {});
    for (auto &record : records) {
        auto candidate = record.candidate;
        const auto action = record.action;
        auto transaction = record.transaction;
        behind_.push_back({std::move(record), std::move(transaction),
                           [acknowledged, candidate, action](storage::RecoveryCommit result) {
                               if (result.succeeded) acknowledged(candidate, action);
                           }});
    }
    // The final queue entry is a barrier and cannot overtake any retry.
    auto &tail = behind_.back().written;
    tail = [tail = std::move(tail), written = std::move(written)](storage::RecoveryCommit result) {
        tail(result);
        written();
    };
    writeNext();
}

bool RecoveryJournal::startNewStore()
{
    if (busy()) return false;
    if (hasPending()) return false;
    directory_ =
        QFileInfo(directory_).absolutePath() + QLatin1Char('/') + QUuid::createUuid().toString(QUuid::WithoutBraces);
    worker_.start([lease = std::exchange(lease_, {})] {});
    userWork_ = false;
    return true;
}

void RecoveryJournal::reset(std::optional<AdoptedStore> adopted)
{
    // Records the caller queued belong in the store it is leaving.
    Q_ASSERT(!busy());
    pending_.clear();
    failures_.clear();
    if (!adopted || !adopted->lease) return;
    lease_ = std::move(adopted->lease);
    directory_ = lease_->directory();
    failures_ = std::move(adopted->failures);
    userWork_ = adopted->userWork;
}

QFuture<bool> RecoveryJournal::discardStore()
{
    if (busy() || hasPending() || hasFailures() || !lease_) return completedFuture(false);
    directory_.clear();
    userWork_ = false;
    return runOffThread(
        worker_, &context_,
        [lease = std::exchange(lease_, {})] {
            try {
                return lease->remove();
            } catch (...) {
                return false;
            }
        },
        [](bool) {});
}

const RecoveryJournal::Pending *RecoveryJournal::pending(const std::string &id) const
{
    const auto found = std::find_if(pending_.begin(), pending_.end(),
                                    [&](const Pending &record) { return record.candidate.id() == id; });
    return found == pending_.end() ? nullptr : &*found;
}

const FileSession *RecoveryJournal::pendingCandidate(const std::string &id, bool includeQueued) const
{
    if (includeQueued) {
        const auto queued = std::find_if(behind_.rbegin(), behind_.rend(), [&](const Behind &entry) {
            return entry.record && entry.record->candidate.id() == id;
        });
        if (queued != behind_.rend()) return &queued->record->candidate;
    }
    const auto found = std::find_if(pending_.rbegin(), pending_.rend(),
                                    [&](const Pending &record) { return record.candidate.id() == id; });
    return found == pending_.rend() ? nullptr : &found->candidate;
}

bool RecoveryJournal::onlyOpenCapturesPending() const
{
    return std::all_of(pending_.cbegin(), pending_.cend(),
                       [](const auto &record) { return record.action == RecoveryAction::Open; });
}

const Problem *RecoveryJournal::failure(const std::string &id) const
{
    const auto found = failures_.find(id);
    return found == failures_.end() ? nullptr : &found->second;
}

void RecoveryJournal::recordFailure(const std::string &id, const Problem &problem)
{
    failures_.insert_or_assign(id, problem);
}

bool RecoveryJournal::settleFailure(const std::string &id, const Problem &expected, const Problem &replacement)
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
