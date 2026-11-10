// Source choice commands apply accept, ignore and combined-side decisions to a result revision.
#include "EditingCommands.h"

#include <algorithm>
#include <array>
#include <optional>
#include <set>
#include <vector>

namespace branch_truce {
namespace detail {
// Commands stay inside the engine. Applying regenerates the displayed plan
// from its revision and selection, then checks every displayed value.
template <typename Preview> struct Planned : Preview {
    EditingCommand &command() { return this->apply.command_; }
    const EditingCommand &command() const { return this->apply.command_; }
};
}
namespace {
using ChoicePlan = detail::Planned<ChoicePreview>;
using BatchPlan = detail::Planned<NonConflictingPreview>;
using WholeFilePlan = detail::Planned<WholeFilePreview>;

// Whether two result ranges claim the same text or the same insertion site.
// Unlike overlaps, two equal empty ranges do.
bool sharesPlace(ByteRange a, ByteRange b) { return a == b || overlaps(a, b); }

bool adjoining(ByteRange a, ByteRange b) { return a == b || a.end == b.begin || b.end == a.begin; }

// The result range of a mapped anchor, or nothing when it has none.
std::optional<ByteRange> mappedRange(const ResultAnchor *anchor) noexcept
{
    if (!anchor || anchor->state != AnchorState::Mapped) return std::nullopt;
    return anchor->result;
}

Decision decision(const Draft &draft, const std::string &id)
{
    const auto found = draft.decisions.find(id);
    return found == draft.decisions.end() ? Decision{} : found->second;
}

// The captured text of a choice scope. Choices exist only when both sides
// were captured, and their ranges come from those captures.
std::string sourceText(const FileSnapshots &snapshots, const SourceAnchor &source)
{
    return std::string(slice(snapshots.source(source.side)->worktree.utf8, source.range));
}

ChoicePlan failed(ChoicePlan preview, Transition state, ChoiceNote note)
{
    preview.state = state;
    preview.note = note;
    return preview;
}

// A scope the user selected, with everything a choice on it reads. Its anchor
// is mapped, which previewChoice checks before building one. The marker
// sections of the current result are listed once per preview.
struct Selection {
    const FileSnapshots &snapshots;
    const CorrespondenceMap &mapping;
    const ChoiceCatalogue &choices;
    const Draft &draft;
    const ChoiceTarget &target;
    const ResultAnchor &anchor;
    ByteRange range;
    MarkerPlanner planner;
    std::vector<MarkerSection> sections;

    // The marker section that encloses the selected scope's section, if any.
    [[nodiscard]] const MarkerSection *block() const
    {
        const auto found = std::find_if(sections.begin(), sections.end(),
                                        [&](const auto &section) { return section.encloses(target.section); });
        return found == sections.end() ? nullptr : &*found;
    }
    [[nodiscard]] bool marked() const { return block() != nullptr; }

