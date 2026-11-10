// The session store owns each file's history and the recovery journal that makes its edits durable.
#pragma once

#include "core/EditingCommands.h"
#include "core/SessionState.h"
#include "recovery/RecoveryJournal.h"
#include <QObject>
#include <QString>
#include <QTimer>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace branch_truce {
struct RecoveryRestore;

// The files of one session in discovery order, their facts, their recovery
// journal and every command that changes them. A command runs on a copy of the
// file and is acknowledged only after its journal transaction commits; a
// failed commit keeps the candidate as the file's pending input until retry.
// Acknowledged always means durable.
//
// Typing is the exception to commit-before-apply: typeResult() applies a typed
// edit at once and records it behind the editor. Typed edits of one file form
// a burst that one undo takes back and one journal record holds; a burst ends
// after kTypingPause without typing, kTypingBurst after it began, at a typed
// line break, and before any other command. Its record is written on a worker
// and the burst is acknowledged once that record is durable; until then
// recording() is true. A crash can lose only typed edits that are not yet
// acknowledged: the current burst and records still being written. A failed
// record rolls the file back to its acknowledged state and keeps every typed
// edit after it as pending input, in order, until retry. Every other command
// first writes the typed records, so the journal keeps the order of the edits.
//
// Views read the store and listen to its signals; the store knows nothing of
// presentation order or text. Used on one thread, the one it lives on.
class SessionStore : public QObject {
    Q_OBJECT
public:
    static constexpr std::chrono::milliseconds kTypingPause{500};
    static constexpr std::chrono::milliseconds kTypingBurst{2000};

    // The journal record each command writes. Replay dispatches on the same
    // value, so a new command is one enum value and one replay case.
    using Action = RecoveryAction;
    // Whether a file's presentation group (unresolved, resolved, outside the
    // app) follows this action. Edits keep a resolved row in place until an
    // explicit publication, discard or support change moves it.
    [[nodiscard]] static bool repositions(Action action);

    // recoveryDirectory is this session's recovery store, written through files.
    explicit SessionStore(QString recoveryDirectory,
                          std::shared_ptr<const storage::FileSystem> files = storage::systemFileSystem(),
                          QObject *parent = nullptr);
    // Records typed edits first, so closing a window keeps them.
    ~SessionStore() override;

    [[nodiscard]] std::size_t size() const { return files_.size(); }
    // Files in discovery order; a storage row indexes this and facts().
    [[nodiscard]] const std::vector<FileSession> &files() const { return files_; }
    [[nodiscard]] const FileFacts &facts(std::size_t row) const { return facts_[row]; }
    [[nodiscard]] const FileSession *file(const std::string &id) const;
    // The storage row of a file ID, or -1.
    [[nodiscard]] int rowOf(const std::string &id) const;
    // Whether the file came from an adopted recovery store rather than this load.
    [[nodiscard]] bool restored(const std::string &id) const { return restored_.contains(id); }
    [[nodiscard]] int supportCount(Support support) const;

    // Recovery state of the session and of one file.
    [[nodiscard]] bool hasPendingRecovery() const { return journal_.hasPending(); }
    [[nodiscard]] bool hasRecoveryFailures() const { return journal_.hasFailures(); }
    [[nodiscard]] bool hasRecoveryFailure(const std::string &id) const { return journal_.failure(id) != nullptr; }
    [[nodiscard]] const Problem *recoveryFailure(const std::string &id) const { return journal_.failure(id); }
    // The command whose commit failed and is waiting for retry, or nothing.
    [[nodiscard]] std::optional<Action> pendingAction(const std::string &id) const;
    [[nodiscard]] Problem pendingError(const std::string &id) const;
    // The draft of a file's newest pending input, which the editor shows until retry.
    [[nodiscard]] const Draft *pendingDraft(const std::string &id) const;
    // Whether typed edits of the file, or of any file, are applied but not yet acknowledged.
    [[nodiscard]] bool recording(const std::string &id) const { return typing_.contains(id); }
    [[nodiscard]] bool recording() const { return !typing_.empty(); }
    // Ends every burst and writes the typed edits' records now, on this
    // thread. Returns whether nothing waits for recovery afterwards.
    bool recordTyping();
    // Records a recovery error that blocks the file until it is settled.
    void recordRecoveryFailure(const std::string &id, const Problem &problem);
    // Replaces a file's recovery error only while it is still exactly
    // expected; an empty replacement clears it. Returns whether it changed.
    bool settleRecoveryFailure(const std::string &id, const Problem &expected, const Problem &replacement = {});
    [[nodiscard]] const QString &recoveryDirectory() const { return journal_.directory(); }
    // Takes the store's lease on first use; empty while it holds, otherwise why the store is unavailable.
    Problem ensureRecoveryLease() { return journal_.ensureLease(); }
    // The held lease, or null. A mutation keeps it to record its observation
    // in this store even after the session moves to another one.
    [[nodiscard]] std::shared_ptr<storage::RecoveryLease> recoveryLease() const { return journal_.lease(); }
    // Commits a transaction that is not a file command, such as a mutation
    // intent, under the store's lease. Nothing is retained on failure.
    [[nodiscard]] storage::RecoveryCommit commitRecord(const storage::RecoveryTransaction &transaction);
    // Deletes the recovery store once nothing in it is needed: no input is
    // pending and no file has a recovery error. A finished session calls it.
    bool discardRecoveryStore();
    // Whether the store holds, or is recording, work other than a launch capture.
    [[nodiscard]] bool holdsUserWork() const { return journal_.holdsUserWork() || recording(); }
    // Moves recovery to a new store beside the current one and releases the
    // current store's lease, so a repeated load never records over captures
    // already in it. Refused while pending input waits for this store.
    bool startNewRecoveryStore();
    // Commits a file's pending input in order under its original transactions.
    // Applied once none is left; Blocked when a commit failed again.
    Transition retryRecovery(const std::string &id);

