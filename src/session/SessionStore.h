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
struct MutationFact;
class BackgroundAlignment;

// Owns file histories and their serial recovery journal on the event-loop thread.
// Command futures finish with Applied only after their exact transaction commits.
// Failure retains candidates and transaction IDs for retry. Typing alone applies
// immediately, in undo bursts, and remains recording until its records commit.
// Requests enter one journal FIFO before any engine work. A same-file request
// computes from its predecessor's candidate; other files keep their own base.
// Typing before commands is queued first; typing during a command is refused.
// One unqueued burst per already-edited file survives capacity refusal and blocks
// further editing until capacity returns. Retry commits the original failed FIFO
// before admitting those bursts. Session replacement refuses active records.
// Closing drains admitted records on the owned worker without a deadline.
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
    [[nodiscard]] bool hasPendingRecovery() const;
    void setExecutionLimits(RecoveryJournal::Limits limits) { journal_.setLimits(limits); }
    [[nodiscard]] bool hasRecoveryFailures() const { return journal_.hasFailures(); }
    [[nodiscard]] bool hasRecoveryFailure(const std::string &id) const { return journal_.failure(id) != nullptr; }
    [[nodiscard]] const Problem *recoveryFailure(const std::string &id) const { return journal_.failure(id); }
    // The oldest failed command of a file, if any.
    [[nodiscard]] std::optional<Action> pendingAction(const std::string &id) const;
    [[nodiscard]] Problem pendingError(const std::string &id) const;
    // The draft of a file's newest pending input, which the editor shows until retry.
    [[nodiscard]] const Draft *pendingDraft(const std::string &id) const;
    // Unacknowledged typing or commands, per file or for the whole store.
    [[nodiscard]] bool recording(const std::string &id) const
    {
        return typing_.contains(id) || commands_.contains(id) || journal_.recording(id);
    }
    [[nodiscard]] bool commandPending(const std::string &id) const { return commands_.contains(id); }
    [[nodiscard]] bool recording() const
    {
        return !typing_.empty() || !commands_.empty() || nextOpen_ < files_.size() || journal_.busy();
    }
    // Queues every open typing burst. True only when nothing remains unfinished.
    bool recordTyping();
    // Records a recovery error that blocks the file until it is settled.
    void recordRecoveryFailure(const std::string &id, const Problem &problem);
    // Replaces a file's recovery error only while it is still exactly
    // expected; an empty replacement clears it. Returns whether it changed.
    bool settleRecoveryFailure(const std::string &id, const Problem &expected, const Problem &replacement = {});
    [[nodiscard]] const QString &recoveryDirectory() const { return journal_.directory(); }
    // The held lease, or null. A mutation keeps it to record its observation
    // in this store even after the session moves to another one.
    [[nodiscard]] std::shared_ptr<storage::RecoveryLease> recoveryLease() const { return journal_.lease(); }
    // Commits a transaction that is not a file command, such as a mutation
    // intent, under the store's lease. Nothing is retained on failure.
    [[nodiscard]] QFuture<RecoveryJournal::Receipt> commitRecord(const storage::RecoveryTransaction &transaction);
    [[nodiscard]] QFuture<RecoveryJournal::Receipt> commitMutation(const FileSession &file, MutationFact fact);
    // Deletes the recovery store once nothing in it is needed: no input is
    // pending and no file has a recovery error. A finished session calls it.
    QFuture<bool> discardRecoveryStore();
    // Whether the store holds, or is recording, work other than a launch capture.
    [[nodiscard]] bool holdsUserWork() const { return journal_.holdsUserWork() || recording(); }
    // Moves recovery to a new store beside the current one and releases the
    // current store's lease, so a repeated load never records over captures
    // already in it. Refused while pending input waits for this store.
    bool startNewRecoveryStore();
    // Retries the store's dependent FIFO under its original transactions.
    // Applied once none is left; Blocked when a commit failed again.
    QFuture<Transition> retryRecovery(const std::string &id);

    // Whether a load may still complete: every file is an untouched
    // placeholder, and only its open capture waits for recovery retry.
    [[nodiscard]] bool canRestoreLoad() const;
    // Resets every file and drops pending input and recovery errors. With a
    // restore that adopted a store, moves to that store, takes its errors and
    // commits only its new files.
    // SessionCoordinator calls this after cancelling the old session's work.
    void replaceFiles(std::vector<FileSession> files, RecoveryRestore *restore = nullptr);
    bool appendFile(FileSession file);

    QFuture<Transition> replaceDraft(const std::string &id, RevisionId expected, Draft draft);
    // One edit, such as a paste or a drop, as its own command: acknowledged
    // once durable, with its passages aligned behind it as for typing.
    QFuture<Transition> editResult(const std::string &id, RevisionId expected, ResultEdit edit);
    // A typed edit: applied at once, joined to the file's burst and recorded
    // behind (see the class comment). Its passages stay provisional until
    // installAlignment().
    Transition typeResult(const std::string &id, RevisionId expected, ResultEdit edit);
    // Installs a map that CorrespondenceMap::realigned() built from the file's
    // pending alignment for `revision`, if that revision is still current.
    // Recovery records hold nothing it changes, so it writes none.
    Transition installAlignment(const std::string &id, RevisionId revision,
                                std::shared_ptr<const CorrespondenceMap> aligned);
    QFuture<Transition> awaitAlignment(const std::string &id, RevisionId revision);
    QFuture<Transition> applyChoice(const std::string &id, const ChoicePreview &preview,
                                    bool confirmReplacement = false);
    QFuture<Transition> applyNonConflicting(const std::string &id, const NonConflictingPreview &preview,
                                            bool confirm = false);
    QFuture<Transition> applyWholeFile(const std::string &id, const WholeFilePreview &preview, bool confirm = false);
    QFuture<Transition> undo(const std::string &id, RevisionId expected);
    QFuture<Transition> redo(const std::string &id, RevisionId expected);
    QFuture<Transition> discardToCheckpoint(const std::string &id, RevisionId expected);
    QFuture<Transition> discardToStaged(const std::string &id, RevisionId expected);
    QFuture<Transition> beginMarkResolved(const std::string &id, RevisionId expected, bool confirmUndecided = false);
    QFuture<Transition> recordCheckpoint(const std::string &id, RevisionId revision, Outcome outcome,
                                         std::string detail = {});
    QFuture<Transition> recordWrite(const std::string &id, RevisionId revision, Outcome outcome,
                                    std::string detail = {});
    QFuture<Transition> recordStage(const std::string &id, RevisionId revision, Outcome outcome,
                                    std::string objectId = {}, std::string mode = {}, std::string detail = {});
    QFuture<Transition> loadLargeFile(const std::string &id);

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
        // One latest unqueued burst, retained on refusal; further typing blocks.
        std::optional<FileSession> refused = {};
    };
    template <typename Command> QFuture<Transition> update(const std::string &id, Action action, Command command);
    void commitOpen(std::size_t row);
    void notify(std::size_t row, std::optional<Action> action);
    // Ends the file's burst and queues its record behind the editor.
    void endBurst(const std::string &id);
    void endBursts();
    // The outcome of a typed record that leaves the file as `candidate`.
    void typedRecordWritten(const std::string &id, const FileSession &candidate, Transition written);

    std::vector<FileSession> files_;
    // facts_[row] is files_[row].facts(), recomputed whenever a row is stored.
    std::vector<FileFacts> facts_;
    std::unordered_map<std::string, std::size_t> rows_;
    RecoveryJournal journal_;
    std::size_t nextOpen_ = 0;
    void admitOpenCaptures();
    std::set<std::string> restored_;
    std::map<std::string, Typing> typing_;
    std::map<std::string, RecoveryJournal::Record> commands_;
    quint64 generation_ = 0;
    bool retrying_ = false;
    // End every burst after a pause in typing, and at the latest kTypingBurst after one began.
    QTimer pause_;
    QTimer limit_;
    std::unique_ptr<BackgroundAlignment> alignment_;
};
}
