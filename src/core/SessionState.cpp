// Session state keeps result revisions, decisions and undo history separate from immutable sources.
#include "SessionState.h"
#include "ResourceLimits.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace branch_truce {
namespace {
std::size_t undecidedCount(const Draft &draft)
{
    return static_cast<std::size_t>(
        std::count_if(draft.decisions.begin(), draft.decisions.end(),
                      [](const auto &entry) { return entry.second.value == DecisionValue::Undecided; }));
}

bool exceedsEnvelope(const std::string &text) { return !withinTextEnvelope(text); }

// The engine's text invariant, as TextSnapshot::utf8 documents it: UTF-8 with
// LF line endings. Adapters decode and convert line endings before this.
bool editableText(std::string_view text) noexcept
{
    return text.find('\r') == std::string_view::npos && validUtf8(text);
}

bool editableSources(const FileSnapshots &snapshots) noexcept
{
    for (const auto *source : {&snapshots.earlier, &snapshots.stage2, &snapshots.stage3})
        if (*source && !editableText((*source)->worktree.utf8)) return false;
    return true;
}

// Only an accepted contribution has a place in a combination order.
bool validDecisions(const std::map<std::string, Decision> &decisions)
{
    return std::none_of(decisions.begin(), decisions.end(), [](const auto &entry) {
        return entry.second.order && entry.second.value != DecisionValue::Accepted;
    });
}

std::size_t sourceLines(const FileSnapshots &snapshots, std::string_view result)
{
    std::size_t lines = lineCount(result);
    for (const auto *source : {&snapshots.earlier, &snapshots.stage2, &snapshots.stage3})
        if (*source) lines = std::max(lines, lineCount((*source)->worktree.utf8));
    return lines;
}

// The anchors of every choice and non-conflicting target, bound to the launch
// mapping. Empty deletions have a section control and one decision, without
// pretending that deleting several lines is a line pick.
void bindTargets(CorrespondenceMap &mapping, const ChoiceCatalogue &choices,
                 const std::vector<NonConflictingTarget> &nonConflicting)
{
    for (const auto &target : choices.targets()) {
        if (!mapping.bind(target.source)) throw std::invalid_argument("Invalid choice scope");
        if (target.source.range.begin != target.source.range.end) continue;
        for (const auto &id : target.decisions) {
            auto source = target.source;
            source.contributionId = id;
            if (!mapping.bind(std::move(source))) throw std::invalid_argument("Invalid deletion scope");
        }
    }
    for (const auto &target : nonConflicting)
        if (!mapping.bind(target.earlier) || !mapping.bind(target.source))
            throw std::invalid_argument("Invalid non-conflicting scope");
}

const ResultAnchor *targetOf(const EditingCommand &command, ByteRange range)
{
    const auto found = std::find_if(command.targets.begin(), command.targets.end(),
                                    [&](const auto &anchor) { return anchor.result == range; });
    return found == command.targets.end() ? nullptr : &*found;
}
}