    // True when the command decides the last open contribution of a marker
    // section, so applying it also removes that block's marker lines.
    [[nodiscard]] bool completesMarkers(const EditingCommand &command) const
    {
        if (command.scope != EditingScope::Mapped || sections.empty()) return false;
        auto decided = draft;
        for (const auto &[id, value] : command.decisions) decided.decisions[id] = value;
        return !planner.edits(decided, mapping, command.decisions, sections).empty();
    }
};

// A refusal that leaves the preview's command unusable, or nothing.
using Refusal = std::optional<std::pair<Transition, ChoiceNote>>;

// The competing scope of a combination: one mapped scope of the other side
// in the same section, the same range winning over adjacent ones.
struct Competitor {
    const ChoiceTarget *target = nullptr;
    const ResultAnchor *anchor = nullptr;
    ByteRange range;
    std::size_t matches = 0;
};

Competitor competitor(const Selection &selected, bool marked)
{
    Competitor found;
    bool exact = false;
    for (const auto &target : selected.choices.targets()) {
        if (target.section != selected.target.section || target.scope != selected.target.scope
            || target.source.side == selected.target.source.side)
            continue;
        const auto *candidate = selected.mapping.anchor(target.source.contributionId);
        const auto range = mappedRange(candidate);
        if (!range || !(marked || adjoining(selected.range, *range))) continue;
        const bool sameRange = selected.range == *range;
        // Neighboring lines are also adjacent. An exact competing range has
        // priority, so an unrelated adjacent line cannot make it ambiguous.
        if (sameRange && !exact) {
            exact = true;
            found.matches = 0;
        }
        if (!sameRange && exact) continue;
        ++found.matches;
        found.target = &target;
        found.anchor = candidate;
        found.range = *range;
    }
    return found;
}

// A partial combination must not erase another contribution, even if it is
// still undecided. Unknown line mappings within this section are unsafe.
Refusal unselectedContribution(const Selection &selected, const EditingCommand &command,
                               const std::vector<ByteRange> &replaced)
{
    std::set<std::string> inspected;
    for (const auto &target : selected.choices.targets()) {
        for (const auto &id : target.decisions) {
            if (command.decisions.contains(id) || !inspected.insert(id).second) continue;
            const auto *unselected = selected.mapping.anchor(id);
            const auto place = mappedRange(unselected);
            if (!place) {
                if (target.section == selected.target.section)
                    return {{Transition::AmbiguousTarget, ChoiceNote::UnplacedSectionLines}};
            } else if (std::any_of(replaced.begin(), replaced.end(),
                                   [&](const auto &range) { return sharesPlace(range, *place); })) {
                return {{Transition::AmbiguousTarget, ChoiceNote::ReplacesUnselected}};
            }
        }
    }
    return std::nullopt;
}

// The edits that put both scopes' text in place of their ranges.
std::vector<ResultEdit> combinationEdits(const Selection &selected, const Competitor &other, bool split,
                                         const std::string &replacement)
{
    const auto &snapshots = selected.snapshots;
    const auto mine = selected.range;
    const auto theirs = other.range;
    // Joining the two scopes of a split block would move text across the
    // shared lines and the other sections. Each scope keeps its own side's
    // text instead, and the decisions' order puts them together when the
    // block is rebuilt once every section is decided.
    if (split)
        return {{mine, sourceText(snapshots, selected.target.source)},
                {theirs, sourceText(snapshots, other.target->source)}};
    if (mine == theirs || mine.begin == mine.end || theirs.begin == theirs.end)
        return {{mine.begin == mine.end ? theirs : mine, replacement}};
    // Already-present contributions can occupy adjacent ranges. Replace their
    // exact union atomically, without duplicating either old range.
    const bool mineFirst = mine.begin < theirs.begin;
    return {{mineFirst ? mine : theirs, replacement}, {mineFirst ? theirs : mine, {}}};
}

ChoicePlan previewCombination(ChoicePlan preview, const Selection &selected, SourceSide firstSide)
{
    // The two sides of a marker section sit apart, across its separator line.
    // In a block Git wrote around lines both sides share, the other sections'
    // scopes and those shared lines sit between them as well.
    const auto *block = selected.target.scope == ChoiceScope::Section ? selected.block() : nullptr;
    const bool marked = block != nullptr;
    const bool split = marked && block->sections.size() > 1;
    const auto other = competitor(selected, marked);
    if (other.matches > 1)
        return failed(std::move(preview), Transition::AmbiguousTarget, ChoiceNote::SeveralCompetitors);
    if (!other.target) return failed(std::move(preview), Transition::AmbiguousTarget, ChoiceNote::NoCompetitor);
    const auto *first = selected.target.source.side == firstSide ? &selected.target : other.target;
    const auto *second = first == &selected.target ? other.target : &selected.target;
    preview.sourceOrder = {first->source, second->source};
    preview.command().expected = preview.revision;
    preview.command().targets = {selected.anchor, *other.anchor};
    std::uint64_t order = 1;
    for (const auto *target : {first, second}) {
        for (const auto &id : target->decisions) {
            if (!selected.draft.decisions.contains(id))
                return failed(std::move(preview), Transition::Blocked, ChoiceNote::NoRecordedChoices);
            preview.command().decisions[id] = {DecisionValue::Accepted, order};
        }
        preview.replacement += sourceText(selected.snapshots, target->source);
        ++order;
    }
    const auto theirs = other.range;
    const ByteRange affected{std::min(selected.range.begin, theirs.begin), std::max(selected.range.end, theirs.end)};
    // A split block's combination edits only this section's two scopes, so
    // only text inside them could be replaced.
    const auto replaced = split ? std::vector<ByteRange>{selected.range, theirs} : std::vector<ByteRange>{affected};
    if (const auto refusal = unselectedContribution(selected, preview.command(), replaced))
        return failed(std::move(preview), refusal->first, refusal->second);
    preview.affectedRange = affected;
    preview.before = std::string(slice(selected.draft.text, affected));
    preview.replacesManual = selected.anchor.manualOverlap || other.anchor->manualOverlap;
    preview.command().edits = combinationEdits(selected, other, split, preview.replacement);
    // Every combination is inspectable and explicitly confirmed, including a
    // new ordering of a decided scope after subsequent manual editing.
    preview.state = Transition::ConfirmationRequired;
    preview.note = ChoiceNote::Combine;
    if (selected.completesMarkers(preview.command()))
        preview.markers = MarkerEffect::Removed;
    else if (split)
        preview.markers = MarkerEffect::KeptUntilDecided;
    return preview;
}

// The range an accept replaces. A source deletion has an empty insertion
// anchor, so its replacement range comes from the captured competitor, never
// nearby text. In a marker section the competitor's anchor lies inside the
// block: its whole region when the insertion point lies past the end marker,
// or one scope of it between lines both regions share.
const ResultAnchor *replacementAnchor(const Selection &selected)
{
    if (selected.target.source.range.begin != selected.target.source.range.end) return &selected.anchor;
    const auto *opposite = selected.choices.opposite(selected.target);
    const auto *anchor = opposite ? selected.mapping.anchor(opposite->source.contributionId) : nullptr;
    const auto range = mappedRange(anchor);
    if (!range) return nullptr;
    const auto at = selected.range.begin;
    if (at != range->begin && at != range->end && !selected.marked()) return nullptr;
    return anchor;
}

// Where an accepted line of the other side now lies. A section choice unmaps
// the line anchors inside it, so a line of another section falls back to its
// own side's section anchor.
const ResultAnchor *acceptedPlace(const Selection &selected, const ChoiceTarget &accepted)
{
    const auto *place = selected.mapping.anchor(accepted.source.contributionId);
    if ((place && place->result) || accepted.section == selected.target.section) return place;
    const auto *section = selected.choices.section(accepted.section, accepted.source.side);
    place = section ? selected.mapping.anchor(section->source.contributionId) : nullptr;
    return place && place->state == AnchorState::Mapped ? place : nullptr;
}

// An accept replaces its scope's range with the captured text, and cannot
// silently replace a contribution of the other side accepted before.
Refusal planReplacement(const Selection &selected, ChoicePlan &preview)
{
    const auto *replaced = replacementAnchor(selected);
    if (!replaced || !replaced->result) return {{Transition::AmbiguousTarget, ChoiceNote::UncertainDeletion}};
    if (replaced != &selected.anchor) preview.command().targets.push_back(*replaced);
    const auto range = *replaced->result;
    preview.before = std::string(slice(selected.draft.text, range));
    preview.affectedRange = range;
    preview.replacement = sourceText(selected.snapshots, selected.target.source);
    preview.command().edits.push_back({range, preview.replacement});
    for (const auto &target : selected.choices.targets()) {
        if (target.scope != ChoiceScope::Line || target.source.side == selected.target.source.side
            || decision(selected.draft, target.source.contributionId).value != DecisionValue::Accepted)
            continue;
        const auto *other = acceptedPlace(selected, target);
        if (!other || !other->result || sharesPlace(range, *other->result))
            return {{Transition::Blocked, ChoiceNote::AcceptedOverlap}};
    }
    return std::nullopt;
}

// Resolving with a side also ignores the competing contributions. A line
// resolves only against exactly matching competing lines.
Refusal planCompetitors(const Selection &selected, ChoicePlan &preview)
{
    const auto *opposite = selected.choices.opposite(selected.target);
    if (!opposite) return {{Transition::InvalidAnchor, ChoiceNote::MissingCompetitor}};
    const bool line = selected.target.scope == ChoiceScope::Line;
    for (const auto &competing : opposite->decisions) {
        const auto *other = selected.mapping.anchor(competing);
        if (line) {
            const auto place = mappedRange(other);
            if (!place) return {{Transition::AmbiguousTarget, ChoiceNote::UncertainCompetingLine}};
            if (!sharesPlace(selected.range, *place)) continue;
            if (*place != selected.range) return {{Transition::AmbiguousTarget, ChoiceNote::NoExactCompetingLine}};
        }
        if (decision(selected.draft, competing).value == DecisionValue::Accepted)
            return {{Transition::Blocked, ChoiceNote::CompetingAccepted}};
        preview.command().decisions[competing] = {DecisionValue::Ignored, {}};
    }
    // A multi-line competing section cannot be silently rejected by a line
    // whose correspondence identifies no competing contribution.
    if (line && preview.command().decisions.size() == selected.target.decisions.size())
        return {{Transition::AmbiguousTarget, ChoiceNote::NoCompetingLine}};
    return std::nullopt;
}

// Resolving with a side accepts it and ignores its competitors.
Refusal planResolution(const Selection &selected, ChoicePlan &preview)
{
    if (auto refusal = planReplacement(selected, preview)) return refusal;
    return planCompetitors(selected, preview);
}

// What each action is: a combination, which orders both sides' scopes, or a
// single-side action that decides its scope one way and plans the rest. Each
// action has one row, so a new action is one row and its planner.
struct ActionRule {
    ChoiceAction action;
    ChoiceNote note;
    // Single-side actions: the decision and the planner, if any.
    DecisionValue decision = DecisionValue::Accepted;
    Refusal (*plan)(const Selection &, ChoicePlan &) = nullptr;
    // Combinations: the side whose scope comes first.
    std::optional<SourceSide> first = std::nullopt;
};

constexpr std::array kActionRules{
    ActionRule{ChoiceAction::Ignore, ChoiceNote::Ignore, DecisionValue::Ignored},
    ActionRule{ChoiceAction::Accept, ChoiceNote::Accept, DecisionValue::Accepted, planReplacement},
    ActionRule{ChoiceAction::ResolveUsingSide, ChoiceNote::ResolveUsingSide, DecisionValue::Accepted, planResolution},
    ActionRule{ChoiceAction::CombineLeftFirst, ChoiceNote::Combine, DecisionValue::Accepted, nullptr,
               SourceSide::Stage2},
    ActionRule{ChoiceAction::CombineRightFirst, ChoiceNote::Combine, DecisionValue::Accepted, nullptr,
               SourceSide::Stage3}};

const ActionRule *ruleOf(ChoiceAction action) noexcept
{
    const auto found =
        std::find_if(kActionRules.begin(), kActionRules.end(), [&](const auto &rule) { return rule.action == action; });
    return found == kActionRules.end() ? nullptr : &*found;
}

ChoicePlan previewSingleSide(ChoicePlan preview, const Selection &selected, const ActionRule &rule)
{
    for (const auto &id : selected.target.decisions) {
        if (!selected.draft.decisions.contains(id))
            return failed(std::move(preview), Transition::Blocked, ChoiceNote::NoRecordedChoices);
        if (decision(selected.draft, id).value != DecisionValue::Undecided)
            return failed(std::move(preview), Transition::Blocked, ChoiceNote::AlreadyDecided);
    }
    const bool ignore = rule.decision == DecisionValue::Ignored;
    preview.command().expected = preview.revision;
    preview.command().targets.push_back(selected.anchor);
    for (const auto &id : selected.target.decisions) preview.command().decisions[id] = {rule.decision, {}};
    if (rule.plan)
        if (auto refusal = rule.plan(selected, preview))
            return failed(std::move(preview), refusal->first, refusal->second);
    const bool confirmation = !ignore
        && std::any_of(preview.command().targets.begin(), preview.command().targets.end(),
                       [](const auto &value) { return value.manualOverlap; });
    preview.replacesManual = confirmation;
    preview.state = confirmation ? Transition::ConfirmationRequired : Transition::Applied;
    preview.note = rule.note;
    if (selected.completesMarkers(preview.command())) preview.markers = MarkerEffect::Removed;
    return preview;
}
}

static ChoicePlan planChoice(const FileSession &file, RevisionId expected, const std::string &target,
                             ChoiceAction action)
{
    ChoicePlan preview;
    preview.revision = expected;
    preview.action = action;
    preview.target = target;
    if (file.current().id != expected)
        return failed(std::move(preview), Transition::StaleRevision, ChoiceNote::StaleRevision);
    if (file.support() != Support::Supported || file.pendingAlignment())
        return failed(std::move(preview), Transition::Blocked, ChoiceNote::FileNotEditable);
    const auto *found = file.catalogue().find(target);
    const auto *mapping = file.correspondence();
    if (!found || !mapping || !file.snapshots())
        return failed(std::move(preview), Transition::InvalidAnchor, ChoiceNote::UnknownScope);
    const auto *anchor = mapping->anchor(target);
    const auto range = mappedRange(anchor);
    if (!range)
        return failed(std::move(preview),
                      anchor && anchor->state == AnchorState::Ambiguous ? Transition::AmbiguousTarget
                                                                        : Transition::InvalidAnchor,
                      ChoiceNote::UnmappedScope);
    const auto *rule = ruleOf(action);
    if (!rule) return failed(std::move(preview), Transition::InvalidAnchor, ChoiceNote::UnknownScope);
    const Selection selected{*file.snapshots(), *mapping, file.catalogue(), file.current().draft, *found,
                             *anchor,           *range,   file.planner(),   file.markerSections()};
    if (rule->first) return previewCombination(std::move(preview), selected, *rule->first);
    return previewSingleSide(std::move(preview), selected, *rule);
}

namespace {
// A batch change, with what applying it touches.
struct Candidate {
    NonConflictingChange change;
    ResultAnchor anchor;
    ByteRange earlier;
    std::vector<std::string> decisions;
    bool unavailable = false;
};

bool inScope(const NonConflictingTarget &target, NonConflictingScope scope)
{
    return scope == NonConflictingScope::Both
        || target.source.side == (scope == NonConflictingScope::Left ? SourceSide::Stage2 : SourceSide::Stage3);
}

// Reads the session state a non-conflicting batch depends on.
class BatchPlanner {
public:
    explicit BatchPlanner(const FileSession &file)
        : mapping_(*file.correspondence()), snapshots_(*file.snapshots()), draft_(file.current().draft)
    {}

