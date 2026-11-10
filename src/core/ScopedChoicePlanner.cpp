// Plans individual source choices, competing contributions and marker effects.
#include "internal/EditingPlans.h"
#include <algorithm>
#include <array>
#include <set>

namespace branch_truce::detail {
namespace {
bool adjoining(ByteRange a, ByteRange b) { return a == b || a.end == b.begin || b.end == a.begin; }

Decision decision(const Draft &draft, const std::string &id)
{
    const auto found = draft.decisions.find(id);
    return found == draft.decisions.end() ? Decision{} : found->second;
}

// One mapped selection and its marker sections.
struct Selection {
    const PlanningInput &input;
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

    // True when the command decides the last open contribution of a marker
    // section, so applying it also removes that block's marker lines.
    [[nodiscard]] bool completesMarkers(const EditingCommand &command) const
    {
        if (command.scope != EditingScope::Mapped || sections.empty()) return false;
        auto decided = input.draft;
        for (const auto &[id, value] : command.decisions) decided.decisions[id] = value;
        return !planner.edits(decided, *input.mapping, command.decisions, sections).empty();
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
    for (const auto &target : selected.input.choices.targets()) {
        if (target.section != selected.target.section || target.scope != selected.target.scope
            || target.source.side == selected.target.source.side)
            continue;
        const auto *candidate = selected.input.mapping->anchor(target.source.contributionId);
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
    for (const auto &target : selected.input.choices.targets()) {
        for (const auto &id : target.decisions) {
            if (command.decisions.contains(id) || !inspected.insert(id).second) continue;
            const auto *unselected = selected.input.mapping->anchor(id);
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

std::vector<ResultEdit> combinationEdits(const Selection &selected, const Competitor &other, bool split,
                                         const std::string &replacement)
{
    const auto &snapshots = *selected.input.snapshots;
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
        return refused(std::move(preview), Transition::AmbiguousTarget, ChoiceNote::SeveralCompetitors);
    if (!other.target) return refused(std::move(preview), Transition::AmbiguousTarget, ChoiceNote::NoCompetitor);
    const auto *first = selected.target.source.side == firstSide ? &selected.target : other.target;
    const auto *second = first == &selected.target ? other.target : &selected.target;
    preview.sourceOrder = {first->source, second->source};
    preview.command().expected = preview.revision;
    preview.command().targets = {selected.anchor, *other.anchor};
    std::uint64_t order = 1;
    for (const auto *target : {first, second}) {
        for (const auto &id : target->decisions) {
            if (!selected.input.draft.decisions.contains(id))
                return refused(std::move(preview), Transition::Blocked, ChoiceNote::NoRecordedChoices);
            preview.command().decisions[id] = {DecisionValue::Accepted, order};
        }
        preview.replacement += sourceText(*selected.input.snapshots, target->source);
        ++order;
    }
    const auto theirs = other.range;
    const ByteRange affected{std::min(selected.range.begin, theirs.begin), std::max(selected.range.end, theirs.end)};
    // A split block's combination edits only this section's two scopes, so
    // only text inside them could be replaced.
    const auto replaced = split ? std::vector<ByteRange>{selected.range, theirs} : std::vector<ByteRange>{affected};
    if (const auto refusal = unselectedContribution(selected, preview.command(), replaced))
        return refused(std::move(preview), refusal->first, refusal->second);
    preview.affectedRange = affected;
    preview.before = std::string(slice(selected.input.draft.text, affected));
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
    const auto *opposite = selected.input.choices.opposite(selected.target);
    const auto *anchor = opposite ? selected.input.mapping->anchor(opposite->source.contributionId) : nullptr;
    const auto range = mappedRange(anchor);
    if (!range) return nullptr;
    const auto at = selected.range.begin;
    if (at != range->begin && at != range->end && !selected.block()) return nullptr;
    return anchor;
}

// Where an accepted line of the other side now lies. A section choice unmaps
// the line anchors inside it, so a line of another section falls back to its
// own side's section anchor.
const ResultAnchor *acceptedPlace(const Selection &selected, const ChoiceTarget &accepted)
{
    const auto *place = selected.input.mapping->anchor(accepted.source.contributionId);
    if ((place && place->result) || accepted.section == selected.target.section) return place;
    const auto *section = selected.input.choices.section(accepted.section, accepted.source.side);
    place = section ? selected.input.mapping->anchor(section->source.contributionId) : nullptr;
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
    preview.before = std::string(slice(selected.input.draft.text, range));
    preview.affectedRange = range;
    preview.replacement = sourceText(*selected.input.snapshots, selected.target.source);
    preview.command().edits.push_back({range, preview.replacement});
    for (const auto &target : selected.input.choices.targets()) {
        if (target.scope != ChoiceScope::Line || target.source.side == selected.target.source.side
            || decision(selected.input.draft, target.source.contributionId).value != DecisionValue::Accepted)
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
    const auto *opposite = selected.input.choices.opposite(selected.target);
    if (!opposite) return {{Transition::InvalidAnchor, ChoiceNote::MissingCompetitor}};
    const bool line = selected.target.scope == ChoiceScope::Line;
    for (const auto &competing : opposite->decisions) {
        const auto *other = selected.input.mapping->anchor(competing);
        if (line) {
            const auto place = mappedRange(other);
            if (!place) return {{Transition::AmbiguousTarget, ChoiceNote::UncertainCompetingLine}};
            if (!sharesPlace(selected.range, *place)) continue;
            if (*place != selected.range) return {{Transition::AmbiguousTarget, ChoiceNote::NoExactCompetingLine}};
        }
        if (decision(selected.input.draft, competing).value == DecisionValue::Accepted)
            return {{Transition::Blocked, ChoiceNote::CompetingAccepted}};
        preview.command().decisions[competing] = {DecisionValue::Ignored, {}};
    }
    // A multi-line competing section cannot be silently rejected by a line
    // whose correspondence identifies no competing contribution.
    if (line && preview.command().decisions.size() == selected.target.decisions.size())
        return {{Transition::AmbiguousTarget, ChoiceNote::NoCompetingLine}};
    return std::nullopt;
}

Refusal planResolution(const Selection &selected, ChoicePlan &preview)
{
    if (auto refusal = planReplacement(selected, preview)) return refusal;
    return planCompetitors(selected, preview);
}

struct ActionRule {
    ChoiceAction action;
    ChoiceNote note;
    DecisionValue decision = DecisionValue::Accepted;
    Refusal (*plan)(const Selection &, ChoicePlan &) = nullptr;
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

ChoicePlan previewSingleSide(ChoicePlan preview, const Selection &selected, const ActionRule &rule)
{
    for (const auto &id : selected.target.decisions) {
        if (!selected.input.draft.decisions.contains(id))
            return refused(std::move(preview), Transition::Blocked, ChoiceNote::NoRecordedChoices);
        if (decision(selected.input.draft, id).value != DecisionValue::Undecided)
            return refused(std::move(preview), Transition::Blocked, ChoiceNote::AlreadyDecided);
    }
    const bool ignore = rule.decision == DecisionValue::Ignored;
    preview.command().expected = preview.revision;
    preview.command().targets.push_back(selected.anchor);
    for (const auto &id : selected.target.decisions) preview.command().decisions[id] = {rule.decision, {}};
    if (const auto refusal = rule.plan ? rule.plan(selected, preview) : std::nullopt)
        return refused(std::move(preview), refusal->first, refusal->second);
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

ChoicePlan planChoice(const PlanningInput &input, MarkerPlanner planner, const MarkerScan &markers, RevisionId expected,
                      const std::string &target, ChoiceAction action)
{
    ChoicePlan preview;
    preview.revision = expected;
    preview.action = action;
    preview.target = target;
    if (input.revision != expected)
        return refused(std::move(preview), Transition::StaleRevision, ChoiceNote::StaleRevision);
    if (!input.editable) return refused(std::move(preview), Transition::Blocked, ChoiceNote::FileNotEditable);
    const auto *found = input.choices.find(target);
    const auto *mapping = input.mapping;
    if (!found || !mapping || !input.snapshots)
        return refused(std::move(preview), Transition::InvalidAnchor, ChoiceNote::UnknownScope);
    const auto *anchor = mapping->anchor(target);
    const auto range = mappedRange(anchor);
    if (!range)
        return refused(std::move(preview),
                       anchor && anchor->state == AnchorState::Ambiguous ? Transition::AmbiguousTarget
                                                                         : Transition::InvalidAnchor,
                       ChoiceNote::UnmappedScope);
    const auto rule =
        std::find_if(kActionRules.begin(), kActionRules.end(), [&](const auto &rule) { return rule.action == action; });
    if (rule == kActionRules.end())
        return refused(std::move(preview), Transition::InvalidAnchor, ChoiceNote::UnknownScope);
    const Selection selected{input,  *found,  *anchor,
                             *range, planner, planner.sections(input.draft.text, *mapping, &markers)};
    if (rule->first) return previewCombination(std::move(preview), selected, *rule->first);
    return previewSingleSide(std::move(preview), selected, *rule);
}

}
