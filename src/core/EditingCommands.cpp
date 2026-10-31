// Source choice commands apply accept, ignore and combined-side decisions to a result revision.
#include "EditingCommands.h"

#include <algorithm>
#include <set>

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

bool adjoining(ByteRange a, ByteRange b)
{
    return a == b || a.end == b.begin || b.end == a.begin;
}

ChoicePreview previewCombination(const FileSession &file, RevisionId expected,
                                 const ChoiceTarget &selected, ChoiceAction action)
{
    ChoicePreview preview;
    preview.revision = expected;
    preview.action = action;
    preview.target = selected.source.contributionId;
    const auto fail = [&](Transition state, std::string detail) {
        preview.state = state;
        preview.detail = std::move(detail);
        return preview;
    };
    const auto *mapping = file.correspondence();
    const auto *anchor = mapping->anchor(preview.target);
    const ChoiceTarget *competitor = nullptr;
    const ResultAnchor *other = nullptr;
    std::size_t matches = 0;
    bool exact = false;
    // The two sides of a marker section sit apart, across its separator line.
    // In a block Git wrote around lines both sides share, the other sections'
    // scopes and those shared lines sit between them as well.
    const auto markers = file.markerSections();
    const auto block = selected.scope != ChoiceScope::Section ? markers.end() : std::find_if(markers.begin(),
        markers.end(), [&](const auto &found) { return found.encloses(selected.section); });
    const bool marked = block != markers.end();
    const bool split = marked && block->sections.size() > 1;
    for (const auto &target : file.choices()) {
        if (target.section != selected.section || target.scope != selected.scope
            || target.source.side == selected.source.side) continue;
        const auto *candidate = mapping->anchor(target.source.contributionId);
        if (!candidate || candidate->state != AnchorState::Mapped || !candidate->result
            || !(marked || adjoining(*anchor->result, *candidate->result))) continue;
        const bool sameRange = *anchor->result == *candidate->result;
        // Neighboring lines are also adjacent. An exact competing range has
        // priority, so an unrelated adjacent line cannot make it ambiguous.
        if (sameRange && !exact) { exact = true; matches = 0; }
        if (!sameRange && exact) continue;
        ++matches;
        competitor = &target;
        other = candidate;
    }
    if (matches > 1)
        return fail(Transition::AmbiguousTarget, "Several competing scopes could match. Use a section or edit the result directly.");
    if (!competitor)
        return fail(Transition::AmbiguousTarget, "The two source scopes cannot be combined safely. Use a section or edit the result directly.");

    const auto *first = &selected;
    const auto *second = competitor;
    const auto firstSide = action == ChoiceAction::CombineLeftFirst ? SourceSide::Stage2 : SourceSide::Stage3;
    if (first->source.side != firstSide) std::swap(first, second);
    preview.sourceOrder = {first->source, second->source};
    preview.command.expected = expected;
    preview.command.targets = {*anchor, *other};
    const auto sourceText = [&](const ChoiceTarget &target) {
        const auto &source = target.source.side == SourceSide::Stage2
            ? file.snapshots()->stage2 : file.snapshots()->stage3;
        return source->worktree.utf8.substr(target.source.range.begin,
            target.source.range.end - target.source.range.begin);
    };
    std::uint64_t order = 1;
    for (const auto *target : {first, second}) {
        for (const auto &id : target->decisions) {
            if (!file.current().draft.decisions.contains(id))
                return fail(Transition::Blocked, "This draft has no recorded scoped choices. Edit the result directly.");
            preview.command.decisions[id] = {DecisionValue::Accepted, order};
        }
        preview.replacement += sourceText(*target);
        ++order;
    }
    const ByteRange affected{std::min(anchor->result->begin, other->result->begin),
                             std::max(anchor->result->end, other->result->end)};
    // A split block's combination edits only this section's two scopes, so
    // only text inside them could be replaced.
    const auto replaced = split ? std::vector<ByteRange>{*anchor->result, *other->result}
                                : std::vector<ByteRange>{affected};
    // A partial combination must not erase another contribution, even if it
    // is still undecided. Unknown line mappings within this section are unsafe.
    std::set<std::string> inspected;
    for (const auto &target : file.choices()) {
        for (const auto &id : target.decisions) {
            if (preview.command.decisions.contains(id) || !inspected.insert(id).second) continue;
            const auto *unselected = mapping->anchor(id);
            if (!unselected || unselected->state != AnchorState::Mapped || !unselected->result) {
                if (target.section == selected.section)
                    return fail(Transition::AmbiguousTarget, "Other lines in this section cannot be located safely. Use a section or edit manually.");
            } else if (std::any_of(replaced.begin(), replaced.end(),
                           [&](const auto &range) { return overlaps(range, *unselected->result); })) {
                return fail(Transition::AmbiguousTarget, "This combination would replace an unselected contribution. Use a section or edit manually.");
            }
        }
    }
    preview.affectedRange = affected;
    preview.before = file.current().draft.text.substr(affected.begin, affected.end - affected.begin);
    preview.replacesManual = anchor->manualOverlap || other->manualOverlap;
    if (split) {
        // Joining the two scopes here would move text across the shared lines
        // and the other sections. Each scope keeps its own side's text instead,
        // and the decisions' order puts them together when the block is rebuilt
        // once every section is decided.
        preview.command.edits = {{*anchor->result, sourceText(selected)}, {*other->result, sourceText(*competitor)}};
    } else if (*anchor->result == *other->result || anchor->result->begin == anchor->result->end
        || other->result->begin == other->result->end) {
        const auto range = anchor->result->begin == anchor->result->end ? *other->result : *anchor->result;
        preview.command.edits.push_back({range, preview.replacement});
    } else {
        // Already-present contributions can occupy adjacent ranges. Replace
        // their exact union atomically, without duplicating either old range.
        const auto earlier = anchor->result->begin < other->result->begin ? *anchor->result : *other->result;
        const auto later = anchor->result->begin < other->result->begin ? *other->result : *anchor->result;
        preview.command.edits = {{earlier, preview.replacement}, {later, {}}};
    }
    // Every combination is inspectable and explicitly confirmed, including a
    // new ordering of a decided scope after subsequent manual editing.
    preview.state = Transition::ConfirmationRequired;
    preview.detail = action == ChoiceAction::CombineLeftFirst ? "Take both: Left then Right. " : "Take both: Right then Left. ";
    preview.detail += "Only the displayed source scopes are combined. Review the exact text for duplication and edit it afterwards if needed.";
    if (file.completesMarkers(preview.command)) preview.detail += " The conflict's marker lines are removed.";
    else if (split) preview.detail += " The conflict keeps its marker lines until its other sections are decided, then holds this text in this order.";
    return preview;
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
    if (action == ChoiceAction::CombineLeftFirst || action == ChoiceAction::CombineRightFirst)
        return previewCombination(file, expected, *found, action);
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
        // In a marker section the competitor's anchor lies inside the block:
        // its whole region when the insertion point lies past the end marker,
        // or one scope of it between lines both regions share.
        const ResultAnchor *replacementAnchor = anchor;
        if (found->source.range.begin == found->source.range.end) {
            const auto opposite = std::find_if(targets.begin(), targets.end(), [&](const auto &target) {
                return target.scope == ChoiceScope::Section && target.section == found->section
                    && target.source.side != found->source.side;
            });
            replacementAnchor = opposite == targets.end() ? nullptr
                : file.correspondence()->anchor(opposite->source.contributionId);
            const auto sections = file.markerSections();
            const bool marked = std::any_of(sections.begin(), sections.end(), [&](const auto &section) {
                return section.encloses(found->section);
            });
            if (!replacementAnchor || replacementAnchor->state != AnchorState::Mapped || !replacementAnchor->result
                || (!marked && anchor->result->begin != replacementAnchor->result->begin
                    && anchor->result->begin != replacementAnchor->result->end))
                return fail(Transition::AmbiguousTarget, "The deletion scope is uncertain. Edit the result directly.");
            preview.command.targets.push_back(*replacementAnchor);
        }
        const auto *snapshots = file.snapshots();
        const auto &source = found->source.side == SourceSide::Stage2 ? snapshots->stage2 : snapshots->stage3;
        preview.before = file.current().draft.text.substr(replacementAnchor->result->begin,
            replacementAnchor->result->end - replacementAnchor->result->begin);
        preview.affectedRange = replacementAnchor->result;
        preview.replacement = source->worktree.utf8.substr(found->source.range.begin,
            found->source.range.end - found->source.range.begin);
        preview.command.edits.push_back({*replacementAnchor->result, preview.replacement});
        // An accept cannot silently replace any previously accepted contribution.
        // A section choice unmaps the line anchors inside it, so a line of
        // another section falls back to its own side's section anchor.
        for (const auto &target : targets) {
            if (target.scope != ChoiceScope::Line || target.source.side == found->source.side
                || decision(file, target.source.contributionId).value != DecisionValue::Accepted) continue;
            const auto *other = file.correspondence()->anchor(target.source.contributionId);
            if ((!other || !other->result) && target.section != found->section) {
                const auto section = std::find_if(targets.begin(), targets.end(), [&](const auto &candidate) {
                    return candidate.scope == ChoiceScope::Section && candidate.section == target.section
                        && candidate.source.side == target.source.side;
                });
                if (section != targets.end()) other = file.correspondence()->anchor(section->source.contributionId);
                if (other && other->state != AnchorState::Mapped) other = nullptr;
            }
            if (!other || !other->result || overlaps(*replacementAnchor->result, *other->result))
                return fail(Transition::Blocked, "Accepted text may overlap this choice. Preview an ordered combination or undo its acceptance.");
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
    preview.replacesManual = confirmation;
    preview.state = confirmation ? Transition::ConfirmationRequired : Transition::Applied;
    preview.detail = action == ChoiceAction::Ignore ? "Ignore preserves result text and leaves the other side undecided."
        : action == ChoiceAction::Accept ? "Accept changes only this scope and leaves the other side undecided."
        : "Resolve using this side accepts this scope and ignores its competing contributions.";
    if (file.completesMarkers(preview.command))
        preview.detail = action == ChoiceAction::ResolveUsingSide
            ? "Resolve using this side accepts this scope, ignores its competing contributions and removes the conflict's marker lines and ignored text."
            : "This decides the last open side of the conflict, so its marker lines and ignored text are removed.";
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

std::vector<NonConflictingTarget> nonConflictingTargets(
    const FileSnapshots &snapshots, const std::vector<ChoiceTarget> &choices)
{
    if (!snapshots.earlier || !snapshots.stage2 || !snapshots.stage3) return {};
    const auto &earlier = snapshots.earlier->worktree.utf8;
    const auto &left = snapshots.stage2->worktree.utf8;
    const auto &right = snapshots.stage3->worktree.utf8;
    const auto a = ExactLineCorrespondence{}.compare(earlier, left);
    const auto b = ExactLineCorrespondence{}.compare(earlier, right);
    std::set<std::string> decisions;
    for (const auto &choice : choices)
        decisions.insert(choice.decisions.begin(), choice.decisions.end());
    // Insertions at a changed range's boundary can depend on its ordering.
    // Keep them outstanding rather than guessing a composition.
    const auto competes = [](ByteRange first, ByteRange second) {
        if (first.begin == first.end) return second.begin <= first.begin && first.begin <= second.end;
        if (second.begin == second.end) return first.begin <= second.begin && second.begin <= first.end;
        return overlaps(first, second);
    };
    std::vector<NonConflictingTarget> result;
    const auto add = [&](SourceSide side, const auto &own, const auto &other,
                         const std::string &text, const std::string &otherText) {
        for (const auto &passage : own) {
            if (passage.kind == PassageKind::Equal || passage.kind == PassageKind::Ambiguous) continue;
            const auto replacement = std::string_view(text).substr(passage.result.begin,
                passage.result.end - passage.result.begin);
            const bool competing = std::any_of(other.begin(), other.end(), [&](const auto &candidate) {
                if (candidate.kind == PassageKind::Equal || !competes(passage.source, candidate.source)) return false;
                // Identical changes are safe in either scope and are deduplicated in a combined batch.
                return candidate.kind == PassageKind::Ambiguous || passage.source != candidate.source
                    || replacement != std::string_view(otherText).substr(candidate.result.begin,
                        candidate.result.end - candidate.result.begin);
            });
            if (competing) continue;
            const auto id = "non-conflicting/" + rangeId(side, passage.result);
            NonConflictingTarget target{{id + "/earlier", SourceSide::Earlier, passage.source},
                                       {id, side, passage.result}, {}};
            for (const auto &line : lines(side, text, passage.result))
                if (decisions.contains(line.contributionId)) target.decisions.push_back(line.contributionId);
            result.push_back(std::move(target));
        }
    };
    add(SourceSide::Stage2, a, b, left, right);
    add(SourceSide::Stage3, b, a, right, left);
    return result;
}

NonConflictingPreview previewNonConflicting(const FileSession &file, RevisionId expected, NonConflictingScope scope)
{
    NonConflictingPreview preview;
    preview.revision = expected;
    preview.scope = scope;
    preview.path = file.path();
    preview.command.expected = expected;
    if (expected != file.current().id) {
        preview.state = Transition::StaleRevision;
        preview.detail = "The result changed. Preview the batch again.";
        return preview;
    }
    if (file.support() != Support::Supported || !file.correspondence()) {
        preview.detail = "This file cannot apply non-conflicting changes.";
        return preview;
    }
    if (!file.snapshots()->earlier) {
        preview.detail = "The common earlier version is unavailable. Non-conflicting changes cannot be identified safely.";
        return preview;
    }
    struct Candidate {
        NonConflictingChange change;
        ResultAnchor anchor;
        ByteRange earlier;
        std::vector<std::string> decisions;
        bool unavailable = false;
    };
    std::vector<Candidate> candidates;
    const auto *mapping = file.correspondence();
    const auto &draft = file.current().draft;
    for (const auto &target : file.nonConflictingTargets()) {
        if ((scope == NonConflictingScope::Left && target.source.side != SourceSide::Stage2)
            || (scope == NonConflictingScope::Right && target.source.side != SourceSide::Stage3)) continue;
        const auto skip = [&] { ++preview.skipped; };
        if (std::any_of(target.decisions.begin(), target.decisions.end(), [&](const auto &id) {
            const auto found = draft.decisions.find(id);
            return found == draft.decisions.end() || found->second.value != DecisionValue::Undecided;
        })) { skip(); continue; }
        const auto *anchor = mapping->anchor(target.earlier.contributionId);
        if (!anchor || anchor->state != AnchorState::Mapped || !anchor->result) { skip(); continue; }
        const auto &source = target.source.side == SourceSide::Stage2 ? file.snapshots()->stage2 : file.snapshots()->stage3;
        const auto replacement = source->worktree.utf8.substr(target.source.range.begin,
            target.source.range.end - target.source.range.begin);
        // An insertion already present in the result lies beside its empty
        // earlier anchor. Its own registered source range identifies it exactly.
        if (target.earlier.range.begin == target.earlier.range.end) {
            const auto *present = mapping->anchor(target.source.contributionId);
            if (present && present->state == AnchorState::Mapped && present->result
                && (anchor->result->begin == present->result->begin || anchor->result->begin == present->result->end)
                && draft.text.substr(present->result->begin, present->result->end - present->result->begin) == replacement)
                anchor = present;
        }
        const auto range = *anchor->result;
        const auto before = draft.text.substr(range.begin, range.end - range.begin);
        const auto base = file.snapshots()->earlier->worktree.utf8.substr(target.earlier.range.begin,
            target.earlier.range.end - target.earlier.range.begin);
        if (before != replacement && (anchor->manualOverlap || before != base)) { skip(); continue; }
        // Never overwrite another accepted contribution, even if it happens
        // to contain the earlier bytes. Other decisions are preserved verbatim.
        const bool acceptedOverlap = std::any_of(draft.decisions.begin(), draft.decisions.end(), [&](const auto &entry) {
            if (entry.second.value != DecisionValue::Accepted
                || std::find(target.decisions.begin(), target.decisions.end(), entry.first) != target.decisions.end()) return false;
            const auto *accepted = mapping->anchor(entry.first);
            return !accepted || accepted->state != AnchorState::Mapped || !accepted->result
                || overlaps(range, *accepted->result);
        });
        if (acceptedOverlap) { skip(); continue; }
        if (before == replacement && target.decisions.empty()) continue;
        candidates.push_back({{{target.source}, range, before, replacement}, *anchor, target.earlier.range, target.decisions});
    }
    // Independent source ranges can collapse onto the same result passage.
    // Reject both unless they describe exactly the same contribution.
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        for (std::size_t j = i + 1; j < candidates.size(); ++j) {
            const auto &a = candidates[i].change;
            const auto &b = candidates[j].change;
            const bool collides = overlaps(a.result, b.result)
                || (a.before != a.replacement && b.before != b.replacement && a.result.begin == b.result.begin);
            if (collides && (a.result != b.result || a.replacement != b.replacement
                || candidates[i].earlier != candidates[j].earlier)) {
                candidates[i].unavailable = true;
                candidates[j].unavailable = true;
            }
        }
    }
    for (auto &candidate : candidates) {
        if (candidate.unavailable) { ++preview.skipped; continue; }
        const auto duplicate = std::find_if(preview.changes.begin(), preview.changes.end(), [&](const auto &change) {
            return change.result == candidate.change.result && change.replacement == candidate.change.replacement;
        });
        if (duplicate != preview.changes.end()) {
            duplicate->sources.push_back(candidate.change.sources.front());
        } else {
            if (candidate.change.before != candidate.change.replacement)
                preview.command.edits.push_back({candidate.change.result, candidate.change.replacement});
            preview.changes.push_back(std::move(candidate.change));
            preview.command.targets.push_back(std::move(candidate.anchor));
        }
        for (const auto &id : candidate.decisions) preview.command.decisions[id] = {DecisionValue::Accepted, {}};
    }
    preview.state = preview.changes.empty() ? Transition::NoChange : Transition::ConfirmationRequired;
    preview.detail = "Only the displayed non-conflicting changes in this file will be applied. "
        "Competing choices, existing decisions, manual edits and uncertain passages are left alone.";
    return preview;
}

Transition applyNonConflicting(FileSession &file, const NonConflictingPreview &preview, bool confirm)
{
    if (preview.revision != file.current().id) return Transition::StaleRevision;
    const auto current = previewNonConflicting(file, preview.revision, preview.scope);
    if (current != preview) return Transition::InvalidAnchor;
    if (current.state != Transition::ConfirmationRequired) return current.state;
    if (!confirm) return Transition::ConfirmationRequired;
    return file.applyCommand(current.command);
}

WholeFilePreview previewWholeFile(const FileSession &file, RevisionId expected, SourceSide side)
{
    WholeFilePreview preview;
    preview.revision = expected;
    preview.fileId = file.id();
    preview.side = side;
    if (expected != file.current().id) {
        preview.state = Transition::StaleRevision;
        preview.detail = "The result changed. Inspect a new whole-file preview.";
        return preview;
    }
    if (file.support() != Support::Supported || !file.snapshots() || !file.correspondence()) {
        preview.detail = "This file has no editable captured source.";
        return preview;
    }
    if (side != SourceSide::Stage2 && side != SourceSide::Stage3) {
        preview.state = Transition::InvalidAnchor;
        preview.detail = "Choose Left or Right for whole-file replacement.";
        return preview;
    }
    const auto &captured = *file.snapshots();
    const auto &source = side == SourceSide::Stage2 ? captured.stage2 : captured.stage3;
    if (!source) {
        preview.detail = "The selected source is unavailable.";
        return preview;
    }
    preview.origin = {captured.provenance.sessionId, file.path(), source->objectId};
    preview.before = file.current().draft.text;
    preview.replacement = source->worktree.utf8;
    preview.command.expected = expected;
    preview.command.scope = EditingScope::WholeFile;
    preview.command.edits.push_back({{0, preview.before.size()}, preview.replacement});
    for (const auto &target : file.choices()) {
        if (target.scope != ChoiceScope::Section) continue;
        for (const auto &id : target.decisions) {
            const auto found = file.current().draft.decisions.find(id);
            // Earlier recovered drafts keep their exact decision vocabulary.
            if (found == file.current().draft.decisions.end() || preview.command.decisions.contains(id)) continue;
            const auto *anchor = file.correspondence()->anchor(id);
            if (!anchor) continue;
            const Decision after{target.source.side == side ? DecisionValue::Accepted : DecisionValue::Ignored, {}};
            preview.contributions.push_back({anchor->source, found->second, after});
            preview.command.decisions.emplace(id, after);
        }
    }
    preview.retainedDecisions = file.current().draft.decisions.size() - preview.command.decisions.size();
    for (const auto &passage : *file.correspondence()->passages(side))
        if (passage.kind != PassageKind::Equal) preview.differences.push_back(passage);
    preview.state = Transition::ConfirmationRequired;
    preview.detail = "Replace the entire result for this file with the selected source. "
        "This gives up the other source's contributions and replaces all differing current text, "
        "including edits made before opening and manual edits made in this session. "
        "Inspect the full result, contribution decisions and replaced passages before confirming. "
        "Undo restores the previous text and decisions. Review the result before saving.";
    return preview;
}

Transition applyWholeFile(FileSession &file, const WholeFilePreview &preview, bool confirm)
{
    if (preview.revision != file.current().id) return Transition::StaleRevision;
    const auto current = previewWholeFile(file, preview.revision, preview.side);
    if (current != preview) return Transition::InvalidAnchor;
    if (current.state != Transition::ConfirmationRequired) return current.state;
    if (!confirm) return Transition::ConfirmationRequired;
    return file.applyCommand(current.command, true);
}
}
