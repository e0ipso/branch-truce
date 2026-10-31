// The recovery journal keeps acknowledged edits durable and retains failed input for retry.
#pragma once

#include "core/SessionState.h"
#include "storage/RecoveryStore.h"
#include <QCoreApplication>
#include <QString>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QLockFile;

namespace branch_truce {
struct RecoveryRestore;

// One session's recovery store and what waits on it: the store's lease,
// input whose commit failed and per-file recovery errors. Callers acknowledge
// input only after its transaction commits. A failed commit keeps the
// candidate with its transaction id, so a retry never appends it twice.
class RecoveryJournal {
    Q_DECLARE_TR_FUNCTIONS(RecoveryJournal)
public:
    struct Pending {
        FileSession candidate;
        QString action;
        QString transactionId;
        QString error;
    };

    explicit RecoveryJournal(QString directory);
    [[nodiscard]] const QString &directory() const { return store_.directory(); }
    // Takes the store's session lease on first use. Empty while the lease is
    // held, otherwise the reason the store is unavailable.
    QString ensureLease();
    // Commits under the lease, without retaining anything on failure.
    [[nodiscard]] storage::RecoveryCommit commit(const storage::RecoveryTransaction &transaction);
    // Commits the candidate as a new transaction. On failure it becomes the
    // file's pending input, replacing any earlier one, and this returns false.
    [[nodiscard]] bool commitCandidate(const FileSession &candidate, const QString &action);
    // Commits a file's pending input under its original transaction id and
    // returns it. Empty when nothing is pending or the commit failed again;
    // a failure keeps the input with the new error.
    [[nodiscard]] std::optional<FileSession> retryPending(const std::string &id);
    // Moves to a new store beside the current one and releases the current
    // store's lease. Refused while input is pending.
    bool startNewStore();
    // Drops pending input and errors. With a restore, adopts its store, its
    // lease and its per-file errors.
    void reset(RecoveryRestore *restore = nullptr);
    // Whether the store holds a record other than a launch capture, as a
    // replay's userWork reports, known without reading it: set when such a
    // record commits here, taken from the restore whose store this adopts,
    // and cleared by a new store.
    [[nodiscard]] bool holdsUserWork() const { return userWork_; }

    [[nodiscard]] bool hasPending() const { return !pending_.empty(); }
    [[nodiscard]] const Pending *pending(const std::string &id) const;
    [[nodiscard]] bool hasFailures() const { return !failures_.empty(); }
    [[nodiscard]] const QString *failure(const std::string &id) const;
    void recordFailure(const std::string &id, const QString &error);
    // Replaces a file's error only while it is still exactly expected; an
    // empty replacement clears it. Returns whether it changed.
    bool settleFailure(const std::string &id, const QString &expected, const QString &replacement);
    // Whether loaded files may replace current ones: each current file is
    // still the untouched placeholder for the same ID and path, and only
    // their open captures wait for retry.
    [[nodiscard]] bool canCompleteLoad(const std::vector<FileSession> &current,
                                       const std::vector<FileSession> &loaded) const;

private:
    storage::RecoveryStore store_;
    std::shared_ptr<QLockFile> lease_;
    std::map<std::string, Pending> pending_;
    std::map<std::string, QString> failures_;
    bool userWork_ = false;
};
}
