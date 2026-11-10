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
RecoveryJournal::RecoveryJournal(QString directory, std::shared_ptr<const storage::FileSystem> files,
                                 storage::RecoveryLimits storageLimits)
    : directory_(std::move(directory)), files_(std::move(files)), storageLimits_(storageLimits)
{
    // One worker writes records behind in queue order.
    worker_.setMaxThreadCount(1);
}

RecoveryJournal::~RecoveryJournal() { close(); }

bool RecoveryJournal::canAdmit(std::size_t additional) const
{
    return entries_ < limits_.count && additional < limits_.count - entries_ && retained_ <= limits_.bytes
        && additional < (limits_.bytes - retained_) / retainedBytes;
}

void RecoveryJournal::commit(std::function<storage::RecoveryTransaction()> transaction, CommitCallback written)
{
    if (!canAdmit()) return written({{false, ProblemCode::RecoveryResourceLimit}, lease_});
    ++entries_;
    retained_ += retainedBytes;
    behind_.push_back(std::make_shared<Behind>(Behind{
        {}, {}, std::move(transaction), {}, [this, written = std::move(written)](storage::RecoveryCommit result) {
            written(Receipt{std::move(result), lease_});
        }}));
    writeNext();
}

RecoveryJournal::Record RecoveryJournal::commitBehind(const FileSession &candidate, RecoveryAction action,
                                                      const FileSession *previous, Callback written, Command command,
                                                      Record predecessor)
{
    if (!canAdmit()) return {};
    auto record =
        std::make_shared<Pending>(Pending{candidate, action, {}, {}, Transition::Applied, candidate.current().id, {}});
    const auto transactionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // Copying a session shares immutable text/maps. The mutable candidate,
    // command validation, comparisons and all encoding belong to the worker.
    record->prepare = [before = previous ? std::optional<FileSession>(*previous) : std::nullopt,
                       command = std::move(command), predecessor = std::move(predecessor),
                       transactionId](Pending &record) mutable {
        if (command) {
            if (predecessor) record.candidate = predecessor->candidate;
            before = record.candidate;
            record.inputRevision = record.candidate.current().id;
            record.transition = command(record.candidate);
            command = {};
            predecessor.reset();
        }
        if (record.transition != Transition::Applied) return;
        record.transaction = recoveryTransaction(record.candidate, record.action, transactionId,
                                                 before ? before->current().draft.text : std::string_view{});
    };
    ++entries_;
    retained_ += retainedBytes;
    behind_.push_back(std::make_shared<Behind>(
        Behind{record, candidate.id(), {}, {}, [record, written = std::move(written)](storage::RecoveryCommit result) {
                   written(record,
                           record->transition != Transition::Applied ? record->transition
                               : result.succeeded                    ? Transition::Applied
                                                                     : Transition::Blocked);
               }}));
    writeNext();
    return record;
}

void RecoveryJournal::settleBehind(Receipt result)
{
    lease_ = std::move(result.lease);
    auto next = std::move(behind_.front());
    behind_.pop_front();
    const bool failed = !result.commit.succeeded && next->record && next->record->transition == Transition::Applied;
    if (result.commit.succeeded) {
        if (!next->record || next->record->action != RecoveryAction::Open) userWork_ = true;
    } else if (failed) {
        next->record->problem = result.commit.problem;
        pending_.push_back(next->record);
    }
    if (!failed) {
        --entries_;
        retained_ -= retainedBytes;
    }
    if (next->written) next->written(std::move(result.commit));
}

void RecoveryJournal::writeNext()
{
    if (writing_ || behind_.empty()) return;
    const auto ticket = nextTicket_++;
    auto terminal = std::make_shared<Receipt>();
    auto entry = behind_.front();
    auto work = [directory = directory_, files = files_, lease = lease_, storageLimits = storageLimits_, entry,
                 blocked = hasPending()]() mutable {
        try {
            if (entry->record && entry->record->prepare) {
                entry->record->prepare(*entry->record);
                entry->record->prepare = {};
            }
            if (entry->record && entry->record->transition != Transition::Applied) return Receipt{{false, {}}, lease};
            if (blocked) return Receipt{{false, ProblemCode::TaskRecoveryBlocked}, lease};
            if (!lease) {
                auto failure = storage::RecoveryLease::Failure::None;
                lease = storage::RecoveryLease::acquire(directory, files, &failure, storageLimits);
                if (!lease)
                    return Receipt{{false,
                                    failure == storage::RecoveryLease::Failure::Held
                                        ? ProblemCode::RecoveryStoreHeld
                                        : ProblemCode::RecoveryStoreUnavailable},
                                   {}};
            }
            if (entry->prepare) entry->transaction = entry->prepare();
            return Receipt{lease->commit(entry->record ? entry->record->transaction : entry->transaction), lease};
        } catch (...) {
            return Receipt{{false, ProblemCode::RecoveryCommitFailed}, lease};
        }
    };
    writing_ =
        Writing{ticket,
                runOffThread(
                    worker_, &context_, [work = std::move(work), terminal]() mutable { return *terminal = work(); },
                    [this, ticket](const Receipt &) { wroteBehind(ticket); },
                    [terminal, lease = lease_](std::exception_ptr) noexcept {
                        return *terminal = Receipt{{false, ProblemCode::RecoveryCommitFailed}, lease};
                    }),
                terminal};
}

void RecoveryJournal::wroteBehind(quint64 ticket)
{
    if (!writing_ || writing_->ticket != ticket) return;
    auto result = std::move(*writing_->terminal);
    writing_.reset();
    settleBehind(std::move(result));
    writeNext();
}

bool RecoveryJournal::recording(const std::string &id) const
{
    return std::any_of(behind_.begin(), behind_.end(),
                       [&](const auto &entry) { return entry->record && entry->id == id; });
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
        behind_.push_back(std::make_shared<Behind>(Behind{record,
                                                          record->candidate.id(),
                                                          {},
                                                          {},
                                                          [acknowledged, record](storage::RecoveryCommit result) {
                                                              if (result.succeeded)
                                                                  acknowledged(record->candidate, record->action);
                                                          },
                                                          std::make_shared<const FileSession>(record->candidate)}));
    }
    auto &tail = behind_.back()->written;
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
    retained_ = entries_ = 0;
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
        [](bool) {}, [](std::exception_ptr) noexcept { return false; });
}

const RecoveryJournal::Pending *RecoveryJournal::pending(const std::string &id) const
{
    const auto found = std::find_if(pending_.begin(), pending_.end(),
                                    [&](const auto &record) { return record->candidate.id() == id; });
    return found == pending_.end() ? nullptr : found->get();
}

const FileSession *RecoveryJournal::pendingCandidate(const std::string &id, bool includeQueued) const
{
    if (includeQueued) {
        const auto queued = std::find_if(behind_.rbegin(), behind_.rend(),
                                         [&](const auto &entry) { return entry->visible && entry->id == id; });
        if (queued != behind_.rend()) return (*queued)->visible.get();
    }
    const auto found = std::find_if(pending_.rbegin(), pending_.rend(),
                                    [&](const auto &record) { return record->candidate.id() == id; });
    return found == pending_.rend() ? nullptr : &(*found)->candidate;
}

bool RecoveryJournal::onlyOpenCapturesPending() const
{
    return std::all_of(pending_.cbegin(), pending_.cend(),
                       [](const auto &record) { return record->action == RecoveryAction::Open; });
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