    // Whether a load may still complete: every file is an untouched
    // placeholder, and only its open capture waits for recovery retry.
    [[nodiscard]] bool canRestoreLoad() const;
    // Resets every file and drops pending input and recovery errors. With a
    // restore that adopted a store, moves to that store, takes its errors and
    // commits only its new files.
    // SessionCoordinator calls this after cancelling the old session's work.
    void replaceFiles(std::vector<FileSession> files, RecoveryRestore *restore = nullptr);
    bool appendFile(FileSession file);

    Transition replaceDraft(const std::string &id, RevisionId expected, Draft draft);
    // One edit, such as a paste or a drop, as its own command: aligned at once
    // and acknowledged once durable.
    Transition editResult(const std::string &id, RevisionId expected, ResultEdit edit);
    // A typed edit: applied at once, joined to the file's burst and recorded
    // behind (see the class comment). Its passages stay provisional until
    // installAlignment() or settleAlignment().
    Transition typeResult(const std::string &id, RevisionId expected, ResultEdit edit);
    // Installs a map that CorrespondenceMap::realigned() built from the file's
    // pending alignment for `revision`, if that revision is still current.
    // Recovery records hold nothing it changes, so it writes none.
    Transition installAlignment(const std::string &id, RevisionId revision,
                                std::shared_ptr<const CorrespondenceMap> aligned);
    // Aligns the file's current revision on this thread if its passages are
    // still provisional, for work that needs exact passages, such as a choice
    // preview. NoChange when they are exact already.
    Transition settleAlignment(const std::string &id);
    Transition applyChoice(const std::string &id, const ChoicePreview &preview, bool confirmReplacement = false);
    Transition applyNonConflicting(const std::string &id, const NonConflictingPreview &preview, bool confirm = false);
    Transition applyWholeFile(const std::string &id, const WholeFilePreview &preview, bool confirm = false);
    Transition undo(const std::string &id, RevisionId expected);
    Transition redo(const std::string &id, RevisionId expected);
    Transition discardToCheckpoint(const std::string &id, RevisionId expected);
    Transition discardToStaged(const std::string &id, RevisionId expected);
    Transition beginMarkResolved(const std::string &id, RevisionId expected, bool confirmUndecided = false);
    Transition recordCheckpoint(const std::string &id, RevisionId revision, Outcome outcome, std::string detail = {});
    Transition recordWrite(const std::string &id, RevisionId revision, Outcome outcome, std::string detail = {});
    Transition recordStage(const std::string &id, RevisionId revision, Outcome outcome, std::string objectId = {},
                           std::string mode = {}, std::string detail = {});
    Transition loadLargeFile(const std::string &id);

Q_SIGNALS:
    // replaceFiles brackets its reset with these two.
    void aboutToReplace();
    void replaced();
    // appendFile brackets the new storage row with these two.
    void aboutToAppend(int row);
    void appended(int row);
    // A command stored a new state of the file in row; fileChanged follows.
    void fileStored(int row, branch_truce::SessionStore::Action action);
    // A file's history, facts or recovery state changed.
    void fileChanged(int row);

private:
    // Typed edits of one file that are not yet acknowledged.
    struct Typing {
        // The file as its last durable record left it; a failed record rolls back to it.
        FileSession acknowledged;
        // The file as its last queued record leaves it; the next record is a change from its text.
        FileSession recorded;
        // Records queued behind and not yet settled.
        int queued = 0;
        // Whether the file holds typed edits no record covers yet. They form
        // the current burst, and the next typed edit joins its undo entry.
        bool open = false;
    };
    template <typename Command> Transition update(const std::string &id, Action action, Command command);
    bool commitCandidate(std::size_t row, FileSession candidate, Action action);
    void commitOpen(std::size_t row);
    void notify(std::size_t row, std::optional<Action> action);
    // Ends the file's burst and queues its record behind the editor.
    void endBurst(const std::string &id);
    void endBursts();
    // The outcome of a typed record that leaves the file as `candidate`.
    void typedRecordWritten(const std::string &id, const FileSession &candidate, RecoveryJournal::Written written);

    std::vector<FileSession> files_;
    // facts_[row] is files_[row].facts(), recomputed whenever a row is stored.
    std::vector<FileFacts> facts_;
    std::unordered_map<std::string, std::size_t> rows_;
    RecoveryJournal journal_;
    std::set<std::string> restored_;
    std::map<std::string, Typing> typing_;
    // End every burst after a pause in typing, and at the latest kTypingBurst after one began.
    QTimer pause_;
    QTimer limit_;
};
}
