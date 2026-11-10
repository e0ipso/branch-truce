// The recovery journal keeps acknowledged edits durable and retains failed input for retry.
#pragma once

#include "core/SessionState.h"
#include "recovery/RecoveryNames.h"
#include "storage/RecoveryStore.h"
#include <QFuture>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace branch_truce {

// A store a restore adopted: its lease, whether it holds a record other than a
// launch capture, and the recovery errors of its files.
struct AdoptedStore {
    std::shared_ptr<storage::RecoveryLease> lease;
    bool userWork = false;
    std::map<std::string, Problem> failures;
};

// One session's recovery store and what waits on it: the store's lease,
// records written behind the caller, input whose commit failed and per-file
// recovery errors. Callers acknowledge input only after its transaction
// commits. A failed commit keeps the encoded transaction, so a retry appends it
// at most once.
//
// Each file has one pending queue, oldest first. A failed record waits there
// with its error, and every later record of that file joins it without being
// tried, so retry appends them in their original order and nothing is dropped
// or reordered. Records written behind go to a worker one at a time, in the
// order they were queued; any other commit first writes them on the calling
// thread, so the journal keeps the order in which the caller made them.
// Used on one thread, the one that made it.
class RecoveryJournal {
public:
    struct Pending {
        FileSession candidate;
        RecoveryAction action;
        storage::RecoveryTransaction transaction;
        // Why the commit failed; empty for a record held behind a failed one.
        Problem problem;
    };
    // How a record written behind ended: durable, failed and kept pending, or
    // kept pending untried behind an earlier failed record of its file.
    enum class Written { Committed, Failed, Held };
    using Callback = std::function<void(Written)>;

    RecoveryJournal(QString directory, std::shared_ptr<const storage::FileSystem> files);
    // Waits for the record a worker is writing. Records still queued behind
    // are dropped; callers write them first with finishBehind().
    ~RecoveryJournal();
    RecoveryJournal(const RecoveryJournal &) = delete;
    RecoveryJournal &operator=(const RecoveryJournal &) = delete;

    [[nodiscard]] const QString &directory() const { return directory_; }
    // Takes the store's session lease on first use. Empty while the lease is
    // held, otherwise the reason the store is unavailable.
    Problem ensureLease();
    // The held lease, or null. A writer that must finish in this store after
    // the journal moves on, such as a running mutation, keeps it.
    [[nodiscard]] std::shared_ptr<storage::RecoveryLease> lease() const { return lease_; }
    // Commits a record that is not a file command, such as a mutation fact,
    // under the lease, without retaining anything on failure.
    [[nodiscard]] storage::RecoveryCommit commit(const storage::RecoveryTransaction &transaction);
    // Commits the candidate as a new transaction. previousText is the file's
    // text as the journal's last record of it left it. On failure the
    // candidate joins the file's pending queue and this returns false.
    [[nodiscard]] bool commitCandidate(const FileSession &candidate, RecoveryAction action,
                                       std::string_view previousText);
    // Queues the candidate's record to be written on a worker and returns at
    // once; previousText is as for commitCandidate. `written` runs on this
    // journal's thread once the record is durable or kept pending, in the
    // order records were queued, and may queue more records.
    void commitBehind(const FileSession &candidate, RecoveryAction action, std::string_view previousText,
                      Callback written);
    // Writes every record queued behind now, on this thread, after the one a
    // worker is writing, and runs their callbacks.
    void finishBehind();
    // Commits a file's pending queue in order under the records' original
    // transactions and returns the file as the last committed record left it.
    // Empty when nothing was pending or the first commit failed again. A
    // failure keeps that record, with the new error, and every later one
    // pending; pending() tells whether the whole queue committed.
    [[nodiscard]] std::optional<FileSession> retryPending(const std::string &id);
    // Moves to a new store beside the current one and releases the current
    // store's lease. Refused while input is pending.
    bool startNewStore();
    // Drops pending input and errors. With an adopted store, moves to it and
    // takes its lease, its per-file errors and its user-work flag.
    void reset(std::optional<AdoptedStore> adopted = std::nullopt);
    // Whether the store holds a record other than a launch capture, as a
    // replay's userWork reports, known without reading it: set when such a
    // record commits here, taken from the store this adopts, and cleared by a
    // new store.
    [[nodiscard]] bool holdsUserWork() const { return userWork_; }
    // Deletes the store once the session needs it no more: nothing is pending
    // and no file has a recovery error. Refused otherwise. Afterwards nothing
    // can be committed.
    bool discardStore();

    [[nodiscard]] bool hasPending() const { return !pending_.empty(); }
    // The oldest pending record of a file, whose action and error stand for
    // its queue, or null.
    [[nodiscard]] const Pending *pending(const std::string &id) const;
    // The file as its newest pending record would leave it, or null.
    [[nodiscard]] const FileSession *pendingCandidate(const std::string &id) const;
    // Whether every pending input is a launch capture.
    [[nodiscard]] bool onlyOpenCapturesPending() const;
    [[nodiscard]] bool hasFailures() const { return !failures_.empty(); }
    [[nodiscard]] const Problem *failure(const std::string &id) const;
    void recordFailure(const std::string &id, const Problem &problem);
    // Replaces a file's error only while it is still exactly expected; an
    // empty replacement clears it. Returns whether it changed.
    bool settleFailure(const std::string &id, const Problem &expected, const Problem &replacement);

private:
    struct Behind {
        Pending record;
        Callback written;
    };
    [[nodiscard]] storage::RecoveryCommit commitUnderLease(const storage::RecoveryTransaction &transaction);
    // Notes a durable record of action.
    void committed(RecoveryAction action);
    // Starts the next record queued behind on the worker, unless one is being
    // written or finishBehind() is writing them.
    void writeNext();
    // Settles the oldest record queued behind with result and runs its callback.
    void settleBehind(const storage::RecoveryCommit &result);
    // Moves the oldest record queued behind to its file's pending queue
    // untried, because an earlier record of that file is pending.
    void holdBehind();
    // The worker's outcome of the record it was given as `ticket`.
    void wroteBehind(quint64 ticket);

    QString directory_;
    std::shared_ptr<const storage::FileSystem> files_;
    std::shared_ptr<storage::RecoveryLease> lease_;
    std::map<std::string, std::deque<Pending>> pending_;
    std::map<std::string, Problem> failures_;
    bool userWork_ = false;
    // Records written behind, oldest first. The worker writes the first one
    // while `writing_` names it.
    std::deque<Behind> behind_;
    struct Writing {
        quint64 ticket;
        QFuture<storage::RecoveryCommit> result;
    };
    std::optional<Writing> writing_;
    quint64 nextTicket_ = 1;
    bool finishing_ = false;
    // Receives the worker's outcomes on this journal's thread. Declared before
    // the worker so the worker is gone before it.
    QObject context_;
    QThreadPool worker_;
};
}
