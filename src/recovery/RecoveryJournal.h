// The recovery journal keeps acknowledged edits durable and retains failed input for retry.
#pragma once

#include "core/SessionState.h"
#include "recovery/RecoveryNames.h"
#include "storage/RecoveryStore.h"
#include <QCoreApplication>
#include <QString>
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
    std::map<std::string, QString> failures;
};

// One session's recovery store and what waits on it: the store's lease,
// input whose commit failed and per-file recovery errors. Callers acknowledge
// input only after its transaction commits. A failed commit keeps the encoded
// transaction, so a retry appends it at most once.
class RecoveryJournal {
    Q_DECLARE_TR_FUNCTIONS(RecoveryJournal)
public:
    struct Pending {
        FileSession candidate;
        RecoveryAction action;
        storage::RecoveryTransaction transaction;
        QString error;
    };

    RecoveryJournal(QString directory, std::shared_ptr<const storage::FileSystem> files);
    [[nodiscard]] const QString &directory() const { return directory_; }
    // Takes the store's session lease on first use. Empty while the lease is
    // held, otherwise the reason the store is unavailable.
    QString ensureLease();
    // The held lease, or null. A writer that must finish in this store after
    // the journal moves on, such as a running mutation, keeps it.
    [[nodiscard]] std::shared_ptr<storage::RecoveryLease> lease() const { return lease_; }
    // Commits a record that is not a file command, such as a mutation fact,
    // under the lease, without retaining anything on failure.
    [[nodiscard]] storage::RecoveryCommit commit(const storage::RecoveryTransaction &transaction);
    // Commits the candidate as a new transaction. previousText is the file's
    // current text before the action. On failure the candidate becomes the
    // file's pending input, replacing any earlier one, and this returns false.
    [[nodiscard]] bool commitCandidate(const FileSession &candidate, RecoveryAction action,
                                       std::string_view previousText);
    // Commits a file's pending input under its original transaction and
    // returns it. Empty when nothing is pending or the commit failed again;
    // a failure keeps the input with the new error.
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
    [[nodiscard]] const Pending *pending(const std::string &id) const;
    // Whether every pending input is a launch capture.
    [[nodiscard]] bool onlyOpenCapturesPending() const;
    [[nodiscard]] bool hasFailures() const { return !failures_.empty(); }
    [[nodiscard]] const QString *failure(const std::string &id) const;
    void recordFailure(const std::string &id, const QString &error);
    // Replaces a file's error only while it is still exactly expected; an
    // empty replacement clears it. Returns whether it changed.
    bool settleFailure(const std::string &id, const QString &expected, const QString &replacement);

private:
    [[nodiscard]] storage::RecoveryCommit commitUnderLease(const storage::RecoveryTransaction &transaction);

    QString directory_;
    std::shared_ptr<const storage::FileSystem> files_;
    std::shared_ptr<storage::RecoveryLease> lease_;
    std::map<std::string, Pending> pending_;
    std::map<std::string, QString> failures_;
    bool userWork_ = false;
};
}