FileSession::FileSession(std::string id, std::string path, Draft initial, Support support, std::string supportDetail,
                         std::optional<FileSnapshots> snapshots, bool initializeChoices,
                         std::shared_ptr<const CorrespondenceAlgorithm> algorithm)
    : id_(std::move(id)),
      path_(std::move(path)),
      support_(support),
      supportDetail_(std::move(supportDetail)),
      algorithm_(algorithm ? std::move(algorithm) : std::make_shared<const ExactLineCorrespondence>())
{
    if (id_.empty() || path_.empty()) throw std::invalid_argument("A file session needs an identity and path");
    if (snapshots) {
        if (snapshots->path != path_ || snapshots->launchResult.text.utf8 != initial.text)
            throw std::invalid_argument("Captured sources and launch result must belong to this file session");
        snapshots_ = std::make_shared<const FileSnapshots>(std::move(*snapshots));
    }
    const auto hold = [&](Support held, std::string detail) {
        support_ = held;
        supportDetail_ = std::move(detail);
        open(launch(1, std::move(initial), false, false));
    };
    if (snapshots_) {
        const auto &attributes = snapshots_->conversion.attributes;
        const auto attribute = attributes.find("conflict-marker-size");
        const auto size = attribute == attributes.end() ? std::optional{kDefaultConflictMarkerSize}
                                                        : conflictMarkerSize(attribute->second);
        if (!size && support_ == Support::Supported) {
            hold(Support::OutsideApp,
                 "Outside app: conflict-marker-size is over " + std::to_string(kMaxConflictMarkerSize));
            return;
        }
        markerSize_ = size.value_or(kDefaultConflictMarkerSize);
    }
    const auto resultBudget = assessText(initial.text);
    if (resultBudget.limit != TextLimit::Within) {
        hold(Support::Unreadable, "Load error: " + textLimitDiagnostic(resultBudget));
        return;
    }
    if (support_ == Support::Supported
        && (!editableText(initial.text) || (snapshots_ && !editableSources(*snapshots_)))) {
        hold(Support::Unreadable, "Load error: text is not UTF-8 with LF line endings");
        return;
    }
    if (snapshots_) {
        const auto lines = sourceLines(*snapshots_, initial.text);
        if (lines > kLargeFileLines) {
            hold(Support::LargeFile,
                 "File of " + std::to_string(lines) + " lines is over " + std::to_string(kLargeFileLines)
                     + " lines and was not loaded");
            return;
        }
    }
    open(launch(1, std::move(initial), true, initializeChoices));
}

FileSession::Launch FileSession::launch(RevisionId revision, Draft initial, bool correspond,
                                        bool initializeChoices) const
{
    Launch result{support_, supportDetail_, {}, {}, {}, {}};
    std::shared_ptr<const CorrespondenceMap> correspondence;
    if (correspond && snapshots_) {
        try {
            auto bound = std::make_shared<CorrespondenceMap>(revision, *snapshots_, initial.text, algorithm_);
            auto choices = std::make_shared<const ChoiceCatalogue>(choiceTargets(*snapshots_, *algorithm_));
            auto nonConflicting = std::make_shared<const std::vector<NonConflictingTarget>>(
                branch_truce::nonConflictingTargets(*snapshots_, choices->targets(), *algorithm_));
            bindTargets(*bound, *choices, *nonConflicting);
            settleLaunchAnchors(*bound, initial.text, markerSize_);
            if (initializeChoices)
                for (const auto &target : choices->targets())
                    for (const auto &id : target.decisions) initial.decisions.try_emplace(id);
            correspondence = std::move(bound);
            result.choices = std::move(choices);
            result.nonConflictingTargets = std::move(nonConflicting);
        } catch (const std::exception &error) {
            result.support = Support::Unreadable;
            result.supportDetail = "Load error: could not build correspondence for " + path_ + ": " + error.what();
        }
    }
    auto scan = resultMarkers(initial.text, markerSize_, correspondence.get());
    if (snapshots_) result.launchFrames = std::make_shared<const LaunchFrames>(launchFrames(initial.text, scan));
    // Counted again with the launch frames, as every later revision is.
    scan = resultMarkers(initial.text, markerSize_, correspondence.get(), result.launchFrames.get());
    initial.unresolvedMarkers = scan.count();
    result.history =
        RevisionHistory(Revision{revision, revision, revision, std::move(initial), std::move(correspondence),
                                 std::make_shared<const MarkerScan>(std::move(scan))});
    return result;
}

void FileSession::open(Launch opened) noexcept
{
    support_ = opened.support;
    supportDetail_ = std::move(opened.supportDetail);
    choices_ = std::move(opened.choices);
    nonConflictingTargets_ = std::move(opened.nonConflictingTargets);
    launchFrames_ = std::move(opened.launchFrames);
    publication_ = PublicationState(opened.history.launchId());
    history_ = std::move(opened.history);
}

