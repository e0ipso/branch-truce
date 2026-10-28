#pragma once

#include "ConflictMarkers.h"
#include "Correspondence.h"
#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
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
    // FileSession::markers().count() for this text. FileSession recomputes it
    // for every revision it stores and ignores a supplied value.
    std::size_t unresolvedMarkers = 0;
    bool operator==(const Draft &) const = default;
};

// A complete conflict-marker block and the choice sections it encloses, in
// text order. Either the two source scopes of one section fill its side
// regions, or each side region holds one scope of every section, in the same
// order, around lines both sides share.
struct MarkerSection {
    MarkerRange block;
    std::vector<std::string> sections;
    [[nodiscard]] bool encloses(const std::string &section) const
    {
        return std::find(sections.begin(), sections.end(), section) != sections.end();
    }
    bool operator==(const MarkerSection &) const = default;
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
enum class EditingScope { Mapped, WholeFile };
struct EditingCommand {
    RevisionId expected = 0;
    std::vector<ResultAnchor> targets;
    std::vector<ResultEdit> edits;
    std::map<std::string, Decision> decisions;
    EditingScope scope = EditingScope::Mapped;
    bool operator==(const EditingCommand &) const = default;
};
struct ChoiceTarget;
struct NonConflictingTarget;

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
    // Conflict-marker blocks and orphan marker lines in the current result.
    // A marker-like line the correspondence maps to identical text in a
    // captured side, or in the earlier version inside a diff3 base region, is
    // source content, not a marker.
    [[nodiscard]] MarkerScan markers() const;
    // Blocks in the current result tied to the choice sections they enclose,
    // in text order. A block whose marker lines or base text no launch block
    // has is orphaned.
    [[nodiscard]] std::vector<MarkerSection> markerSections() const;
    // True when the command decides the last open contribution of a marker
    // section, so applying it also removes that block's marker lines.
    [[nodiscard]] bool completesMarkers(const EditingCommand &command) const;
    [[nodiscard]] const std::vector<ChoiceTarget> &choices() const noexcept;
    [[nodiscard]] const std::vector<NonConflictingTarget> &nonConflictingTargets() const noexcept;
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
    // editor commands cannot overwrite a later revision. Changed ranges count
    // as hand edits unless recovery replay supplies its recorded anchors and
    // hand-edited result ranges.
    Transition replaceDraft(RevisionId expected, Draft draft,
                            const std::optional<std::set<std::string>> &handEdited = std::nullopt,
                            std::vector<ByteRange> handEditedRanges = {});
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
    [[nodiscard]] std::vector<MarkerSection> markerSections(const std::string &text,
                                                            const CorrespondenceMap &mapping) const;
    // Text a choice deletes to resolve a marker section, leaving `anchor`
    // empty, or a whole block it rebuilds from the text its sections keep.
    struct MarkerEdit {
        ByteRange range;
        std::string anchor;
        std::optional<std::vector<KeptSpan>> rebuilt;
    };
    [[nodiscard]] std::vector<MarkerEdit> markerEdits(
        const Draft &draft, const CorrespondenceMap &mapping,
        const std::map<std::string, Decision> &decided) const;
    std::string id_;
    std::string path_;
    Support support_;
    std::string supportDetail_;
    std::shared_ptr<const FileSnapshots> snapshots_;
    std::shared_ptr<const std::vector<ChoiceTarget>> choices_;
    std::shared_ptr<const std::vector<NonConflictingTarget>> nonConflictingTargets_;
    // Marker lines and base text of each complete block in the launch result.
    std::shared_ptr<const std::set<std::vector<std::string>>> launchBlocks_;
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
