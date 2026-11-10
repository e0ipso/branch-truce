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
#include <string_view>
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

// A revision a recovery journal recorded: its id, the id of the revision whose
// result text it repeats, its draft and its hand-edited anchors and ranges.
struct RecordedRevision {
    RevisionId id = 0;
    RevisionId result = 0;
    Draft draft;
    std::set<std::string> handEdited;
    std::vector<ByteRange> handEditedRanges;
};

// How editResult records an edit.
struct EditOptions {
    // Deferred leaves the passages provisional for a worker to align (see
    // FileSession).
    Realignment realignment = Realignment::Now;
    // Joins the edit to the current undo entry, so one undo takes back a burst
    // of typing and a recovery journal can record the burst once. The first
    // entry, where the history starts, is never joined.
    bool join = false;
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
//
// Aligning the sources with an edited result costs time in proportion to the
// product of their line counts, so an editor can defer it. The owning thread
// edits with Realignment::Deferred and hands pendingAlignment() to a worker.
// That map is immutable and shared, so the worker may call realigned() on it
// while the owner keeps editing; the worker touches no session. The owner then
// passes the result to installAlignment(), which keeps it only if the revision
// it was made for is still current. Until then the revision's passages are
// provisional (see CorrespondenceMap::edited), and so are the marker scan and
// the word differences built from them.
class FileSession {
public:
    // `algorithm` aligns the captured sources with the result and the two
    // sides with each other; without one, ExactLineCorrespondence does.
    FileSession(std::string id, std::string path, Draft initial, Support support = Support::Supported,
                std::string supportDetail = {}, std::optional<FileSnapshots> snapshots = std::nullopt,
                bool initializeChoices = false, std::shared_ptr<const CorrespondenceAlgorithm> algorithm = {});

    [[nodiscard]] const std::string &id() const noexcept { return id_; }
    [[nodiscard]] const std::string &path() const noexcept { return path_; }
    [[nodiscard]] Support support() const noexcept { return support_; }
    // The alignment the session's choices and decision IDs come from.
    [[nodiscard]] const CorrespondenceAlgorithm &algorithm() const noexcept { return *algorithm_; }
    // The adapter's explanation of its support decision, if it gave one.
    [[nodiscard]] const std::string &supportDetail() const noexcept { return supportDetail_; }
    // Why the session itself held the file back, if it did.
    [[nodiscard]] const std::optional<LoadFailure> &loadFailure() const noexcept { return loadFailure_; }
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
    // Ties the blocks of this session's results to its choice sections. It
    // refers to the session, so it is valid only while the session is unchanged.
    [[nodiscard]] MarkerPlanner planner() const noexcept;
    [[nodiscard]] const std::vector<ChoiceTarget> &choices() const noexcept { return catalogue().targets(); }
    [[nodiscard]] const ChoiceCatalogue &catalogue() const noexcept;
    [[nodiscard]] const std::vector<NonConflictingTarget> &nonConflictingTargets() const noexcept;
    // The Save, review, write and stage outcomes adapters reported, naming
    // revisions by id. Every revision it names stays stored.
    [[nodiscard]] const PublicationState &publication() const noexcept { return publication_; }
    // A stored revision: the current one, an undo history entry, the launch
    // revision or one the publication state names. Null once it is dropped.
    [[nodiscard]] const Revision *revision(RevisionId id) const noexcept { return history_.find(id); }
    [[nodiscard]] bool canUndo() const noexcept { return history_.canUndo(); }
    [[nodiscard]] bool canRedo() const noexcept { return history_.canRedo(); }
    [[nodiscard]] FileFacts facts() const;

    // Each editing transaction carries text and decisions together. Stale
    // editor commands cannot overwrite a later revision. A whole-buffer update
    // has no edit location, so ranges its comparison changed count as hand
    // edits. The text must be UTF-8 with LF line endings. Decision keys are
    // the caller's recorded vocabulary and need no anchor.
    Transition replaceDraft(RevisionId expected, const Draft &draft);
    // Restores a revision a recovery journal recorded, in one step however
    // many commands made it, so a journal can record only the last revision
    // of a burst of edits. Its id is at least the next revision id; its result
    // id is its own, or the current one's when the text is unchanged
    // (InvalidOutcome otherwise). The recorded hand-edit flags and ranges
    // replace the ones the comparison derives (InvalidAnchor if they do not
    // fit). Text and decisions are checked as replaceDraft checks them.
    Transition replayDraft(RevisionId expected, const RecordedRevision &recorded);
    Transition editResult(RevisionId expected, const ResultEdit &edit, EditOptions options = {});
    // The current revision's correspondence while its passages are
    // provisional, or null. Safe to read from any thread.
    [[nodiscard]] std::shared_ptr<const CorrespondenceMap> pendingAlignment() const noexcept;
    // Replaces the current revision's provisional passages with `aligned`, a
    // map pendingAlignment() returned for `revision` and realigned() built.
    // The revision keeps its id, text, decisions and anchors; its marker scan
    // is counted again. StaleRevision once another revision is current or the
    // passages are no longer pending; InvalidAnchor for a map of other anchors.
    // Recovery records hold nothing it changes.
    Transition installAlignment(RevisionId revision, std::shared_ptr<const CorrespondenceMap> aligned);
    // Decision keys of a command must name a registered anchor.
    Transition applyCommand(const EditingCommand &command, bool confirmReplacement = false);
    Transition undo(RevisionId expected);
    Transition redo(RevisionId expected);
    Transition discardToCheckpoint(RevisionId expected);
    // A deliberate discard may reuse verified identity only while both the
    // worktree and index are still known to contain that staged revision.
    Transition discardToStaged(RevisionId expected);