const ChoiceCatalogue &FileSession::catalogue() const noexcept
{
    static const ChoiceCatalogue empty;
    return choices_ ? *choices_ : empty;
}

const std::vector<NonConflictingTarget> &FileSession::nonConflictingTargets() const noexcept
{
    static const std::vector<NonConflictingTarget> empty;
    return nonConflictingTargets_ ? *nonConflictingTargets_ : empty;
}

MarkerPlanner FileSession::planner() const noexcept
{
    static const LaunchFrames none;
    return {catalogue(), launchFrames_ ? *launchFrames_ : none, markerSize_};
}

std::vector<ByteRange> FileSession::conflictRegions() const
{
    if (support_ != Support::Supported) return {};
    const auto &scan = markers();
    std::vector<ByteRange> regions;
    regions.reserve(scan.count());
    for (const auto &block : scan.blocks) regions.push_back({block.begin.bytes.begin, block.end.bytes.end});
    for (const auto &orphan : scan.orphans) regions.push_back(orphan.bytes);
    std::sort(regions.begin(), regions.end(),
              [](const auto &left, const auto &right) { return left.begin < right.begin; });
    return regions;
}

std::vector<MarkerSection> FileSession::markerSections() const
{
    const auto *mapping = correspondence();
    if (!mapping || !launchFrames_ || !choices_) return {};
    return planner().sections(current().draft.text, *mapping, &markers());
}

bool FileSession::completesMarkers(const EditingCommand &command) const
{
    const auto *mapping = correspondence();
    if (!mapping || command.scope != EditingScope::Mapped || !launchFrames_ || !choices_) return false;
    auto draft = current().draft;
    for (const auto &[id, value] : command.decisions) draft.decisions[id] = value;
    return !planner().edits(draft, *mapping, command.decisions, &markers()).empty();
}

Revision FileSession::successor(Draft draft, std::shared_ptr<const CorrespondenceMap> correspondence) const
{
    const auto id = history_.nextId();
    if (id == std::numeric_limits<RevisionId>::max()) throw std::overflow_error("Session revision space exhausted");
    const auto &previous = current();
    if (!correspondence && previous.correspondence)
        correspondence = std::make_shared<const CorrespondenceMap>(previous.correspondence->recomputed(id, draft.text));
    // Counted against this revision's own passages, so replay derives the same count.
    auto scan = resultMarkers(draft.text, markerSize_, correspondence.get(), launchFrames_.get());
    draft.unresolvedMarkers = scan.count();
    const auto result = draft.text == previous.draft.text ? previous.result : id;
    const auto decisions =
        draft.decisions == previous.draft.decisions && draft.unresolvedMarkers == previous.draft.unresolvedMarkers
        ? previous.decisions
        : id;
    return {id,
            result,
            decisions,
            std::move(draft),
            std::move(correspondence),
            std::make_shared<const MarkerScan>(std::move(scan))};
}

Revision FileSession::restored(const Revision &revision) const
{
    auto correspondence = revision.correspondence
        ? std::make_shared<const CorrespondenceMap>(revision.correspondence->atRevision(history_.nextId()))
        : nullptr;
    auto copy = successor(revision.draft, std::move(correspondence));
    // Restored content can still be the manually saved content. The transaction
    // identity is fresh, so restoring it never restores review.
    copy.result = revision.result;
    copy.decisions = revision.decisions;
    return copy;
}