    // The change a target makes, or nothing when it must be skipped.
    [[nodiscard]] std::optional<Candidate> candidate(const NonConflictingTarget &target) const
    {
        if (std::any_of(target.decisions.begin(), target.decisions.end(), [&](const auto &id) {
                const auto found = draft_.decisions.find(id);
                return found == draft_.decisions.end() || found->second.value != DecisionValue::Undecided;
            }))
            return std::nullopt;
        const auto *anchor = mapping_.anchor(target.earlier.contributionId);
        if (!mappedRange(anchor)) return std::nullopt;
        const auto replacement = sourceText(snapshots_, target.source);
        anchor = present(target, *anchor, replacement);
        if (!anchor->result) return std::nullopt;
        const auto range = *anchor->result;
        const auto before = std::string(slice(draft_.text, range));
        const auto base = sourceText(snapshots_, target.earlier);
        if (before != replacement && (anchor->manualOverlap || before != base)) return std::nullopt;
        if (overlapsAccepted(target, range)) return std::nullopt;
        return Candidate{
            {{target.source}, range, before, replacement}, *anchor, target.earlier.range, target.decisions};
    }

private:
    // An insertion already present in the result lies beside its empty
    // earlier anchor. Its own registered source range identifies it exactly.
    [[nodiscard]] const ResultAnchor *present(const NonConflictingTarget &target, const ResultAnchor &earlier,
                                              const std::string &replacement) const
    {
        if (target.earlier.range.begin != target.earlier.range.end) return &earlier;
        const auto *inserted = mapping_.anchor(target.source.contributionId);
        const auto range = mappedRange(inserted);
        if (!earlier.result || !range) return &earlier;
        const auto at = earlier.result->begin;
        if ((at == range->begin || at == range->end) && slice(draft_.text, *range) == replacement) return inserted;
        return &earlier;
    }

