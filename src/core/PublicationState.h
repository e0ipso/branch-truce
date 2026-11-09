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
    Revision revision;
    bool confirmedUndecided;
};

struct StagedResult {
    Revision revision;
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
class PublicationState {
public:
    PublicationState() = default;
    explicit PublicationState(RevisionId floor) noexcept : outcomeFloor_(floor) {}

    [[nodiscard]] const std::optional<Revision> &checkpoint() const noexcept { return checkpoint_; }
    [[nodiscard]] const std::optional<Review> &reviewed() const noexcept { return reviewed_; }
    [[nodiscard]] const std::optional<Revision> &written() const noexcept { return written_; }
    [[nodiscard]] const std::optional<StagedResult> &staged() const noexcept { return staged_; }
    [[nodiscard]] const std::optional<OperationOutcome> &checkpointOutcome() const noexcept
    {
        return checkpointOutcome_;
    }
    [[nodiscard]] const std::optional<OperationOutcome> &writeOutcome() const noexcept { return writeOutcome_; }
    [[nodiscard]] const std::optional<OperationOutcome> &stageOutcome() const noexcept { return stageOutcome_; }
    // Nothing was saved, reviewed, written or staged yet.
    [[nodiscard]] bool untouched() const noexcept;
    // The current revision is the checkpoint's own successfully saved one.
    [[nodiscard]] bool savedAt(RevisionId current) const noexcept;
    // The staged revision is known written and staged, so discarding to it
    // needs no new review.
    [[nodiscard]] bool stagedVerified() const noexcept;

    void markResolved(const Revision &current, bool confirmedUndecided);
    // `stored` is the revision with id `revision`, or nullptr when it is gone.
    Transition recordCheckpoint(const Revision *stored, RevisionId revision, RevisionId current, Outcome outcome,
                                std::string detail);
    Transition recordWrite(RevisionId revision, RevisionId current, Outcome outcome, std::string detail);
    Transition recordStage(RevisionId revision, RevisionId current, Outcome outcome, std::string objectId,
                           std::string mode, std::string detail);
    // Discarding to the checkpoint made `current`, so outcomes of the
    // revisions before it are stale.
    void discardedToCheckpoint(RevisionId current);
    // Discarding to the staged revision fences every revision before `floor`.
    void discardedToStaged(RevisionId floor);

    [[nodiscard]] FileFacts facts(const Revision &current, Support support, std::size_t undecided) const;

private:
    [[nodiscard]] bool fenced(RevisionId revision, RevisionId current) const noexcept
    {
        return revision < outcomeFloor_ && revision != current;
    }
    [[nodiscard]] std::vector<Blocker> blockers(const Revision &current, Support support, const FileFacts &facts,
                                                bool written, bool staged) const;
    [[nodiscard]] FileStatus status(const FileFacts &facts, Support support, bool checkpoint) const;

    RevisionId outcomeFloor_ = 1;
    std::optional<Revision> checkpoint_;
    std::optional<Review> reviewed_;
    // The review's checkpoint outcome survives ordinary Saves of newer drafts.
    bool reviewCheckpointSucceeded_ = false;
    std::optional<Revision> written_;
    std::optional<StagedResult> staged_;
    std::optional<OperationOutcome> checkpointOutcome_;
    std::optional<OperationOutcome> writeOutcome_;
    std::optional<OperationOutcome> stageOutcome_;
};

}