Transition FileSession::replaceDraft(RevisionId expected, const Draft &draft,
                                     const std::optional<std::set<std::string>> &handEdited,
                                     std::vector<ByteRange> handEditedRanges)
{
    if (expected != current().id) return Transition::StaleRevision;
    if (support_ != Support::Supported) return Transition::Blocked;
    if (exceedsEnvelope(draft.text)) return Transition::Blocked;
    if (!editableText(draft.text) || !validDecisions(draft.decisions)) return Transition::InvalidAnchor;
    if (!handEdited && !handEditedRanges.empty()) return Transition::InvalidAnchor;
    // The marker count follows from the text, so a supplied count changes nothing.
    const bool unchanged = draft.text == current().draft.text && draft.decisions == current().draft.decisions;
    std::shared_ptr<const CorrespondenceMap> mapping;
    if (handEdited && correspondence()) {
        auto replayed = correspondence()->recomputed(history_.nextId(), draft.text);
        if (!replayed.restoreHandEdits(*handEdited) || !replayed.restoreHandEditedRanges(std::move(handEditedRanges)))
            return Transition::InvalidAnchor;
        // A recorded choice can change only the hand-edit flags and ranges.
        if (unchanged && replayed.handEditedAnchors() == correspondence()->handEditedAnchors()
            && replayed.handEditedRanges() == correspondence()->handEditedRanges())
            return Transition::NoChange;
        mapping = std::make_shared<const CorrespondenceMap>(std::move(replayed));
    } else if (unchanged) {
        return Transition::NoChange;
    } else if (handEdited && (!handEdited->empty() || !handEditedRanges.empty())) {
        return Transition::InvalidAnchor;
    }
    history_.publish(successor(draft, std::move(mapping)), RevisionHistory::Step::Edit);
    return Transition::Applied;
}

Transition FileSession::bindSourceAnchor(RevisionId expected, SourceAnchor source)
{
    if (expected != current().id) return Transition::StaleRevision;
    if (support_ != Support::Supported) return Transition::Blocked;
    const auto *mapping = correspondence();
    if (!mapping) return Transition::InvalidAnchor;
    if (const auto *existing = mapping->anchor(source.contributionId))
        return existing->source == source ? Transition::NoChange : Transition::InvalidAnchor;
    auto updated = std::make_shared<CorrespondenceMap>(*mapping);
    if (!updated->bind(std::move(source))) return Transition::InvalidAnchor;
    auto revision = current();
    revision.correspondence = std::move(updated);
    history_.replaceCurrent(std::move(revision));
    return Transition::Applied;
}

Transition FileSession::editResult(RevisionId expected, const ResultEdit &edit)
{
    if (expected != current().id) return Transition::StaleRevision;
    if (support_ != Support::Supported) return Transition::Blocked;
    // The stored text already holds the invariant, so only the edit is checked.
    if (!validByteRange(current().draft.text, edit.range) || !editableText(edit.replacement))
        return Transition::InvalidAnchor;
    auto draft = current().draft;
    draft.text.replace(edit.range.begin, edit.range.end - edit.range.begin, edit.replacement);
    if (exceedsEnvelope(draft.text)) return Transition::Blocked;
    if (draft == current().draft) return Transition::NoChange;
    auto mapping = correspondence()
        ? std::make_shared<const CorrespondenceMap>(correspondence()->edited(history_.nextId(), edit))
        : nullptr;
    history_.publish(successor(std::move(draft), std::move(mapping)), RevisionHistory::Step::Edit);
    return Transition::Applied;
}