    // Never overwrite another accepted contribution, even if it happens to
    // contain the earlier bytes. Other decisions are preserved verbatim.
    [[nodiscard]] bool overlapsAccepted(const NonConflictingTarget &target, ByteRange range) const
    {
        return std::any_of(draft_.decisions.begin(), draft_.decisions.end(), [&](const auto &entry) {
            if (entry.second.value != DecisionValue::Accepted
                || std::find(target.decisions.begin(), target.decisions.end(), entry.first) != target.decisions.end())
                return false;
            const auto *accepted = mapping_.anchor(entry.first);
            const auto place = mappedRange(accepted);
            return !place || sharesPlace(range, *place);
        });
    }

    const CorrespondenceMap &mapping_;
    const FileSnapshots &snapshots_;
    const Draft &draft_;
};

// Independent source ranges can collapse onto the same result passage.
// Reject both unless they describe exactly the same contribution.
void markCollisions(std::vector<Candidate> &candidates)
{
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        for (std::size_t j = i + 1; j < candidates.size(); ++j) {
            const auto &a = candidates[i].change;
            const auto &b = candidates[j].change;
            const bool collides = sharesPlace(a.result, b.result)
                || (a.before != a.replacement && b.before != b.replacement && a.result.begin == b.result.begin);
            if (collides
                && (a.result != b.result || a.replacement != b.replacement
                    || candidates[i].earlier != candidates[j].earlier)) {
                candidates[i].unavailable = true;
                candidates[j].unavailable = true;
            }
        }
    }
}

// Identical changes from both sides appear once in a combined batch.
void addCandidate(BatchPlan &preview, Candidate candidate)
{
    const auto duplicate = std::find_if(preview.changes.begin(), preview.changes.end(), [&](const auto &change) {
        return change.result == candidate.change.result && change.replacement == candidate.change.replacement;
    });
    if (duplicate != preview.changes.end()) {
        duplicate->sources.push_back(candidate.change.sources.front());
    } else {
        if (candidate.change.before != candidate.change.replacement)
            preview.command().edits.push_back({candidate.change.result, candidate.change.replacement});
        preview.changes.push_back(std::move(candidate.change));
        preview.command().targets.push_back(std::move(candidate.anchor));
    }
    for (const auto &id : candidate.decisions) preview.command().decisions[id] = {DecisionValue::Accepted, {}};
}
}

