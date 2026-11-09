// The session store owns each file's history and the recovery journal that makes its edits durable.
#pragma once

#include "core/EditingCommands.h"
#include "core/SessionState.h"
#include "recovery/RecoveryJournal.h"
#include <QObject>
#include <QString>
#include <cstddef>
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
// Views read the store and listen to its signals; the store knows nothing of
// presentation order or text. Used on one thread, the one it lives on.
class SessionStore : public QObject {
    Q_OBJECT
public:
    // The journal record each command writes, one name per recovery action.
    enum class Action {
        Open,
        ReplaceDraft,
        Undo,
        Redo,
        ManualSave,
        DiscardToCheckpoint,
        DiscardToStaged,
        MarkResolvedReview,
        WriteOutcome,
        StageOutcome,
        Support,
        LoadLarge
    };
    Q_ENUM(Action)
    // The action's name in the recovery journal.
    [[nodiscard]] static QString actionName(Action action);
    [[nodiscard]] static std::optional<Action> actionNamed(const QString &name);
    // Whether a file's presentation group (unresolved, resolved, outside the
    // app) follows this action. Edits keep a resolved row in place until an
    // explicit publication, discard or support change moves it.
    [[nodiscard]] static bool repositions(Action action);

    // recoveryDirectory is this session's recovery store.
    explicit SessionStore(QString recoveryDirectory, QObject *parent = nullptr);

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
    [[nodiscard]] const QString *recoveryFailure(const std::string &id) const { return journal_.failure(id); }
    // The command whose commit failed and is waiting for retry, or nothing.
    [[nodiscard]] std::optional<Action> pendingAction(const std::string &id) const;
    [[nodiscard]] QString pendingError(const std::string &id) const;
    // The draft of a file's pending input, which the editor shows until retry.
    [[nodiscard]] const Draft *pendingDraft(const std::string &id) const;
    // Records a recovery error that blocks the file until it is settled.
    void recordRecoveryFailure(const std::string &id, const QString &error);
    // Replaces a file's recovery error only while it is still exactly
    // expected; an empty replacement clears it. Returns whether it changed.
    bool settleRecoveryFailure(const std::string &id, const QString &expected, const QString &replacement = {});
    [[nodiscard]] const QString &recoveryDirectory() const { return journal_.directory(); }
    // Takes the store's lease on first use; empty while it holds, otherwise why the store is unavailable.
    QString ensureRecoveryLease() { return journal_.ensureLease(); }
    // Commits a transaction that is not a file command, such as a mutation
    // intent, under the store's lease. Nothing is retained on failure.
    [[nodiscard]] storage::RecoveryCommit commitRecord(const storage::RecoveryTransaction &transaction);
    // Whether the store holds a record other than a launch capture.
    [[nodiscard]] bool holdsUserWork() const { return journal_.holdsUserWork(); }
    // Moves recovery to a new store beside the current one and releases the
    // current store's lease, so a repeated load never records over captures
    // already in it. Refused while pending input waits for this store.
    bool startNewRecoveryStore() { return journal_.startNewStore(); }
    // Commits a file's pending input under its original transaction.
    Transition retryRecovery(const std::string &id);

    // Whether files may complete the current load: every file is still the
    // untouched placeholder for the same ID and path, and only its open
    // capture waits for recovery retry.
    [[nodiscard]] bool canCompleteLoad(const std::vector<FileSession> &files) const;
    [[nodiscard]] bool canRestoreLoad() const { return canCompleteLoad(files_); }
    // Resets every file and drops pending input and recovery errors. With a
    // restore, adopts its store and errors, and commits only its new files.
    // SessionCoordinator calls this after cancelling the old session's work.
    void replaceFiles(std::vector<FileSession> files, RecoveryRestore *restore = nullptr);
    bool appendFile(FileSession file);

    Transition replaceDraft(const std::string &id, RevisionId expected, Draft draft);
    Transition editResult(const std::string &id, RevisionId expected, ResultEdit edit);
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
    Transition setSupport(const std::string &id, Support support, std::string detail = {});
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
    template <typename Command> Transition update(const std::string &id, Action action, Command command);
    bool commitCandidate(std::size_t row, FileSession candidate, Action action);
    void notify(std::size_t row, std::optional<Action> action);

    std::vector<FileSession> files_;
    // facts_[row] is files_[row].facts(), recomputed whenever a row is stored.
    std::vector<FileFacts> facts_;
    std::unordered_map<std::string, std::size_t> rows_;
    RecoveryJournal journal_;
    std::set<std::string> restored_;
};
}
