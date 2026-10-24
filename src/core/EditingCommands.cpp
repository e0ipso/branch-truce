#include "EditingCommands.h"

#include <algorithm>

namespace branch_truce {
namespace {
std::string rangeId(SourceSide side, ByteRange range)
{
    return std::to_string(static_cast<int>(side)) + "/" + std::to_string(range.begin)
        + "/" + std::to_string(range.end);
}

std::vector<SourceAnchor> lines(SourceSide side, std::string_view text, ByteRange range)
{
    std::vector<SourceAnchor> result;
    auto begin = range.begin;
    do {
        const auto newline = text.find('\n', begin);
        const auto end = newline == std::string_view::npos ? range.end : std::min(range.end, newline + 1);
        const ByteRange line{begin, end};
        result.push_back({"pick/" + rangeId(side, line), side, line});
        begin = end;
    } while (begin < range.end);
    return result;
}

Decision decision(const FileSession &file, const std::string &id)
{
    const auto found = file.current().draft.decisions.find(id);
    return found == file.current().draft.decisions.end() ? Decision{} : found->second;
}

bool overlaps(ByteRange a, ByteRange b)
{
    if (a == b) return true;
    return a.begin < b.end && b.begin < a.end;
}
}

std::vector<ChoiceTarget> choiceTargets(const FileSnapshots &snapshots)
{
    if (!snapshots.stage2 || !snapshots.stage3) return {};
    const auto &left = snapshots.stage2->worktree.utf8;
    const auto &right = snapshots.stage3->worktree.utf8;
    std::vector<ChoiceTarget> result;
    for (const auto &passage : ExactLineCorrespondence{}.compare(left, right)) {
        if (passage.kind == PassageKind::Equal) continue;
        const auto section = "region/" + rangeId(SourceSide::Stage2, passage.source)
            + "/" + rangeId(SourceSide::Stage3, passage.result);
        const auto a = lines(SourceSide::Stage2, left, passage.source);
        const auto b = lines(SourceSide::Stage3, right, passage.result);
        const auto add = [&](SourceSide side, ByteRange range, const auto &own, const auto &other) {
            std::vector<std::string> ids, competitors;
            for (const auto &line : own) ids.push_back(line.contributionId);
            for (const auto &line : other) competitors.push_back(line.contributionId);
            result.push_back({{"section/" + rangeId(side, range), side, range},
                ChoiceScope::Section, section, ids, competitors});
            for (const auto &line : own)
                if (line.range.begin != line.range.end)
                    result.push_back({line, ChoiceScope::Line, section, {line.contributionId}, {}});
        };
        add(SourceSide::Stage2, passage.source, a, b);
        add(SourceSide::Stage3, passage.result, b, a);
    }
    return result;
}

std::map<std::string, Decision> initialChoiceDecisions(const FileSnapshots &snapshots)
{
    std::map<std::string, Decision> result;
    for (const auto &target : choiceTargets(snapshots))
        for (const auto &id : target.decisions) result.emplace(id, Decision{});
    return result;
}

ChoicePreview previewChoice(const FileSession &file, RevisionId expected,
                            const std::string &id, ChoiceAction action)
{
    ChoicePreview preview;
    preview.revision = expected;
    preview.action = action;
    preview.target = id;
    const auto fail = [&](Transition state, std::string detail) {
        preview.state = state;
        preview.detail = std::move(detail);
        return preview;
    };
    if (file.current().id != expected) return fail(Transition::StaleRevision, "The result changed. Select the scope again.");
    if (file.support() != Support::Supported) return fail(Transition::Blocked, "This file cannot be edited in Branch Truce.");
    const auto &targets = file.choices();
    const auto found = std::find_if(targets.begin(), targets.end(), [&](const auto &target) {
        return target.source.contributionId == id;
    });
    if (found == targets.end() || !file.correspondence()) return fail(Transition::InvalidAnchor, "Unknown source scope.");
    const auto *anchor = file.correspondence()->anchor(id);
    if (!anchor || anchor->state != AnchorState::Mapped || !anchor->result)
        return fail(anchor && anchor->state == AnchorState::Ambiguous ? Transition::AmbiguousTarget : Transition::InvalidAnchor,
                    "This scope cannot be mapped safely. Edit the result directly.");
    for (const auto &selected : found->decisions) {
        if (!file.current().draft.decisions.contains(selected))
            return fail(Transition::Blocked, "This draft has no recorded scoped choices. Edit the result directly.");
        if (decision(file, selected).value != DecisionValue::Undecided)
            return fail(Transition::Blocked, "This scope already has a decision. Undo intervening actions to change it.");
    }
    preview.command.expected = expected;
    preview.command.targets.push_back(*anchor);
    for (const auto &selected : found->decisions)
        preview.command.decisions[selected] = {action == ChoiceAction::Ignore ? DecisionValue::Ignored : DecisionValue::Accepted, {}};
    if (action != ChoiceAction::Ignore) {
        // A source deletion has an empty insertion anchor. Its replacement
        // range must come from the captured competitor, never nearby text.
        const ResultAnchor *replacementAnchor = anchor;
        if (found->source.range.begin == found->source.range.end) {
            const auto opposite = std::find_if(targets.begin(), targets.end(), [&](const auto &target) {
                return target.scope == ChoiceScope::Section && target.section == found->section
                    && target.source.side != found->source.side;
            });
            replacementAnchor = opposite == targets.end() ? nullptr
                : file.correspondence()->anchor(opposite->source.contributionId);
            if (!replacementAnchor || replacementAnchor->state != AnchorState::Mapped || !replacementAnchor->result
                || (anchor->result->begin != replacementAnchor->result->begin
                    && anchor->result->begin != replacementAnchor->result->end))
                return fail(Transition::AmbiguousTarget, "The deletion scope is uncertain. Edit the result directly.");
            preview.command.targets.push_back(*replacementAnchor);
        }
        const auto *snapshots = file.snapshots();
        const auto &source = found->source.side == SourceSide::Stage2 ? snapshots->stage2 : snapshots->stage3;
        preview.before = file.current().draft.text.substr(replacementAnchor->result->begin,
            replacementAnchor->result->end - replacementAnchor->result->begin);
        preview.replacement = source->worktree.utf8.substr(found->source.range.begin,
            found->source.range.end - found->source.range.begin);
        preview.command.edits.push_back({*replacementAnchor->result, preview.replacement});
        // An accept cannot silently replace any previously accepted contribution.
        for (const auto &target : targets) {
            if (target.scope != ChoiceScope::Line || target.source.side == found->source.side
                || decision(file, target.source.contributionId).value != DecisionValue::Accepted) continue;
            const auto *other = file.correspondence()->anchor(target.source.contributionId);
            if (!other || !other->result || overlaps(*replacementAnchor->result, *other->result))
                return fail(Transition::Blocked, "Accepted text may overlap this choice. Undo its acceptance before replacing this scope.");
        }
    }
    if (action == ChoiceAction::ResolveUsingSide) {
        const auto opposite = std::find_if(targets.begin(), targets.end(), [&](const auto &target) {
            return target.scope == ChoiceScope::Section && target.section == found->section
                && target.source.side != found->source.side;
        });
        if (opposite == targets.end()) return fail(Transition::InvalidAnchor, "Missing competing source scope.");
        for (const auto &competitor : opposite->decisions) {
            const auto *other = file.correspondence()->anchor(competitor);
            if (found->scope == ChoiceScope::Line) {
                if (!other || !other->result || other->state != AnchorState::Mapped)
                    return fail(Transition::AmbiguousTarget, "The competing line scope is uncertain. Use a section or edit manually.");
                if (!overlaps(*anchor->result, *other->result)) continue;
                if (*other->result != *anchor->result)
                    return fail(Transition::AmbiguousTarget, "This line has no exact competing scope. Use a section or edit manually.");
            }
            if (decision(file, competitor).value == DecisionValue::Accepted)
                return fail(Transition::Blocked, "Undo the competing acceptance before resolving this scope.");
            preview.command.decisions[competitor] = {DecisionValue::Ignored, {}};
        }
        // A multi-line competing section cannot be silently rejected by a line
        // whose correspondence identifies no competing contribution.
        if (found->scope == ChoiceScope::Line && preview.command.decisions.size() == found->decisions.size())
            return fail(Transition::AmbiguousTarget, "No competing line can be identified safely. Use a section or edit manually.");
    }
    const bool confirmation = action != ChoiceAction::Ignore && std::any_of(
        preview.command.targets.begin(), preview.command.targets.end(), [](const auto &value) { return value.manualOverlap; });
    preview.state = confirmation ? Transition::ConfirmationRequired : Transition::Applied;
    preview.detail = action == ChoiceAction::Ignore ? "Ignore preserves result text and leaves the other side undecided."
        : action == ChoiceAction::Accept ? "Accept changes only this scope and leaves the other side undecided."
        : "Resolve using this side accepts this scope and ignores its competing contributions.";
    return preview;
}

Transition applyChoice(FileSession &file, const ChoicePreview &preview, bool confirmReplacement)
{
    if (preview.revision != file.current().id) return Transition::StaleRevision;
    const auto current = previewChoice(file, preview.revision, preview.target, preview.action);
    if (current != preview) return Transition::InvalidAnchor;
    if (current.state != Transition::Applied && current.state != Transition::ConfirmationRequired) return current.state;
    if (current.state == Transition::ConfirmationRequired && !confirmReplacement) return Transition::ConfirmationRequired;
    return file.applyCommand(current.command, confirmReplacement);
}
}