static BatchPlan planNonConflicting(const FileSession &file, RevisionId expected, NonConflictingScope scope)
{
    BatchPlan preview;
    preview.revision = expected;
    preview.scope = scope;
    preview.path = file.path();
    preview.command().expected = expected;
    if (expected != file.current().id) {
        preview.state = Transition::StaleRevision;
        preview.note = BatchNote::StaleRevision;
        return preview;
    }
    if (file.support() != Support::Supported || file.pendingAlignment() || !file.correspondence()
        || !file.snapshots()) {
        preview.note = BatchNote::FileNotEditable;
        return preview;
    }
    if (!file.snapshots()->earlier) {
        preview.note = BatchNote::NoEarlierVersion;
        return preview;
    }
    const BatchPlanner planner(file);
    std::vector<Candidate> candidates;
    for (const auto &target : file.nonConflictingTargets()) {
        if (!inScope(target, scope)) continue;
        auto candidate = planner.candidate(target);
        if (!candidate) {
            ++preview.skipped;
            continue;
        }
        if (candidate->change.before == candidate->change.replacement && target.decisions.empty()) continue;
        candidates.push_back(std::move(*candidate));
    }
    markCollisions(candidates);
    for (auto &candidate : candidates) {
        if (candidate.unavailable)
            ++preview.skipped;
        else
            addCandidate(preview, std::move(candidate));
    }
    preview.state = preview.changes.empty() ? Transition::NoChange : Transition::ConfirmationRequired;
    preview.note = BatchNote::Batch;
    return preview;
}