    // Captures the review declaration of Mark resolved, not a review-only UI
    // action. Cancelling confirmation calls nothing and changes nothing.
    Transition beginMarkResolved(RevisionId expected, bool confirmUndecided = false);
    // Outcomes are for revisions the session still stores: the current one,
    // an undo history entry or the launch revision. An outcome for a revision
    // a later edit discarded is stale.
    Transition recordCheckpoint(RevisionId revision, Outcome outcome, std::string detail = {});
    Transition recordWrite(RevisionId revision, Outcome outcome, std::string detail = {});
    Transition recordStage(RevisionId revision, Outcome outcome, std::string objectId = {}, std::string mode = {},
                           std::string detail = {});
    // An adapter's own support decision, which replaces any load failure.
    // An adapter may hold a file back as OutsideApp or Unreadable at any
    // time. Supported admits a file it held back only if the session's own
    // checks pass, as they do at launch, and aligns a captured file whose
    // launch revision is still untouched; otherwise the result is Blocked.
    // A hold the session made itself (loadFailure()) is never lifted here,
    // and LargeFile is the session's decision, so it is InvalidOutcome.
    Transition setSupport(Support support, std::string detail = {});
    // Opens a LargeFile session as it would have opened below the line
    // threshold. The loaded text gets a fresh revision and an empty history.
    Transition loadLargeFile();

private:
    // Why the session holds a text back instead of editing it.
    struct Hold {
        Support support;
        LoadFailure failure;
    };
    // The hold for `text` when an adapter decided `support`, or nothing when
    // the session can open it that way. Only a file the app edits has its
    // captures and line count checked.
    [[nodiscard]] std::optional<Hold> holdFor(std::string_view text, Support support) const;
    // Everything opening a launch text derives, built before any member
    // changes so a failure leaves the session as it was.
    struct Launch {
        Support support = Support::Supported;
        std::optional<LoadFailure> failure;
        std::shared_ptr<const ChoiceCatalogue> choices;
        std::shared_ptr<const std::vector<NonConflictingTarget>> nonConflictingTargets;
        std::shared_ptr<const LaunchFrames> launchFrames;
        RevisionHistory history;
    };
    // A Supported launch of a captured file is aligned; any other is not.
    [[nodiscard]] Launch launch(RevisionId revision, Draft initial, Support support, std::optional<LoadFailure> failure,
                                bool initializeChoices) const;
    void open(Launch opened) noexcept;
    // The launch revision is the only one and no outcome was recorded, so
    // opening the text again loses nothing.
    [[nodiscard]] bool launchUntouched() const noexcept;
    // Makes `next` the publication state and keeps every revision it names.
    void adopt(PublicationState next);
    // The untouched launch revision of a captured file opened again as
    // Supported and aligned, or nothing when it is no longer untouched.
    [[nodiscard]] std::optional<Launch> relaunchSupported() const;
    // The revision after the current one with this draft, numbered `id` or
    // the next id. Without a mapping the current one is recomputed against
    // the new text.
    [[nodiscard]] Revision successor(Draft draft, std::shared_ptr<const CorrespondenceMap> correspondence = {},
                                     std::optional<RevisionId> id = std::nullopt) const;
    // Why a draft cannot replace the current one, or Applied.
    [[nodiscard]] Transition validateDraft(RevisionId expected, const Draft &draft) const;
    // A fresh revision repeating `revision`'s text, decisions and mapping.
    [[nodiscard]] Revision restored(const Revision &revision) const;
    [[nodiscard]] Transition validateCommand(const EditingCommand &command, const std::vector<ResultEdit> &edits,
                                             bool confirmReplacement) const;
    [[nodiscard]] Transition validateTarget(const ResultAnchor &target) const;
    [[nodiscard]] Transition validateEdit(const EditingCommand &command, const ResultEdit &edit,
                                          const ResultEdit *previous, bool confirmReplacement) const;

    std::string id_;
    std::string path_;
    Support support_;
    std::string supportDetail_;
    std::optional<LoadFailure> loadFailure_;
    std::shared_ptr<const FileSnapshots> snapshots_;
    std::shared_ptr<const CorrespondenceAlgorithm> algorithm_;
    // The conflict-marker-size Git used for this file.
    std::size_t markerSize_ = kDefaultConflictMarkerSize;
    std::shared_ptr<const ChoiceCatalogue> choices_;
    std::shared_ptr<const std::vector<NonConflictingTarget>> nonConflictingTargets_;
    // Marker lines and base text of each complete block in the launch result.
    std::shared_ptr<const LaunchFrames> launchFrames_;
    RevisionHistory history_;
    PublicationState publication_;
};

}