Transition FileSession::validateCommand(const EditingCommand &command, const std::vector<ResultEdit> &edits,
                                        bool confirmReplacement) const
{
    const auto &text = current().draft.text;
    const bool wholeFile = command.scope == EditingScope::WholeFile;
    if (wholeFile) {
        if (!command.targets.empty() || edits.size() != 1 || edits.front().range != ByteRange{0, text.size()})
            return Transition::InvalidAnchor;
        if (!confirmReplacement) return Transition::ConfirmationRequired;
    } else if (command.targets.empty()) {
        return Transition::InvalidAnchor;
    }
    for (const auto &target : command.targets) {
        if (target.revision != current().id) return Transition::StaleRevision;
        const auto *bound = correspondence()->anchor(target.source.contributionId);
        if (!bound || *bound != target) return Transition::InvalidAnchor;
        if (target.state == AnchorState::Ambiguous) return Transition::AmbiguousTarget;
        if (target.state != AnchorState::Mapped || !target.result) return Transition::InvalidAnchor;
    }
    for (std::size_t i = 0; i < edits.size(); ++i) {
        const auto &edit = edits[i];
        if (!validByteRange(text, edit.range) || !editableText(edit.replacement)) return Transition::InvalidAnchor;
        if (i && (edit.range.end > edits[i - 1].range.begin || edit.range.begin == edits[i - 1].range.begin))
            return Transition::AmbiguousTarget;
        if (wholeFile) continue;
        const auto *target = targetOf(command, edit.range);
        if (!target) return Transition::InvalidAnchor;
        if (!confirmReplacement && std::any_of(command.targets.begin(), command.targets.end(), [&](const auto &anchor) {
                return anchor.result == edit.range && anchor.manualOverlap;
            }))
            return Transition::ConfirmationRequired;
    }
    if (!validDecisions(command.decisions)) return Transition::InvalidAnchor;
    for (const auto &[id, value] : command.decisions)
        if (!correspondence()->anchor(id)) return Transition::InvalidAnchor;
    return Transition::Applied;
}

Transition FileSession::applyCommand(const EditingCommand &command, bool confirmReplacement)
{
    if (command.expected != current().id) return Transition::StaleRevision;
    if (support_ != Support::Supported) return Transition::Blocked;
    const auto *mapping = correspondence();
    if (!mapping) return Transition::InvalidAnchor;
    auto edits = command.edits;
    std::sort(edits.begin(), edits.end(), [](const auto &a, const auto &b) { return a.range.begin > b.range.begin; });
    if (const auto refused = validateCommand(command, edits, confirmReplacement); refused != Transition::Applied)
        return refused;
    const bool wholeFile = command.scope == EditingScope::WholeFile;
    const auto next = history_.nextId();
    auto draft = current().draft;
    for (const auto &[id, value] : command.decisions) draft.decisions[id] = value;
    auto updated = std::make_shared<CorrespondenceMap>(*mapping);
    for (const auto &edit : edits) {
        draft.text.replace(edit.range.begin, edit.range.end - edit.range.begin, edit.replacement);
        if (exceedsEnvelope(draft.text)) return Transition::Blocked;
        *updated = wholeFile ? updated->recomputed(next, draft.text, false)
                             : updated->edited(next, edit, false, targetOf(command, edit.range)->source.contributionId);
    }
    // Deciding a marker section's last contribution resolves its block in
    // this same history entry, so one undo brings the markers back.
    if (!wholeFile && launchFrames_ && choices_) {
        for (const auto &edit : planner().edits(draft, *updated, command.decisions)) {
            *updated = edit.rebuilt ? updated->rebuilt(next, edit.range, *edit.rebuilt)
                                    : updated->edited(next, {edit.range, {}}, false, edit.anchor);
            draft.text = updated->text();
        }
    }
    // Choosing the text a hand edit left unchanged still replaces that edit.
    if (draft == current().draft && updated->handEditedAnchors() == mapping->handEditedAnchors()
        && updated->handEditedRanges() == mapping->handEditedRanges())
        return Transition::NoChange;
    history_.publish(successor(std::move(draft), std::make_shared<const CorrespondenceMap>(updated->atRevision(next))),
                     RevisionHistory::Step::Edit);
    return Transition::Applied;
}