static WholeFilePlan planWholeFile(const FileSession &file, RevisionId expected, SourceSide side)
{
    WholeFilePlan preview;
    preview.revision = expected;
    preview.fileId = file.id();
    preview.side = side;
    if (expected != file.current().id) {
        preview.state = Transition::StaleRevision;
        preview.note = WholeFileNote::StaleRevision;
        return preview;
    }
    if (file.support() != Support::Supported || file.pendingAlignment() || !file.snapshots()
        || !file.correspondence()) {
        preview.note = WholeFileNote::NoCapturedSource;
        return preview;
    }
    if (side != SourceSide::Stage2 && side != SourceSide::Stage3) {
        preview.state = Transition::InvalidAnchor;
        preview.note = WholeFileNote::InvalidSide;
        return preview;
    }
    const auto &captured = *file.snapshots();
    const auto &source = captured.source(side);
    if (!source) {
        preview.note = WholeFileNote::SourceUnavailable;
        return preview;
    }
    const auto &draft = file.current().draft;
    const auto &mapping = *file.correspondence();
    preview.origin = {captured.provenance.sessionId, file.path(), source->objectId};
    preview.before = draft.text;
    preview.replacement = source->worktree.utf8;
    preview.command().expected = expected;
    preview.command().scope = EditingScope::WholeFile;
    preview.command().edits.push_back({{0, preview.before.size()}, preview.replacement});
    for (const auto &target : file.choices()) {
        if (target.scope != ChoiceScope::Section) continue;
        const Decision after{target.source.side == side ? DecisionValue::Accepted : DecisionValue::Ignored, {}};
        for (const auto &id : target.decisions) {
            const auto found = draft.decisions.find(id);
            const auto *anchor = mapping.anchor(id);
            // Earlier recovered drafts keep their exact decision vocabulary.
            if (found == draft.decisions.end() || preview.command().decisions.contains(id) || !anchor) continue;
            preview.contributions.push_back({anchor->source, found->second, after});
            preview.command().decisions.emplace(id, after);
        }
    }
    preview.retainedDecisions = draft.decisions.size() - preview.command().decisions.size();
    if (const auto *passages = mapping.passages(side))
        for (const auto &passage : *passages)
            if (passage.kind != PassageKind::Equal) preview.differences.push_back(passage);
    preview.state = Transition::ConfirmationRequired;
    preview.note = WholeFileNote::Replace;
    return preview;
}

