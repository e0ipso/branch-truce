// Session state keeps result revisions, decisions and undo history separate from immutable sources.
#pragma once

#include "ChoiceCatalogue.h"
#include "ConflictMarkers.h"
#include "Correspondence.h"
#include "Draft.h"
#include "MarkerResolution.h"
#include "PublicationState.h"
#include "RevisionHistory.h"
#include "Transition.h"
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace branch_truce {

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

// One file's editing session: its immutable captured sources, the revisions
// of its result and the publication outcomes adapters report. It performs no
// I/O. Success callbacks must come from adapters that have persisted or
// verified the exact revision.
//
// A session is a value. Copies share every revision and capture, which never
// change once stored, so a copy costs pointers rather than text. A session is
// not synchronized: one thread at a time may use it, and a session built on a
// worker thread may be handed to another thread once.
class FileSession {
public:
    FileSession(std::string id, std::string path, Draft initial, Support support = Support::Supported,
                std::string supportDetail = {}, std::optional<FileSnapshots> snapshots = std::nullopt,
                bool initializeChoices = false);

    [[nodiscard]] const std::string &id() const noexcept { return id_; }
    [[nodiscard]] const std::string &path() const noexcept { return path_; }
    [[nodiscard]] Support support() const noexcept { return support_; }
    [[nodiscard]] const std::string &supportDetail() const noexcept { return supportDetail_; }
    // Shared immutable capture, retained independently of every result revision.
    // Null for sessions without an admitted source/result capture.
    [[nodiscard]] const FileSnapshots *snapshots() const noexcept { return snapshots_.get(); }
    [[nodiscard]] const Revision &current() const noexcept { return history_.current(); }
    [[nodiscard]] const CorrespondenceMap *correspondence() const noexcept { return current().correspondence.get(); }
    // Conflict-marker blocks and orphan marker lines in the current result.
    // A marker-like line the correspondence maps to identical text in a
    // captured side, or in the earlier version inside a diff3 base region, is
    // source content, not a marker.
    [[nodiscard]] const MarkerScan &markers() const noexcept { return *current().markers; }
    // Actual unresolved result regions, independent of source decisions. Each
    // recognized block or orphan marker is one stop, in result byte order.
    // Markerless drafts retain their decisions without inventing conflicts.
    [[nodiscard]] std::vector<ByteRange> conflictRegions() const;
    [[nodiscard]] std::size_t conflictCount() const noexcept
    {
        return support_ == Support::Supported ? current().draft.unresolvedMarkers : 0;
    }
    // Blocks in the current result tied to the choice sections they enclose,
    // in text order. A block whose marker lines or base text no launch block
    // has is orphaned.
    [[nodiscard]] std::vector<MarkerSection> markerSections() const;
    // True when the command decides the last open contribution of a marker
    // section, so applying it also removes that block's marker lines.
    [[nodiscard]] bool completesMarkers(const EditingCommand &command) const;
    [[nodiscard]] const std::vector<ChoiceTarget> &choices() const noexcept { return catalogue().targets(); }
    [[nodiscard]] const ChoiceCatalogue &catalogue() const noexcept;
    [[nodiscard]] const std::vector<NonConflictingTarget> &nonConflictingTargets() const noexcept;
    [[nodiscard]] const std::optional<Revision> &checkpoint() const noexcept { return publication_.checkpoint(); }
    [[nodiscard]] const std::optional<Review> &reviewed() const noexcept { return publication_.reviewed(); }
    [[nodiscard]] const std::optional<Revision> &written() const noexcept { return publication_.written(); }
    [[nodiscard]] const std::optional<StagedResult> &staged() const noexcept { return publication_.staged(); }
    [[nodiscard]] const std::optional<OperationOutcome> &checkpointOutcome() const noexcept
    {
        return publication_.checkpointOutcome();
    }
    [[nodiscard]] const std::optional<OperationOutcome> &writeOutcome() const noexcept
    {
        return publication_.writeOutcome();
    }
    [[nodiscard]] const std::optional<OperationOutcome> &stageOutcome() const noexcept
    {
        return publication_.stageOutcome();
    }
    [[nodiscard]] bool canUndo() const noexcept { return history_.canUndo(); }
    [[nodiscard]] bool canRedo() const noexcept { return history_.canRedo(); }
    [[nodiscard]] FileFacts facts() const;

    // Each editing transaction carries text and decisions together. Stale
    // editor commands cannot overwrite a later revision. Changed ranges count
    // as hand edits unless recovery replay supplies its recorded anchors and
    // hand-edited result ranges. The text must be UTF-8 with LF line endings.
    // Decision keys are the caller's recorded vocabulary and need no anchor.
    Transition replaceDraft(RevisionId expected, const Draft &draft,
                            const std::optional<std::set<std::string>> &handEdited = std::nullopt,
                            std::vector<ByteRange> handEditedRanges = {});
    // Binding records a stable source contribution without changing decisions.
    // Inspect the returned current anchor's state before offering a choice.
    Transition bindSourceAnchor(RevisionId expected, SourceAnchor source);
    Transition editResult(RevisionId expected, const ResultEdit &edit);
    // Decision keys of a command must name a registered anchor.
    Transition applyCommand(const EditingCommand &command, bool confirmReplacement = false);
    // A preview uses this exact anchor and revision. Passing confirmation after
    // either changes cannot authorize a different replacement.
    Transition replaceMapped(RevisionId expected, const ResultAnchor &anchor, std::string replacement,
                             std::optional<Decision> decision = {}, bool confirmManual = false);
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
    Transition recordStage(RevisionId revision, Outcome outcome, std::string objectId = {}, std::string mode = {},
                           std::string detail = {});
    Transition setSupport(Support support, std::string detail = {});
    // Opens a LargeFile session as it would have opened below the line
    // threshold. The loaded text gets a fresh revision and an empty history.
    Transition loadLargeFile();

private:
    // Everything opening a launch text derives, built before any member
    // changes so a failure leaves the session as it was.
    struct Launch {
        Support support = Support::Supported;
        std::string supportDetail;
        std::shared_ptr<const ChoiceCatalogue> choices;
        std::shared_ptr<const std::vector<NonConflictingTarget>> nonConflictingTargets;
        std::shared_ptr<const LaunchFrames> launchFrames;
        RevisionHistory history;
    };
    [[nodiscard]] Launch launch(RevisionId revision, Draft initial, bool correspond, bool initializeChoices) const;
    void open(Launch opened) noexcept;
    // The revision after the current one with this draft. Without a mapping
    // the current one is recomputed against the new text.
    [[nodiscard]] Revision successor(Draft draft, std::shared_ptr<const CorrespondenceMap> correspondence = {}) const;
    // A fresh revision repeating `revision`'s text, decisions and mapping.
    [[nodiscard]] Revision restored(const Revision &revision) const;
    [[nodiscard]] MarkerPlanner planner() const noexcept;
    [[nodiscard]] Transition validateCommand(const EditingCommand &command, const std::vector<ResultEdit> &edits,
                                             bool confirmReplacement) const;

    std::string id_;
    std::string path_;
    Support support_;
    std::string supportDetail_;
    std::shared_ptr<const FileSnapshots> snapshots_;
    // The conflict-marker-size Git used for this file.
    std::size_t markerSize_ = kDefaultConflictMarkerSize;
    std::shared_ptr<const ChoiceCatalogue> choices_;
    std::shared_ptr<const std::vector<NonConflictingTarget>> nonConflictingTargets_;
    // Marker lines and base text of each complete block in the launch result.
    std::shared_ptr<const LaunchFrames> launchFrames_;
    RevisionHistory history_;
    PublicationState publication_;
};

[[nodiscard]] const char *statusText(FileStatus status) noexcept;
[[nodiscard]] const char *blockerText(Blocker blocker) noexcept;

}