Transition FileSession::replaceMapped(RevisionId expected, const ResultAnchor &anchor, std::string replacement,
                                      std::optional<Decision> decision, bool confirmManual)
{
    if (expected != current().id || anchor.revision != current().id) return Transition::StaleRevision;
    if (support_ != Support::Supported) return Transition::Blocked;
    const auto *mapping = correspondence();
    const auto *bound = mapping ? mapping->anchor(anchor.source.contributionId) : nullptr;
    if (!bound || *bound != anchor) return Transition::InvalidAnchor;
    if (anchor.state == AnchorState::Ambiguous) return Transition::AmbiguousTarget;
    if (anchor.state != AnchorState::Mapped || !anchor.result || !validByteRange(current().draft.text, *anchor.result)
        || !editableText(replacement))
        return Transition::InvalidAnchor;
    if (anchor.manualOverlap && !confirmManual) return Transition::ConfirmationRequired;
    auto draft = current().draft;
    draft.text.replace(anchor.result->begin, anchor.result->end - anchor.result->begin, replacement);
    if (exceedsEnvelope(draft.text)) return Transition::Blocked;
    if (decision) draft.decisions[anchor.source.contributionId] = *decision;
    if (draft == current().draft) return Transition::NoChange;
    const ResultEdit edit{*anchor.result, std::move(replacement)};
    auto updated = std::make_shared<const CorrespondenceMap>(
        mapping->edited(history_.nextId(), edit, false, anchor.source.contributionId));
    history_.publish(successor(std::move(draft), std::move(updated)), RevisionHistory::Step::Edit);
    return Transition::Applied;
}

Transition FileSession::undo(RevisionId expected)
{
    if (expected != current().id) return Transition::StaleRevision;
    if (support_ != Support::Supported) return Transition::Blocked;
    if (!canUndo()) return Transition::NoChange;
    history_.publish(restored(history_.previous()), RevisionHistory::Step::Undo);
    return Transition::Applied;
}

Transition FileSession::redo(RevisionId expected)
{
    if (expected != current().id) return Transition::StaleRevision;
    if (support_ != Support::Supported) return Transition::Blocked;
    if (!canRedo()) return Transition::NoChange;
    history_.publish(restored(history_.following()), RevisionHistory::Step::Redo);
    return Transition::Applied;
}

Transition FileSession::discardToCheckpoint(RevisionId expected)
{
    if (expected != current().id) return Transition::StaleRevision;
    if (support_ != Support::Supported) return Transition::Blocked;
    const auto &checkpoint = publication_.checkpoint();
    const auto &target = checkpoint ? *checkpoint : *history_.find(history_.launchId());
    // Matching text after undo can still have pending Save callbacks from a
    // discarded revision. Only the checkpoint's own successful transaction is
    // a no-op; otherwise restore its identity and fence those callbacks below.
    if (target.id == current().id && publication_.savedAt(target.id)) return Transition::NoChange;
    history_.publish(restored(target), RevisionHistory::Step::Edit);
    publication_.discardedToCheckpoint(current().id);
    return Transition::Applied;
}

Transition FileSession::beginMarkResolved(RevisionId expected, bool confirmUndecided)
{
    if (expected != current().id) return Transition::StaleRevision;
    if (support_ != Support::Supported || current().draft.unresolvedMarkers != 0) return Transition::Blocked;
    if (undecidedCount(current().draft) != 0 && !confirmUndecided) return Transition::ConfirmationRequired;
    publication_.markResolved(current(), confirmUndecided);
    return Transition::Applied;
}

Transition FileSession::recordCheckpoint(RevisionId revision, Outcome outcome, std::string detail)
{
    return publication_.recordCheckpoint(history_.find(revision), revision, current().id, outcome, std::move(detail));
}

Transition FileSession::recordWrite(RevisionId revision, Outcome outcome, std::string detail)
{
    return publication_.recordWrite(revision, current().id, outcome, std::move(detail));
}

Transition FileSession::recordStage(RevisionId revision, Outcome outcome, std::string objectId, std::string mode,
                                    std::string detail)
{
    return publication_.recordStage(revision, current().id, outcome, std::move(objectId), std::move(mode),
                                    std::move(detail));
}