ChoicePreview previewChoice(const FileSession &file, RevisionId expected, const std::string &target,
                            ChoiceAction action)
{
    return planChoice(file, expected, target, action);
}

NonConflictingPreview previewNonConflicting(const FileSession &file, RevisionId expected, NonConflictingScope scope)
{
    return planNonConflicting(file, expected, scope);
}

WholeFilePreview previewWholeFile(const FileSession &file, RevisionId expected, SourceSide side)
{
    return planWholeFile(file, expected, side);
}

namespace {
template <typename Preview, typename Planner>
Transition applyPreview(FileSession &file, const Preview &preview, bool confirm, Planner planner)
{
    if (preview.revision != file.current().id) return Transition::StaleRevision;
    const auto current = planner();
    if (static_cast<const Preview &>(current) != preview) return Transition::PreviewChanged;
    if (current.state != Transition::Applied && current.state != Transition::ConfirmationRequired) return current.state;
    if (current.state == Transition::ConfirmationRequired && !confirm) return Transition::ConfirmationRequired;
    return file.applyCommand(current.command(), confirm);
}
}

Transition applyChoice(FileSession &file, const ChoicePreview &preview, bool confirmReplacement)
{
    return applyPreview(file, preview, confirmReplacement,
                        [&] { return planChoice(file, preview.revision, preview.target, preview.action); });
}

Transition applyNonConflicting(FileSession &file, const NonConflictingPreview &preview, bool confirm)
{
    return applyPreview(file, preview, confirm,
                        [&] { return planNonConflicting(file, preview.revision, preview.scope); });
}

Transition applyWholeFile(FileSession &file, const WholeFilePreview &preview, bool confirm)
{
    return applyPreview(file, preview, confirm, [&] { return planWholeFile(file, preview.revision, preview.side); });
}
}
