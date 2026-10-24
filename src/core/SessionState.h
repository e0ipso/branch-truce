#pragma once

#include "Correspondence.h"
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace branch_truce {

enum class DecisionValue { Undecided, Accepted, Ignored };
struct Decision {
    DecisionValue value = DecisionValue::Undecided;
    // Explicit combination order, supplied by the editing command.
    std::optional<std::uint64_t> order;
    bool operator==(const Decision &) const = default;
};

struct Draft {
    std::string text;
    std::map<std::string, Decision> decisions;
    std::size_t unresolvedMarkers = 0;
    bool operator==(const Draft &) const = default;
};

struct Revision {
    RevisionId id;
    RevisionId result;
    RevisionId decisions;
    Draft draft;
    std::shared_ptr<const CorrespondenceMap> correspondence = {};
};

// One atomic history entry. Future composition and bulk planners can submit
// mapped edits and explicit ordered decisions through the same boundary.
struct EditingCommand {
    RevisionId expected = 0;
    std::vector<ResultAnchor> targets;
    std::vector<ResultEdit> edits;
    std::map<std::string, Decision> decisions;
    bool operator==(const EditingCommand &) const = default;
};
struct ChoiceTarget;

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

enum class Support { Supported, OutsideApp, Unreadable };
enum class Transition {
    Applied, NoChange, StaleRevision, ConfirmationRequired, Blocked, InvalidOutcome,
    InvalidAnchor, AmbiguousTarget
};
enum class Blocker {
    OutsideApp, Unreadable, UnresolvedMarkers, Undecided, UnsavedDraft,
    ReviewNeeded, WriteRequired, StageRequired, EarlierStagedResult,
    CheckpointFailed, WriteFailed, StageFailed, UncertainOutcome
};
enum class FileStatus {
    OutsideApp, Unreadable, OutcomeUncertain, CheckpointFailed, WriteFailed,
    StageFailed, EarlierStaged, Resolved, WrittenUnresolved, SavedDraft,
    Undecided, ReviewNeeded
};

struct FileFacts {
    std::size_t undecided;
    bool unsaved;
    bool needsReview;
    bool earlierStaged;
    bool resolved;
    FileStatus status;
    std::vector<Blocker> blockers;
};

// Value-only session bookkeeping. This class performs no I/O. Success callbacks
// must come from adapters that have persisted/verified the exact revision.
class FileSession {
public:
    FileSession(std::string id, std::string path, Draft initial,
                Support support = Support::Supported, std::string supportDetail = {},
                std::optional<FileSnapshots> snapshots = std::nullopt,
                bool initializeChoices = false);

    [[nodiscard]] const std::string &id() const noexcept { return id_; }
    [[nodiscard]] const std::string &path() const noexcept { return path_; }
    [[nodiscard]] Support support() const noexcept { return support_; }
    [[nodiscard]] const std::string &supportDetail() const noexcept { return supportDetail_; }
    // Shared immutable capture, retained independently of every result revision.
    // Null for sessions without an admitted source/result capture.
    [[nodiscard]] const FileSnapshots *snapshots() const noexcept { return snapshots_.get(); }
    [[nodiscard]] const Revision &current() const { return revisions_.at(current_); }
    [[nodiscard]] const CorrespondenceMap *correspondence() const noexcept {
        return current().correspondence.get();
    }
    [[nodiscard]] const std::vector<ChoiceTarget> &choices() const noexcept;
    [[nodiscard]] const std::optional<Revision> &checkpoint() const noexcept { return checkpoint_; }
    [[nodiscard]] const std::optional<Review> &reviewed() const noexcept { return reviewed_; }
    [[nodiscard]] const std::optional<Revision> &written() const noexcept { return written_; }
    [[nodiscard]] const std::optional<StagedResult> &staged() const noexcept { return staged_; }
    [[nodiscard]] const std::optional<OperationOutcome> &checkpointOutcome() const noexcept { return checkpointOutcome_; }
    [[nodiscard]] const std::optional<OperationOutcome> &writeOutcome() const noexcept { return writeOutcome_; }
    [[nodiscard]] const std::optional<OperationOutcome> &stageOutcome() const noexcept { return stageOutcome_; }
    [[nodiscard]] bool canUndo() const noexcept { return cursor_ > 0; }
    [[nodiscard]] bool canRedo() const noexcept { return cursor_ + 1 < history_.size(); }
    [[nodiscard]] FileFacts facts() const;

    // Each editing transaction carries text and decisions together. Stale
    // editor commands cannot overwrite a later revision.
    Transition replaceDraft(RevisionId expected, Draft draft);
    // Binding records a stable source contribution without changing decisions.
    // Inspect the returned current anchor's state before offering a choice.
    Transition bindSourceAnchor(RevisionId expected, SourceAnchor source);
    Transition editResult(RevisionId expected, ResultEdit edit);
    Transition applyCommand(const EditingCommand &command, bool confirmReplacement = false);
    // A preview uses this exact anchor and revision. Passing confirmation after
    // either changes cannot authorize a different replacement.
    Transition replaceMapped(RevisionId expected, const ResultAnchor &anchor,
                             std::string replacement, std::optional<Decision> decision = {},
                             bool confirmManual = false);
    Transition undo(RevisionId expected);
    Transition redo(RevisionId expected);
    Transition discardToCheckpoint(RevisionId expected);
    // A deliberate discard may reuse verified identity only while both the
    // worktree and index are still known to contain that staged revision.
    Transition discardToStaged(RevisionId expected);

    // Captures the review declaration of Mark resolved, not a review-only UI
    // action. Cancelling confirmation calls nothing and changes nothing.
    Transition beginMarkResolved(RevisionId expected, bool confirmUndecided = false);
    Transition recordCheckpoint(RevisionId revision, Outcome outcome, std::string detail = {});
    Transition recordWrite(RevisionId revision, Outcome outcome, std::string detail = {});
    Transition recordStage(RevisionId revision, Outcome outcome,
                           std::string objectId = {}, std::string mode = {},
                           std::string detail = {});
    Transition setSupport(Support support, std::string detail = {});

private:
    RevisionId advance(const Draft &draft,
                       std::shared_ptr<const CorrespondenceMap> correspondence = {});
    RevisionId restore(const Revision &revision);
    void appendHistory(RevisionId revision);
    std::string id_;
    std::string path_;
    Support support_;
    std::string supportDetail_;
    std::shared_ptr<const FileSnapshots> snapshots_;
    std::shared_ptr<const std::vector<ChoiceTarget>> choices_;
    std::map<RevisionId, Revision> revisions_;
    RevisionId current_ = 1;
    RevisionId next_ = 2;
    RevisionId outcomeFloor_ = 1;
    std::vector<RevisionId> history_{1};
    std::size_t cursor_ = 0;
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

[[nodiscard]] const char *statusText(FileStatus status) noexcept;
[[nodiscard]] const char *blockerText(Blocker blocker) noexcept;

}
