// Publication state records Save, Mark resolved, write and stage outcomes for a file's revisions.
#pragma once

#include "RevisionHistory.h"
#include "Transition.h"
#include <optional>
#include <string>
#include <vector>

namespace branch_truce {

enum class Outcome { Succeeded, Failed, Uncertain };
struct OperationOutcome {
    RevisionId revision;
    Outcome outcome;
    std::string detail;
};

struct Review {
    RevisionId revision;
    bool confirmedUndecided;
};

struct StagedResult {
    RevisionId revision;
    std::string objectId;
    std::string mode;
    bool confirmedUndecided;
};

enum class Blocker {
    OutsideApp,
    Unreadable,
    LargeFile,
    UnresolvedMarkers,
    Undecided,
    UnsavedDraft,
    ReviewNeeded,
    WriteRequired,
    StageRequired,
    EarlierStagedResult,
    CheckpointFailed,
    WriteFailed,
    StageFailed,
    UncertainOutcome
};
enum class FileStatus {
    OutsideApp,
    Unreadable,
    LargeFile,
    OutcomeUncertain,
    CheckpointFailed,
    WriteFailed,
    StageFailed,
    EarlierStaged,
    Resolved,
    WrittenUnresolved,
    SavedDraft,
    Undecided,
    ReviewNeeded
};

struct FileFacts {
    std::size_t undecided;
    bool unsaved;
    bool needsReview;
    bool earlierStaged;
    bool resolved;
    FileStatus status;
    std::vector<Blocker> blockers;
    // Every contribution is decided and no conflict marker remains, while the
    // current result still waits for Mark resolved. Derived from the current
    // revision, so an undo that reopens a choice clears it. It is not review
    // and changes nothing in the repository or Git.
    bool settled = false;
};

// The outcomes adapters report for one file's revisions, and what they mean
// for the current revision. Revisions below the outcome floor were discarded,
// so outcomes for them are stale unless the revision is current again.
//
// It names revisions by id. FileSession keeps every revision named here in
// its history, where they count toward kMaxHistoryBytes, and resolves the
// ids with FileSession::revision().
class PublicationState {
public:
    PublicationState() = default;
    explicit PublicationState(RevisionId floor) noexcept : outcomeFloor_(floor) {}

    // The revision the last successful Save stored.
    [[nodiscard]] const std::optional<RevisionId> &checkpoint() const noexcept { return checkpoint_; }
    [[nodiscard]] const std::optional<Review> &reviewed() const noexcept { return reviewed_; }
    // The reviewed revision the last successful write put in the worktree.
    [[nodiscard]] const std::optional<RevisionId> &written() const noexcept { return written_; }
    [[nodiscard]] const std::optional<StagedResult> &staged() const noexcept { return staged_; }
    [[nodiscard]] const std::optional<OperationOutcome> &checkpointOutcome() const noexcept
    {
        return checkpointOutcome_;
    }
    [[nodiscard]] const std::optional<OperationOutcome> &writeOutcome() const noexcept { return writeOutcome_; }
    [[nodiscard]] const std::optional<OperationOutcome> &stageOutcome() const noexcept { return stageOutcome_; }
    // Nothing was saved, reviewed, written or staged yet.
    [[nodiscard]] bool untouched() const noexcept;
    // `revision` is the checkpoint and its Save is the latest one and succeeded.
    [[nodiscard]] bool checkpointed(RevisionId revision) const noexcept;
    // `revision` is the written revision and its write succeeded.
    [[nodiscard]] bool writtenAt(RevisionId revision) const noexcept;
    // The current revision is the checkpoint's own successfully saved one.
    [[nodiscard]] bool savedAt(RevisionId current) const noexcept;
    // The staged revision is known written and staged, so discarding to it
    // needs no new review.
    [[nodiscard]] bool stagedVerified() const noexcept;
    // Every revision named above, which the history must keep.
    [[nodiscard]] std::vector<RevisionId> held() const;

    void markResolved(RevisionId current, bool confirmedUndecided);
    // `stored` says whether the revision with id `revision` is still stored.
    Transition recordCheckpoint(bool stored, RevisionId revision, RevisionId current, Outcome outcome,
                                std::string detail);
    Transition recordWrite(RevisionId revision, RevisionId current, Outcome outcome, std::string detail);
    Transition recordStage(RevisionId revision, RevisionId current, Outcome outcome, std::string objectId,
                           std::string mode, std::string detail);
    // Discarding to the checkpoint made `current`, so outcomes of the
    // revisions before it are stale.
    void discardedToCheckpoint(RevisionId current);
    // Discarding to the staged revision fences every revision before `floor`.
    void discardedToStaged(RevisionId floor);

    // `checkpoint` is the revision checkpoint() names, if any.
    [[nodiscard]] FileFacts facts(const Revision &current, const Revision *checkpoint, Support support,
                                  std::size_t undecided) const;

private:
    [[nodiscard]] bool fenced(RevisionId revision, RevisionId current) const noexcept
    {
        return revision < outcomeFloor_ && revision != current;
    }
    [[nodiscard]] std::vector<Blocker> blockers(const Revision &current, Support support, const FileFacts &facts,
                                                bool written, bool staged) const;
    [[nodiscard]] FileStatus status(const FileFacts &facts, Support support, bool checkpoint) const;

    RevisionId outcomeFloor_ = 1;
    std::optional<RevisionId> checkpoint_;
    std::optional<Review> reviewed_;
    // The review's checkpoint outcome survives ordinary Saves of newer drafts.
    bool reviewCheckpointSucceeded_ = false;
    std::optional<RevisionId> written_;
    std::optional<StagedResult> staged_;
    std::optional<OperationOutcome> checkpointOutcome_;
    std::optional<OperationOutcome> writeOutcome_;
    std::optional<OperationOutcome> stageOutcome_;
};

}