Transition FileSession::discardToStaged(RevisionId expected)
{
    if (expected != current().id) return Transition::StaleRevision;
    if (support_ != Support::Supported || !publication_.stagedVerified()) return Transition::Blocked;
    const auto &staged = publication_.staged()->revision;
    if (current().id == staged.id) return Transition::NoChange;
    const auto floor = history_.nextId();
    history_.reopen(staged);
    publication_.discardedToStaged(floor);
    return Transition::Applied;
}

Transition FileSession::setSupport(Support support, std::string detail)
{
    if (support_ == support && supportDetail_ == detail) return Transition::NoChange;
    support_ = support;
    supportDetail_ = std::move(detail);
    return Transition::Applied;
}

Transition FileSession::loadLargeFile()
{
    if (support_ != Support::LargeFile) return Transition::NoChange;
    // Every edit and outcome is blocked until the file loads, so the launch
    // revision is the only one. It never had choices or a correspondence.
    if (!snapshots_ || current().id != history_.launchId() || history_.depth() != 1 || !publication_.untouched())
        return Transition::Blocked;
    const auto revision = history_.nextId();
    if (revision == std::numeric_limits<RevisionId>::max())
        throw std::overflow_error("Session revision space exhausted");
    Draft initial{current().draft.text, current().draft.decisions, 0};
    auto loaded = launch(revision, std::move(initial), true, true);
    if (loaded.support == Support::LargeFile) loaded.support = Support::Supported;
    if (loaded.support == Support::Supported) loaded.supportDetail.clear();
    open(std::move(loaded));
    return Transition::Applied;
}

FileFacts FileSession::facts() const
{
    return publication_.facts(current(), support_, undecidedCount(current().draft));
}

const char *statusText(FileStatus status) noexcept
{
    switch (status) {
    case FileStatus::OutsideApp: return "Needs work outside Branch Truce";
    case FileStatus::Unreadable: return "File could not be read";
    case FileStatus::LargeFile: return "Large file not loaded";
    case FileStatus::OutcomeUncertain: return "Outcome uncertain; verification required";
    case FileStatus::CheckpointFailed: return "Draft save failed";
    case FileStatus::WriteFailed: return "Repository write failed";
    case FileStatus::StageFailed: return "Written, unresolved";
    case FileStatus::EarlierStaged: return "Earlier result staged; current draft not applied";
    case FileStatus::Resolved: return "Written and resolved";
    case FileStatus::WrittenUnresolved: return "Written, unresolved";
    case FileStatus::SavedDraft: return "Draft saved in Branch Truce; repository file unchanged";
    case FileStatus::Undecided: return "Source choices remain undecided";
    case FileStatus::ReviewNeeded: return "Current result needs review";
    }
    return "";
}

const char *blockerText(Blocker blocker) noexcept
{
    switch (blocker) {
    case Blocker::OutsideApp: return "Handle this file outside Branch Truce";
    case Blocker::Unreadable: return "File content unavailable";
    case Blocker::LargeFile: return "Large file waits for confirmation to load";
    case Blocker::UnresolvedMarkers: return "Known conflict markers remain";
    case Blocker::Undecided: return "Undecided source choices need confirmation";
    case Blocker::UnsavedDraft: return "Current draft has no manual Save checkpoint";
    case Blocker::ReviewNeeded: return "Current result needs Mark resolved";
    case Blocker::WriteRequired: return "Current result has not been verified written";
    case Blocker::StageRequired: return "Current result has not been verified staged";
    case Blocker::EarlierStagedResult: return "Earlier result staged; current draft not applied";
    case Blocker::CheckpointFailed: return "Draft save failed; retry or discard explicitly";
    case Blocker::WriteFailed: return "Repository write failed";
    case Blocker::StageFailed: return "Git resolution failed";
    case Blocker::UncertainOutcome: return "Inspect uncertain mutation before finishing";
    }
    return "";
}
}
