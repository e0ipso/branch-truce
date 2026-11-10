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

// Serial persistence for one store. Commands and typing share one FIFO. A failed
// transaction holds every dependent entry until retry, under the original IDs.
// Callbacks run on the owning thread; only close() waits, during destruction.
class RecoveryJournal {
public:
    struct Pending {
        FileSession candidate;
        RecoveryAction action;
        storage::RecoveryTransaction transaction;
        // The commit failure, or why an earlier failed record held this one.
        Problem problem;
        Transition transition = Transition::Applied;
        RevisionId inputRevision = 0;
        std::function<void(Pending &)> prepare;
    };
    // A durable record, or input retained after a failed or held transaction.
    using Record = std::shared_ptr<Pending>;
    using Callback = std::function<void(const Record &, Transition)>;
    using Command = std::function<Transition(FileSession &)>;
    struct Limits {
        std::size_t count = 32;
        std::size_t bytes = std::size_t{4} * 1024 * 1024 * 1024;
    };
    // History, protected revisions, captures, derived maps and encoding copies.
    // Shared storage is charged again: this deliberately overestimates retention.
    static constexpr std::size_t retainedBytes =
        kMaxHistoryBytes + 16 * kMaxMetadataBytes + 32 * kMaxRecoveryPayloadBytes;
    void setLimits(Limits limits) { limits_ = limits; }
    [[nodiscard]] bool canAdmit(std::size_t additional = 0) const;

    RecoveryJournal(QString directory, std::shared_ptr<const storage::FileSystem> files);
    ~RecoveryJournal();
    RecoveryJournal(const RecoveryJournal &) = delete;
    RecoveryJournal &operator=(const RecoveryJournal &) = delete;

    [[nodiscard]] const QString &directory() const { return directory_; }
    [[nodiscard]] std::shared_ptr<storage::RecoveryLease> lease() const { return lease_; }
    struct Receipt {
        storage::RecoveryCommit commit;
        std::shared_ptr<storage::RecoveryLease> lease;
    };
    using CommitCallback = std::function<void(Receipt)>;
    void commit(std::function<storage::RecoveryTransaction()> transaction, CommitCallback written);
    // Refusal happens before copying a snapshot or encoding it. The caller
    // retains uncommitted typing and retries admission when capacity returns.
    Record commitBehind(const FileSession &candidate, RecoveryAction action, const FileSession *previous,
                        Callback written, Command command = {}, Record predecessor = {});
    void retryPending(const std::string &id, std::function<void(FileSession, RecoveryAction)> acknowledged,
                      std::function<void()> written);
    [[nodiscard]] bool recording(const std::string &id) const;
    [[nodiscard]] bool busy() const { return writing_.has_value() || !behind_.empty(); }
    // Shutdown drains admitted records on the owned worker. There is no time
    // limit: an acknowledged record is never abandoned for a shutdown deadline.
    void close();
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
    QFuture<bool> discardStore();

    [[nodiscard]] bool hasPending() const { return !pending_.empty(); }
    // The oldest pending record of a file, whose action and error stand for
    // its queue, or null.
    [[nodiscard]] const Pending *pending(const std::string &id) const;
    // The file as its newest pending record would leave it, or null.
    [[nodiscard]] const FileSession *pendingCandidate(const std::string &id, bool includeQueued = false) const;
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
        Record record;
        std::string id;
        std::function<storage::RecoveryTransaction()> prepare;
        storage::RecoveryTransaction transaction;
        std::function<void(storage::RecoveryCommit)> written;
        std::shared_ptr<const FileSession> visible = {};
    };
    void writeNext();
    void settleBehind(Receipt result);
    void wroteBehind(quint64 ticket);

    QString directory_;
    std::shared_ptr<const storage::FileSystem> files_;
    std::shared_ptr<storage::RecoveryLease> lease_;
    std::deque<Record> pending_;
    std::map<std::string, Problem> failures_;
    bool userWork_ = false;
    // Records written behind, oldest first. The worker writes the first one
    // while `writing_` names it.
    std::deque<std::shared_ptr<Behind>> behind_;
    Limits limits_;
    std::size_t retained_ = 0;
    std::size_t entries_ = 0;
    struct Writing {
        quint64 ticket;
        QFuture<Receipt> result;
        std::shared_ptr<Receipt> terminal;
    };
    std::optional<Writing> writing_;
    quint64 nextTicket_ = 1;

    // Receives the worker's outcomes on this journal's thread. Declared before
    // the worker so the worker is gone before it.
    QObject context_;
    QThreadPool worker_;
};
}
