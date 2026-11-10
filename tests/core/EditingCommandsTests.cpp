// Editing commands: choices, combinations, non-conflicting batches, whole-file replacement and marker resolution.
#include "TestHarness.h"
#include "core/EditingCommands.h"
#include "core/SessionFixtures.h"
#include "fixtures/ChoiceFile.h"
#include <algorithm>
#include <array>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace branch_truce;
using test_support::choose;
using test_support::engaged;
using test_support::has;
using test_support::titled;

namespace {
const char *name(ChoiceScope scope) { return scope == ChoiceScope::Line ? "line" : "section"; }

const char *name(ChoiceAction action)
{
    switch (action) {
    case ChoiceAction::Accept: return "accept";
    case ChoiceAction::Ignore: return "ignore";
    case ChoiceAction::ResolveUsingSide: return "resolve using side";
    case ChoiceAction::CombineLeftFirst: return "left first";
    case ChoiceAction::CombineRightFirst: break;
    }
    return "right first";
}

const char *name(NonConflictingScope scope)
{
    switch (scope) {
    case NonConflictingScope::Left: return "left";
    case NonConflictingScope::Right: return "right";
    case NonConflictingScope::Both: break;
    }
    return "both sides";
}

const char *name(SourceSide side)
{
    switch (side) {
    case SourceSide::Earlier: return "earlier";
    case SourceSide::Stage2: return "left";
    case SourceSide::Stage3: break;
    }
    return "right";
}

// The captured sources; a fixture without them fails the case.
const FileSnapshots &sourcesOf(const FileSession &file)
{
    const auto *sources = file.snapshots();
    CHECK(sources != nullptr);
    return *sources;
}

// The capture of `side`; a fixture without one fails the case.
const SourceSnapshot &capturedSide(const FileSession &file, SourceSide side)
{
    const auto *sources = file.snapshots();
    CHECK(sources != nullptr);
    const SourceSnapshot *captured = nullptr;
    if (sources != nullptr) {
        const auto &source = sources->source(side);
        if (source.has_value()) captured = &source.value();
    }
    CHECK(captured != nullptr);
    return *captured;
}

struct Observed {
    RevisionId id = 0;
    Draft draft;
    bool undo = false;
    bool redo = false;
};

Observed observe(const FileSession &file)
{
    return {file.current().id, file.current().draft, file.canUndo(), file.canRedo()};
}

void expectUnchanged(const FileSession &file, const Observed &before)
{
    CHECK(file.current().id == before.id);
    CHECK(file.current().draft == before.draft);
    CHECK(file.canUndo() == before.undo);
    CHECK(file.canRedo() == before.redo);
}

void expectCurrent(const FileSession &file, const Draft &draft)
{
    CHECK(file.current().draft == draft);
    CHECK(file.correspondence() != nullptr);
    CHECK(file.correspondence()->revision() == file.current().id);
}

// Undoes every step back to the first and redoes them all, checking each draft.
void replay(FileSession &file, const std::vector<Draft> &steps)
{
    CHECK(steps.size() >= 2);
    expectCurrent(file, steps.back());
    for (std::size_t index = steps.size() - 1; index > 0; --index) {
        CHECK(file.undo(file.current().id) == Transition::Applied);
        expectCurrent(file, steps[index - 1]);
    }
    CHECK(!file.canUndo() && file.canRedo());
    for (std::size_t index = 1; index < steps.size(); ++index) {
        CHECK(file.redo(file.current().id) == Transition::Applied);
        expectCurrent(file, steps[index]);
    }
    CHECK(file.canUndo() && !file.canRedo());
}

ResultAnchor anchor(const FileSession &file, const std::string &id = "choice")
{
    const auto *found = file.correspondence() ? file.correspondence()->anchor(id) : nullptr;
    CHECK(found != nullptr);
    return *found;
}

// The `scope` target of `side` whose source text is exactly `line`.
const ChoiceTarget &targetFor(const FileSession &file, SourceSide side, ChoiceScope scope, std::string_view line)
{
    const std::string_view text = capturedSide(file, side).worktree.utf8;
    for (const auto &target : file.choices()) {
        if (target.source.side != side || target.scope != scope) continue;
        if (text.substr(target.source.range.begin, target.source.range.end - target.source.range.begin) == line)
            return target;
    }
    throw std::runtime_error("fixture has no requested choice scope");
}

void expectDecision(const FileSession &file, const ChoiceTarget &target, Decision expected)
{
    CHECK(!target.decisions.empty());
    for (const auto &id : target.decisions) CHECK(file.current().draft.decisions.at(id) == expected);
}

const Decision kIgnored{DecisionValue::Ignored, {}};
const Decision kUndecided{};

bool contains(const std::vector<std::string> &ids, const std::string &id)
{
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// Opens the one diff3 block Git wrote for these versions, checks that Git's
// separator is zero-based line `separator`, then keeps each side in turn by
// accepting it and ignoring the other. Mark resolved applies once both are
// decided, and undo, redo and a replay of the drafts give the same text.
void resolvesDiff3Block(const std::string &left, const std::string &earlier, const std::string &right,
                        const std::string &merged, std::size_t separator)
{
    for (const auto kept : {SourceSide::Stage2, SourceSide::Stage3}) {
        const auto other = kept == SourceSide::Stage2 ? SourceSide::Stage3 : SourceSide::Stage2;
        auto file = test_support::diff3File(left, right, earlier, merged);
        const auto launch = file.current().draft;
        const auto scan = file.markers();
        CHECK(scan.orphans.empty() && scan.blocks.size() == 1);
        CHECK(scan.blocks.front().separator.line == separator);
        CHECK(launch.unresolvedMarkers == 1 && file.markerSections().size() == 1);
        CHECK(choose(file, kept, ChoiceAction::Accept) == Transition::Applied);
        const auto partly = file.current().draft;
        // Accepting a deletion removes the other side's text, but the block stays.
        const auto held = file.markers();
        CHECK(partly.unresolvedMarkers == 1 && held.orphans.empty() && file.markerSections().size() == 1);
        CHECK(has(file, Blocker::Undecided) && has(file, Blocker::UnresolvedMarkers));
        CHECK(choose(file, other, ChoiceAction::Ignore) == Transition::Applied);
        const auto chosen = file.current().draft;
        CHECK(chosen.text == (kept == SourceSide::Stage2 ? left : right));
        CHECK(chosen.unresolvedMarkers == 0 && file.markerSections().empty());
        CHECK(!has(file, Blocker::UnresolvedMarkers) && !has(file, Blocker::Undecided));
        CHECK(file.undo(file.current().id) == Transition::Applied);
        CHECK(file.current().draft == partly && file.markers() == held);
        CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == chosen);
        CHECK(file.beginMarkResolved(file.current().id) == Transition::Applied);
        auto replayed = test_support::diff3File(left, right, earlier, merged);
        CHECK(test_support::replay(replayed, partly) == Transition::Applied);
        CHECK(replayed.current().draft == partly && replayed.markers() == held);
        CHECK(test_support::replay(replayed, chosen) == Transition::Applied);
        CHECK(replayed.current().draft == chosen && replayed.markers().count() == 0);
        CHECK(replayed.beginMarkResolved(replayed.current().id) == Transition::Applied);
    }
}

void confirmedOrderedScopesPreserveUnrelatedTextAndDecisions(ChoiceScope scope, ChoiceAction action)
{
    auto file = test_support::combinationFile(scope);
    const auto target = test_support::choiceTarget(file, SourceSide::Stage2, scope);
    CHECK(file.editResult(1, {{0, 0}, "manual prefix\n"}) == Transition::Applied);
    const auto before = file.current();
    const auto preview = previewChoice(file, before.id, target.source.contributionId, action);
    const bool leftFirst = action == ChoiceAction::CombineLeftFirst;
    const auto left = scope == ChoiceScope::Line ? "left first\n" : "left first\nleft second\n";
    const auto right = scope == ChoiceScope::Line ? "right first\n" : "right first\nright second\n";
    const auto expected = leftFirst ? std::string(left) + right : std::string(right) + left;
    CHECK(preview.state == Transition::ConfirmationRequired && preview.replacement == expected);
    CHECK(preview.sourceOrder.size() == 2 && preview.affectedRange);
    CHECK(preview.sourceOrder[0].side == (leftFirst ? SourceSide::Stage2 : SourceSide::Stage3));
    CHECK(preview.sourceOrder[1].side != preview.sourceOrder[0].side);
    CHECK(applyChoice(file, preview) == Transition::ConfirmationRequired);
    CHECK(file.current().id == before.id && file.current().draft == before.draft);
    CHECK(applyChoice(file, preview, true) == Transition::Applied);
    auto text = before.draft.text;
    const auto &affected = engaged(preview.affectedRange);
    text.replace(affected.begin, affected.end - affected.begin, expected);
    const auto chosen = file.current().draft;
    CHECK(chosen.text == text && chosen.text.starts_with("manual prefix\nheader\n"));
    CHECK(chosen.text.ends_with("bridge\nleft later\ntail\n"));
    for (const auto &[id, value] : chosen.decisions) {
        const auto expectedDecision = preview.command.decisions.find(id);
        CHECK(value
              == (expectedDecision == preview.command.decisions.end() ? before.draft.decisions.at(id)
                                                                      : expectedDecision->second));
    }
    for (std::size_t order = 0; order < preview.sourceOrder.size(); ++order) {
        const auto &source = preview.sourceOrder[order];
        for (const auto &choice : file.choices()) {
            if (choice.source == source)
                for (const auto &id : choice.decisions)
                    CHECK(chosen.decisions.at(id) == Decision{DecisionValue::Accepted, order + 1});
        }
    }
    CHECK(file.undo(file.current().id) == Transition::Applied && file.current().draft == before.draft);
    CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == chosen);
}

void partialCombinationsCanBeReorderedRepeatedly()
{
    auto file = test_support::choiceFile();
    const auto target = test_support::choiceTarget(file, SourceSide::Stage2, ChoiceScope::Section, 1);
    for (const auto action :
         {ChoiceAction::CombineLeftFirst, ChoiceAction::CombineRightFirst, ChoiceAction::CombineLeftFirst}) {
        const auto preview = previewChoice(file, file.current().id, target.source.contributionId, action);
        CHECK(preview.state == Transition::ConfirmationRequired);
        CHECK(applyChoice(file, preview, true) == Transition::Applied);
        const auto suffix = action == ChoiceAction::CombineLeftFirst ? "left later\nright later\ntail\n"
                                                                     : "right later\nleft later\ntail\n";
        CHECK(file.current().draft.text == std::string("header\nleft first\nleft second\nbridge\n") + suffix);
    }
}

void overlappingManualWorkIsDisplayedAndPreservedByStaleOrAlteredPreviews()
{
    auto file = test_support::combinationFile(ChoiceScope::Line);
    const auto target = test_support::choiceTarget(file, SourceSide::Stage2, ChoiceScope::Line);
    CHECK(file.editResult(1, {{7, 18}, "manual first\n"}) == Transition::Applied);
    const auto before = file.current();
    const auto preview = previewChoice(file, before.id, target.source.contributionId, ChoiceAction::CombineRightFirst);
    CHECK(preview.state == Transition::ConfirmationRequired && preview.replacesManual);
    CHECK(preview.before == "manual first\n" && preview.replacement == "right first\nleft first\n");
    CHECK(applyChoice(file, preview) == Transition::ConfirmationRequired);
    auto altered = preview;
    altered.replacement += "undisplayed\n";
    CHECK(applyChoice(file, altered, true) == Transition::InvalidAnchor);
    CHECK(file.current().id == before.id && file.current().draft == before.draft);
    CHECK(file.editResult(before.id, {{0, 0}, "newer edit\n"}) == Transition::Applied);
    const auto newer = file.current();
    CHECK(applyChoice(file, preview, true) == Transition::StaleRevision);
    CHECK(file.current().id == newer.id && file.current().draft == newer.draft);
    const auto fresh = previewChoice(file, newer.id, target.source.contributionId, ChoiceAction::CombineRightFirst);
    CHECK(applyChoice(file, fresh, true) == Transition::Applied);
    CHECK(file.current().draft.text == "newer edit\nheader\nright first\nleft first\nbridge\nleft later\ntail\n");
    CHECK(file.undo(file.current().id) == Transition::Applied && file.current().draft == newer.draft);
}

void fileBatchesApplyOnlyEligibleContributionsAfterConfirmation(NonConflictingScope scope)
{
    auto file = test_support::batchFile();
    CHECK(file.editResult(1, {{0, 0}, "manual prefix\n"}) == Transition::Applied);
    const auto before = file.current();
    const auto preview = previewNonConflicting(file, before.id, scope);
    CHECK(preview.state == Transition::ConfirmationRequired && preview.scope == scope && preview.path == file.path());
    CHECK(preview.note == BatchNote::Batch);
    CHECK(preview.changes.size() == (scope == NonConflictingScope::Both ? 2 : 1));
    CHECK(applyNonConflicting(file, preview) == Transition::ConfirmationRequired);
    CHECK(file.current().id == before.id && file.current().draft == before.draft);
    CHECK(applyNonConflicting(file, preview, true) == Transition::Applied);
    const auto chosen = file.current().draft;
    std::string expected = "manual prefix\nheader\n";
    expected += scope == NonConflictingScope::Right ? "base left\n" : "left only\n";
    expected += "bridge one\n";
    expected += scope == NonConflictingScope::Left ? "base right\n" : "right only\n";
    expected += "bridge two\nbase conflict\ntail\n";
    CHECK(chosen.text == expected);
    CHECK(!preview.command.decisions.empty());
    for (const auto &[id, value] : chosen.decisions)
        CHECK(value
              == (preview.command.decisions.contains(id) ? Decision{DecisionValue::Accepted, {}}
                                                         : before.draft.decisions.at(id)));
    const auto competing = test_support::choiceTarget(file, SourceSide::Stage2, ChoiceScope::Section, 2);
    for (const auto &id : competing.decisions) CHECK(chosen.decisions.at(id).value == DecisionValue::Undecided);
    CHECK(file.undo(file.current().id) == Transition::Applied && file.current().draft == before.draft);
    CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == chosen);
    CHECK(previewNonConflicting(file, file.current().id, scope).state == Transition::NoChange);
}

void manualOverlapAndExistingDecisionsAreExcludedFromBatches()
{
    auto file = test_support::batchFile();
    const auto position = file.current().draft.text.find("base right\n");
    CHECK(file.editResult(1, {{position, position + 11}, "manual right\n"}) == Transition::Applied);
    const auto preview = previewNonConflicting(file, file.current().id);
    CHECK(preview.state == Transition::ConfirmationRequired && preview.changes.size() == 1 && preview.skipped > 0);
    CHECK(preview.changes.front().replacement == "left only\n");
    CHECK(applyNonConflicting(file, preview, true) == Transition::Applied);
    CHECK(file.current().draft.text
          == "header\nleft only\nbridge one\nmanual right\nbridge two\nbase conflict\ntail\n");
    const auto chosen = file.current().draft;
    CHECK(previewNonConflicting(file, file.current().id).state == Transition::NoChange);
    CHECK(file.current().draft == chosen);
}

void aPreviouslyIgnoredContributionStaysIgnoredInABatch()
{
    auto file = test_support::batchFile();
    const auto target = test_support::choiceTarget(file, SourceSide::Stage2, ChoiceScope::Section);
    const auto ignore = previewChoice(file, 1, target.source.contributionId, ChoiceAction::Ignore);
    CHECK(applyChoice(file, ignore) == Transition::Applied);
    const auto before = file.current().draft;
    const auto preview = previewNonConflicting(file, file.current().id);
    CHECK(preview.state == Transition::ConfirmationRequired && preview.changes.size() == 1 && preview.skipped > 0);
    CHECK(preview.changes.front().replacement == "right only\n");
    CHECK(applyNonConflicting(file, preview, true) == Transition::Applied);
    CHECK(file.current().draft.text == "header\nbase left\nbridge one\nright only\nbridge two\nbase conflict\ntail\n");
    for (const auto &id : target.decisions) {
        CHECK(before.decisions.at(id).value == DecisionValue::Ignored);
        CHECK(file.current().draft.decisions.at(id) == before.decisions.at(id));
    }
}

void absentEarlierContextStaleAndAlteredBatchesNeverApply()
{
    auto missing = test_support::choiceFile();
    const auto blocked = previewNonConflicting(missing, 1);
    CHECK(blocked.state == Transition::Blocked && blocked.note == BatchNote::NoEarlierVersion);
    CHECK(applyNonConflicting(missing, blocked, true) == Transition::Blocked && missing.current().id == 1);
    auto file = test_support::batchFile();
    const auto preview = previewNonConflicting(file, 1);
    auto altered = preview;
    altered.changes.front().replacement = "not displayed";
    CHECK(applyNonConflicting(file, altered, true) == Transition::InvalidAnchor && file.current().id == 1);
    CHECK(file.editResult(1, {{0, 0}, "manual\n"}) == Transition::Applied);
    const auto current = file.current();
    CHECK(applyNonConflicting(file, preview, true) == Transition::StaleRevision);
    CHECK(file.current().id == current.id && file.current().draft == current.draft);
}

void insertionsTouchingACompetingChangedRangeStayOutstanding()
{
    auto captured = sourcesOf(test_support::choiceFile("head\ninserted\nbase\ntail\n", "head\nchanged\ntail\n"));
    const std::string earlier = "head\nbase\ntail\n";
    captured.earlier = SourceSnapshot{"base", "100644", earlier, test_support::choiceText(earlier), "base", "Earlier"};
    captured.launchResult.text = test_support::choiceText(earlier);
    FileSession file("choices", captured.path, {earlier, {}, 0}, Support::Supported, {}, captured, true);
    CHECK(file.nonConflictingTargets().empty());
    const auto before = file.current().draft;
    const auto preview = previewNonConflicting(file, 1);
    CHECK(preview.state == Transition::NoChange && preview.changes.empty());
    CHECK(applyNonConflicting(file, preview, true) == Transition::NoChange);
    CHECK(file.current().id == 1 && file.current().draft == before);
    for (const auto &[id, value] : before.decisions) {
        (void)id;
        CHECK(value.value == DecisionValue::Undecided);
    }
}

void ambiguousRepeatedResultPassagesStayOutstanding()
{
    const auto source = test_support::batchFile();
    const auto earlier = capturedSide(source, SourceSide::Earlier).worktree.utf8;
    auto captured = sourcesOf(source);
    captured.launchResult.text = test_support::choiceText(earlier + earlier);
    FileSession file("choices", source.path(), {earlier + earlier, {}, 0}, Support::Supported, {}, captured, true);
    const auto before = file.current().draft;
    const auto preview = previewNonConflicting(file, 1);
    CHECK(preview.state == Transition::NoChange && preview.changes.empty() && preview.skipped > 0);
    CHECK(applyNonConflicting(file, preview, true) == Transition::NoChange);
    CHECK(file.current().id == 1 && file.current().draft == before);
}

void wholeFilePreviewsReplaceExactlyTheSelectedSource(SourceSide side)
{
    auto file = test_support::choiceFile();
    CHECK(file.editResult(1, {{0, 0}, "manual prefix\n"}) == Transition::Applied);
    const auto before = file.current();
    const auto preview = previewWholeFile(file, before.id, side);
    const auto &source = capturedSide(file, side);
    CHECK(preview.state == Transition::ConfirmationRequired && preview.before == before.draft.text);
    CHECK(preview.replacement == source.worktree.utf8 && preview.fileId == file.id());
    CHECK(preview.origin.sourceObjectId == source.objectId && !preview.contributions.empty()
          && !preview.differences.empty());
    CHECK(preview.note == WholeFileNote::Replace && preview.retainedDecisions == 0);
    CHECK(std::any_of(preview.differences.begin(), preview.differences.end(), [](const auto &difference) {
        return difference.result.begin == 0 && difference.result.end >= std::string("manual prefix\n").size();
    }));
    CHECK(applyWholeFile(file, preview) == Transition::ConfirmationRequired);
    CHECK(file.current().id == before.id && file.current().draft == before.draft);
    CHECK(applyWholeFile(file, preview, true) == Transition::Applied);
    const auto chosen = file.current().draft;
    CHECK(chosen.text == preview.replacement && preview.contributions.size() == chosen.decisions.size());
    for (const auto &contribution : preview.contributions) {
        CHECK(contribution.before == before.draft.decisions.at(contribution.source.contributionId));
        CHECK(chosen.decisions.at(contribution.source.contributionId) == contribution.after);
        CHECK(contribution.after
              == Decision{contribution.source.side == side ? DecisionValue::Accepted : DecisionValue::Ignored, {}});
    }
    CHECK(!file.publication().reviewed() && !file.publication().written() && !file.publication().staged());
    CHECK(file.undo(file.current().id) == Transition::Applied && file.current().draft == before.draft);
    CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == chosen);
}

void alteredOrStaleWholeFileConfirmationCannotChangeTheDraft()
{
    auto file = test_support::choiceFile();
    const auto before = file.current();
    const auto preview = previewWholeFile(file, before.id, SourceSide::Stage3);
    for (const int field : {0, 1, 2, 3}) {
        auto altered = preview;
        if (field == 0) altered.replacement += "hidden text\n";
        if (field == 1) altered.before = "different displayed result";
        if (field == 2) altered.contributions.front().after = {DecisionValue::Undecided, {}};
        if (field == 3) altered.command.edits.front().replacement = "hidden command";
        CHECK(applyWholeFile(file, altered, true) == Transition::InvalidAnchor);
        CHECK(file.current().id == before.id && file.current().draft == before.draft);
    }
    CHECK(file.editResult(before.id, {{0, 0}, "new manual edit\n"}) == Transition::Applied);
    const auto newer = file.current();
    CHECK(applyWholeFile(file, preview, true) == Transition::StaleRevision);
    CHECK(file.current().id == newer.id && file.current().draft == newer.draft);
}

void wholeFileReplacementRetainsRecoveredDecisionsWithoutSourceScopes()
{
    const auto source = test_support::choiceFile();
    const Draft legacy{"legacy manual result\n",
                       {{"legacy-left", {DecisionValue::Accepted, 3}}, {"legacy-right", {DecisionValue::Ignored, {}}}},
                       0};
    auto captured = sourcesOf(source);
    captured.launchResult.text = test_support::choiceText(legacy.text);
    FileSession file(source.id(), source.path(), legacy, Support::Supported, {}, captured);
    const auto preview = previewWholeFile(file, 1, SourceSide::Stage3);
    CHECK(preview.state == Transition::ConfirmationRequired && preview.retainedDecisions == legacy.decisions.size());
    CHECK(preview.contributions.empty() && preview.command.decisions.empty());
    CHECK(applyWholeFile(file, preview, true) == Transition::Applied);
    CHECK(file.current().draft.text == capturedSide(source, SourceSide::Stage3).worktree.utf8);
    CHECK(file.current().draft.decisions == legacy.decisions);
    CHECK(file.undo(file.current().id) == Transition::Applied && file.current().draft == legacy);
}

void scopedAcceptPreservesUnrelatedEditsAndRestoresAtomicHistory(ChoiceScope scope)
{
    auto file = test_support::choiceFile();
    const auto target = test_support::choiceTarget(file, SourceSide::Stage2, scope);
    const auto original = file.current().draft.text;
    CHECK(file.editResult(file.current().id, {{7, 18}, "manual first\n"}) == Transition::Applied);
    const auto second = file.current().draft.text.find("left second\n");
    CHECK(second != std::string::npos);
    CHECK(file.editResult(file.current().id, {{second, second + 12}, "manual second\n"}) == Transition::Applied);
    CHECK(file.editResult(file.current().id, {{0, 0}, "manual prefix\n"}) == Transition::Applied);
    const auto end = file.current().draft.text.size();
    CHECK(file.editResult(file.current().id, {{end, end}, "manual suffix\n"}) == Transition::Applied);
    const auto before = file.current().draft;
    const auto preview = previewChoice(file, file.current().id, target.source.contributionId, ChoiceAction::Accept);
    CHECK(preview.state == Transition::ConfirmationRequired);
    CHECK(applyChoice(file, preview) == Transition::ConfirmationRequired);
    CHECK(file.current().draft == before);
    CHECK(applyChoice(file, preview, true) == Transition::Applied);
    const auto accepted = file.current().draft;
    std::string expected = "manual prefix\n";
    expected +=
        scope == ChoiceScope::Section ? original : "header\nleft first\nmanual second\nbridge\nleft later\ntail\n";
    expected += "manual suffix\n";
    CHECK(accepted.text == expected);
    for (const auto &[id, decision] : accepted.decisions)
        CHECK(decision.value == (contains(target.decisions, id) ? DecisionValue::Accepted : DecisionValue::Undecided));
    CHECK(file.undo(file.current().id) == Transition::Applied && file.current().draft == before);
    CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == accepted);
    CHECK(applyChoice(file, preview, true) == Transition::StaleRevision);
    CHECK(file.current().draft == accepted);
}

void ignorePreservesManualTextAndCompetingDecisions(ChoiceScope scope)
{
    auto file = test_support::choiceFile();
    CHECK(file.editResult(1, {{7, 18}, "manual target\n"}) == Transition::Applied);
    const auto before = file.current().draft;
    const auto target = test_support::choiceTarget(file, SourceSide::Stage2, scope);
    const auto preview = previewChoice(file, file.current().id, target.source.contributionId, ChoiceAction::Ignore);
    CHECK(preview.state == Transition::Applied);
    CHECK(applyChoice(file, preview) == Transition::Applied);
    const auto ignored = file.current().draft;
    CHECK(ignored.text == before.text);
    for (const auto &[id, decision] : ignored.decisions)
        CHECK(decision.value == (contains(target.decisions, id) ? DecisionValue::Ignored : DecisionValue::Undecided));
    CHECK(previewChoice(file, file.current().id, target.source.contributionId, ChoiceAction::Accept).state
          == Transition::Blocked);
    CHECK(file.undo(file.current().id) == Transition::Applied && file.current().draft == before);
    CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == ignored);
}

void resolveUsingOneSideDecidesOnlyItsSectionAndItsCompetitors()
{
    auto file = test_support::choiceFile();
    CHECK(file.editResult(1, {{0, 0}, "manual prefix\n"}) == Transition::Applied);
    const auto before = file.current().draft;
    const auto target = test_support::choiceTarget(file, SourceSide::Stage3, ChoiceScope::Section);
    const auto preview =
        previewChoice(file, file.current().id, target.source.contributionId, ChoiceAction::ResolveUsingSide);
    CHECK(preview.state == Transition::Applied || preview.state == Transition::ConfirmationRequired);
    CHECK(applyChoice(file, preview, true) == Transition::Applied);
    const auto resolved = file.current().draft;
    CHECK(resolved.text == "manual prefix\nheader\nright first\nright second\nbridge\nleft later\ntail\n");
    const auto &competitors = file.catalogue().opposite(target)->decisions;
    for (const auto &[id, decision] : resolved.decisions) {
        const bool own = contains(target.decisions, id);
        const bool competitor = contains(competitors, id);
        CHECK(decision.value
              == (own ? DecisionValue::Accepted
                      : competitor ? DecisionValue::Ignored
                                   : DecisionValue::Undecided));
    }
    CHECK(file.undo(file.current().id) == Transition::Applied && file.current().draft == before);
    CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == resolved);
}

void resolveUsingOneLineLeavesTheOtherSectionAndManualEditsUnchanged()
{
    auto file = test_support::choiceFile("header\nleft\nbridge\nleft later\ntail\n",
                                         "header\nright\nbridge\nright later\ntail\n");
    CHECK(file.editResult(1, {{0, 0}, "manual prefix\n"}) == Transition::Applied);
    const auto before = file.current().draft;
    const auto target = test_support::choiceTarget(file, SourceSide::Stage3, ChoiceScope::Line);
    const auto competitor = test_support::choiceTarget(file, SourceSide::Stage2, ChoiceScope::Line);
    const auto preview =
        previewChoice(file, file.current().id, target.source.contributionId, ChoiceAction::ResolveUsingSide);
    CHECK(preview.state == Transition::Applied || preview.state == Transition::ConfirmationRequired);
    CHECK(applyChoice(file, preview, true) == Transition::Applied);
    const auto chosen = file.current().draft;
    CHECK(chosen.text == "manual prefix\nheader\nright\nbridge\nleft later\ntail\n");
    CHECK(chosen.decisions.at(target.decisions.front()).value == DecisionValue::Accepted);
    CHECK(chosen.decisions.at(competitor.decisions.front()).value == DecisionValue::Ignored);
    const auto later = test_support::choiceTarget(file, SourceSide::Stage2, ChoiceScope::Line, 1);
    CHECK(chosen.decisions.at(later.decisions.front()).value == DecisionValue::Undecided);
    CHECK(file.undo(file.current().id) == Transition::Applied && file.current().draft == before);
    CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == chosen);
}

// Resolves the first section with `first`, then decides the second with
// `action` on the other side.
void decideSectionsWithDifferentSides(bool marked, ChoiceAction action, SourceSide first)
{
    auto file = marked ? test_support::markedFile() : test_support::choiceFile();
    const auto second = first == SourceSide::Stage2 ? SourceSide::Stage3 : SourceSide::Stage2;
    CHECK(choose(file, first, ChoiceAction::ResolveUsingSide) == Transition::Applied);
    const auto before = file.current().draft;
    CHECK(choose(file, second, action, ChoiceScope::Section, 1) == Transition::Applied);
    const auto chosen = file.current().draft;
    if (marked && action == ChoiceAction::Accept) {
        CHECK(chosen.text == before.text && chosen.unresolvedMarkers == 1);
    } else {
        std::string expected = first == SourceSide::Stage2 ? "header\nleft first\nleft second\nbridge\n"
                                                           : "header\nright first\nright second\nbridge\n";
        expected += second == SourceSide::Stage2 ? "left later\n" : "right later\n";
        expected += "tail\n";
        CHECK(chosen.text == expected);
        CHECK(chosen.unresolvedMarkers == 0);
    }
    const auto &picked = test_support::choiceTarget(file, second, ChoiceScope::Section, 1);
    for (const auto &id : picked.decisions) CHECK(chosen.decisions.at(id).value == DecisionValue::Accepted);
    CHECK(file.undo(file.current().id) == Transition::Applied && file.current().draft == before);
}

void eachSectionCanBeDecidedWithADifferentSide()
{
    for (const bool marked : {false, true})
        for (const auto action : {ChoiceAction::ResolveUsingSide, ChoiceAction::Accept})
            for (const auto first : {SourceSide::Stage2, SourceSide::Stage3})
                decideSectionsWithDifferentSides(marked, action, first);
}

void anAcceptStillRefusesAcceptedTextItMayReplace()
{
    // The competing side of the same section holds the accepted text.
    auto same = test_support::choiceFile();
    CHECK(choose(same, SourceSide::Stage2, ChoiceAction::Accept) == Transition::Applied);
    for (const auto action : {ChoiceAction::Accept, ChoiceAction::ResolveUsingSide}) {
        const auto &right = test_support::choiceTarget(same, SourceSide::Stage3, ChoiceScope::Section);
        CHECK(previewChoice(same, same.current().id, right.source.contributionId, action).state == Transition::Blocked);
    }
    // Typing across the start of the accepted section leaves neither
    // its lines nor the section locatable.
    auto typed = test_support::choiceFile();
    CHECK(choose(typed, SourceSide::Stage2, ChoiceAction::Accept) == Transition::Applied);
    const auto at = typed.current().draft.text.find("\nleft first");
    CHECK(typed.editResult(typed.current().id, {{at, at + 2}, "\nL"}) == Transition::Applied);
    const auto &later = test_support::choiceTarget(typed, SourceSide::Stage3, ChoiceScope::Section, 1);
    const auto refused = previewChoice(typed, typed.current().id, later.source.contributionId, ChoiceAction::Accept);
    CHECK(refused.state == Transition::Blocked);
    CHECK(refused.note == ChoiceNote::AcceptedOverlap);
}

void staleAmbiguousAndCancelledCommandsLeaveTextAndDecisionsUntouched()
{
    auto ambiguous = test_support::choiceFile("head\nrepeat\nrepeat\ntail\n", "head\nchanged\nrepeat\ntail\n",
                                              "head\nrepeat\ntail\n");
    const auto ambiguousSources = sourcesOf(ambiguous);
    const std::string_view left = capturedSide(ambiguous, SourceSide::Stage2).worktree.utf8;
    int repeats = 0;
    for (const auto &target : ambiguous.choices()) {
        if (target.scope != ChoiceScope::Line || target.source.side != SourceSide::Stage2) continue;
        const auto line = left.substr(target.source.range.begin, target.source.range.end - target.source.range.begin);
        if (line != "repeat\n") continue;
        ++repeats;
        const auto before = observe(ambiguous);
        const auto rejected = previewChoice(ambiguous, before.id, target.source.contributionId, ChoiceAction::Accept);
        CHECK(rejected.state == Transition::AmbiguousTarget);
        CHECK(applyChoice(ambiguous, rejected, true) == Transition::AmbiguousTarget);
        expectUnchanged(ambiguous, before);
    }
    CHECK(repeats == 2);
    CHECK(sourcesOf(ambiguous) == ambiguousSources);

    auto stale = test_support::choiceFile();
    const auto line = targetFor(stale, SourceSide::Stage2, ChoiceScope::Line, "left first\n");
    const auto preview = previewChoice(stale, 1, line.source.contributionId, ChoiceAction::Accept);
    CHECK(stale.editResult(1, {{0, 0}, "new manual edit\n"}) == Transition::Applied);
    const auto afterEdit = observe(stale);
    CHECK(applyChoice(stale, preview, true) == Transition::StaleRevision);
    expectUnchanged(stale, afterEdit);

    auto missingEarlier = test_support::choiceFile();
    const auto blocked = observe(missingEarlier);
    const auto unavailable = previewNonConflicting(missingEarlier, blocked.id);
    CHECK(unavailable.state == Transition::Blocked);
    CHECK(applyNonConflicting(missingEarlier, unavailable, true) == Transition::Blocked);
    expectUnchanged(missingEarlier, blocked);

    const std::string prefix = "manual prefix\n";
    auto combination = test_support::combinationFile(ChoiceScope::Line);
    CHECK(combination.editResult(1, {{0, 0}, prefix}) == Transition::Applied);
    const auto first = targetFor(combination, SourceSide::Stage2, ChoiceScope::Line, "left first\n");
    const auto combined = previewChoice(combination, combination.current().id, first.source.contributionId,
                                        ChoiceAction::CombineRightFirst);
    const auto combinedBefore = observe(combination);
    CHECK(combined.state == Transition::ConfirmationRequired);
    CHECK(combined.replacement == "right first\nleft first\n");
    CHECK(applyChoice(combination, combined) == Transition::ConfirmationRequired);
    expectUnchanged(combination, combinedBefore);
    auto altered = combined;
    altered.replacement += "undisplayed\n";
    CHECK(applyChoice(combination, altered, true) == Transition::InvalidAnchor);
    expectUnchanged(combination, combinedBefore);

    auto batch = test_support::batchFile();
    CHECK(batch.editResult(1, {{0, 0}, prefix}) == Transition::Applied);
    const auto batchPreview = previewNonConflicting(batch, batch.current().id, NonConflictingScope::Both);
    const auto batchBefore = observe(batch);
    CHECK(batchPreview.state == Transition::ConfirmationRequired && batchPreview.changes.size() == 2);
    CHECK(applyNonConflicting(batch, batchPreview) == Transition::ConfirmationRequired);
    expectUnchanged(batch, batchBefore);

    auto whole = test_support::choiceFile();
    CHECK(whole.editResult(1, {{0, 0}, prefix}) == Transition::Applied);
    const auto wholePreview = previewWholeFile(whole, whole.current().id, SourceSide::Stage3);
    const auto wholeBefore = observe(whole);
    CHECK(wholePreview.state == Transition::ConfirmationRequired);
    CHECK(wholePreview.before == wholeBefore.draft.text);
    CHECK(wholePreview.replacement == capturedSide(whole, SourceSide::Stage3).worktree.utf8);
    CHECK(wholePreview.replacement != wholeBefore.draft.text);
    CHECK(applyWholeFile(whole, wholePreview) == Transition::ConfirmationRequired);
    expectUnchanged(whole, wholeBefore);
}

void ignoreAndScopedAcceptancePreserveUnrelatedTextAndDecisions()
{
    const std::string prefix = "manual prefix\n";
    const std::string original = "header\nleft first\nleft second\nbridge\nleft later\ntail\n";
    auto file = test_support::choiceFile();
    const auto sources = sourcesOf(file);
    const auto leftFirst = targetFor(file, SourceSide::Stage2, ChoiceScope::Line, "left first\n");
    const auto leftSecond = targetFor(file, SourceSide::Stage2, ChoiceScope::Line, "left second\n");
    const auto rightFirst = targetFor(file, SourceSide::Stage3, ChoiceScope::Line, "right first\n");
    const auto rightSecond = targetFor(file, SourceSide::Stage3, ChoiceScope::Line, "right second\n");
    const auto leftLater = targetFor(file, SourceSide::Stage2, ChoiceScope::Line, "left later\n");
    const auto rightLater = targetFor(file, SourceSide::Stage3, ChoiceScope::Line, "right later\n");
    CHECK(file.current().draft.decisions.size() == 6);
    std::vector<Draft> steps{file.current().draft};
    CHECK(file.editResult(1, {{0, 0}, prefix}) == Transition::Applied);
    CHECK(file.current().draft.text == prefix + original);
    steps.push_back(file.current().draft);

    const auto acceptPresent =
        previewChoice(file, file.current().id, leftFirst.source.contributionId, ChoiceAction::Accept);
    CHECK(acceptPresent.state == Transition::Applied);
    CHECK(applyChoice(file, acceptPresent) == Transition::Applied);
    CHECK(file.current().draft.text == prefix + original);
    expectDecision(file, leftFirst, Decision{DecisionValue::Accepted, {}});
    expectDecision(file, leftSecond, kUndecided);
    expectDecision(file, rightFirst, kUndecided);
    expectDecision(file, rightSecond, kUndecided);
    expectDecision(file, leftLater, kUndecided);
    expectDecision(file, rightLater, kUndecided);
    steps.push_back(file.current().draft);

    const auto acceptOther =
        previewChoice(file, file.current().id, rightLater.source.contributionId, ChoiceAction::Accept);
    CHECK(acceptOther.state == Transition::ConfirmationRequired);
    CHECK(acceptOther.replacement == "right later\n");
    CHECK(applyChoice(file, acceptOther) == Transition::ConfirmationRequired);
    expectCurrent(file, steps.back());
    CHECK(applyChoice(file, acceptOther, true) == Transition::Applied);
    CHECK(file.current().draft.text == prefix + "header\nleft first\nleft second\nbridge\nright later\ntail\n");
    expectDecision(file, leftFirst, Decision{DecisionValue::Accepted, {}});
    expectDecision(file, rightLater, Decision{DecisionValue::Accepted, {}});
    expectDecision(file, leftLater, kUndecided);
    expectDecision(file, leftSecond, kUndecided);
    expectDecision(file, rightFirst, kUndecided);
    expectDecision(file, rightSecond, kUndecided);
    steps.push_back(file.current().draft);

    const auto ignore = previewChoice(file, file.current().id, leftSecond.source.contributionId, ChoiceAction::Ignore);
    CHECK(ignore.state == Transition::Applied);
    CHECK(applyChoice(file, ignore) == Transition::Applied);
    CHECK(file.current().draft.text == steps.back().text);
    expectDecision(file, leftSecond, kIgnored);
    expectDecision(file, leftFirst, Decision{DecisionValue::Accepted, {}});
    expectDecision(file, rightLater, Decision{DecisionValue::Accepted, {}});
    expectDecision(file, leftLater, kUndecided);
    expectDecision(file, rightFirst, kUndecided);
    expectDecision(file, rightSecond, kUndecided);
    steps.push_back(file.current().draft);
    replay(file, steps);
    CHECK(sourcesOf(file) == sources);
}

void linesInsideAMultiLineConflictPairByIndex()
{
    auto file = test_support::commonVersionFile();
    const auto sources = sourcesOf(file);
    const auto line = [&](SourceSide side, std::string_view text) {
        return targetFor(file, side, ChoiceScope::Line, text);
    };
    const auto leftOne = line(SourceSide::Stage2, "left one\n");
    const auto leftTwo = line(SourceSide::Stage2, "left two\n");
    const auto leftThree = line(SourceSide::Stage2, "left three\n");
    const auto rightOne = line(SourceSide::Stage3, "right one\n");
    const auto rightTwo = line(SourceSide::Stage3, "right two\n");
    const auto rightThree = line(SourceSide::Stage3, "right three\n");
    const auto mapped = [&](const ChoiceTarget &target) {
        const auto found = anchor(file, target.source.contributionId);
        CHECK(found.state == AnchorState::Mapped && found.result.has_value());
        const auto &result = engaged(found.result);
        return file.current().draft.text.substr(result.begin, result.end - result.begin);
    };
    // Each line maps to the result line at its own index, and replacing
    // that launch text needs confirmation. The section keeps the whole block.
    for (const auto &[target, expected] : {std::pair{leftOne, "base one\n"},
                                           {leftTwo, "base two\n"},
                                           {leftThree, "base three\n"},
                                           {rightOne, "base one\n"},
                                           {rightTwo, "base two\n"},
                                           {rightThree, "base three\n"}}) {
        CHECK(mapped(target) == expected);
        CHECK(anchor(file, target.source.contributionId).manualOverlap);
    }
    CHECK(mapped(targetFor(file, SourceSide::Stage2, ChoiceScope::Section, "left one\nleft two\nleft three\n"))
          == "base one\nbase two\nbase three\n");
    std::vector<Draft> steps{file.current().draft};

    const auto acceptLeft = previewChoice(file, file.current().id, leftTwo.source.contributionId, ChoiceAction::Accept);
    CHECK(acceptLeft.state == Transition::ConfirmationRequired);
    CHECK(acceptLeft.before == "base two\n" && acceptLeft.replacement == "left two\n");
    CHECK(applyChoice(file, acceptLeft) == Transition::ConfirmationRequired);
    expectCurrent(file, steps.back());
    CHECK(applyChoice(file, acceptLeft, true) == Transition::Applied);
    CHECK(file.current().draft.text == "header\nbase one\nleft two\nbase three\ntail\n");
    expectDecision(file, leftTwo, Decision{DecisionValue::Accepted, {}});
    for (const auto &other : {leftOne, leftThree, rightOne, rightTwo, rightThree})
        expectDecision(file, other, kUndecided);
    // The competing line follows the text that replaced its own.
    CHECK(mapped(rightTwo) == "left two\n");
    steps.push_back(file.current().draft);

    const auto acceptRight =
        previewChoice(file, file.current().id, rightThree.source.contributionId, ChoiceAction::Accept);
    CHECK(acceptRight.state == Transition::ConfirmationRequired);
    CHECK(acceptRight.before == "base three\n" && acceptRight.replacement == "right three\n");
    CHECK(applyChoice(file, acceptRight, true) == Transition::Applied);
    CHECK(file.current().draft.text == "header\nbase one\nleft two\nright three\ntail\n");
    expectDecision(file, leftTwo, Decision{DecisionValue::Accepted, {}});
    expectDecision(file, rightThree, Decision{DecisionValue::Accepted, {}});
    for (const auto &other : {leftOne, leftThree, rightOne, rightTwo}) expectDecision(file, other, kUndecided);
    steps.push_back(file.current().draft);

    const auto ignore = previewChoice(file, file.current().id, leftOne.source.contributionId, ChoiceAction::Ignore);
    CHECK(ignore.state == Transition::Applied);
    CHECK(applyChoice(file, ignore) == Transition::Applied);
    CHECK(file.current().draft.text == steps.back().text);
    expectDecision(file, leftOne, kIgnored);
    expectDecision(file, rightOne, kUndecided);
    steps.push_back(file.current().draft);
    // The block now holds a line from each side and matches neither source.
    CHECK(file.current().draft.text != capturedSide(file, SourceSide::Stage2).worktree.utf8);
    CHECK(file.current().draft.text != capturedSide(file, SourceSide::Stage3).worktree.utf8);
    replay(file, steps);
    CHECK(sourcesOf(file) == sources);
}

void surplusLinesInsertAtTheEndOfAnUnevenBlock()
{
    auto file = test_support::commonVersionFile("header\nl1\nl2\nl3\nl4\ntail\n", "header\nr1\nr2\ntail\n",
                                                "header\nb1\nb2\nb3\ntail\n");
    const auto sources = sourcesOf(file);
    const auto l1 = targetFor(file, SourceSide::Stage2, ChoiceScope::Line, "l1\n");
    const auto l2 = targetFor(file, SourceSide::Stage2, ChoiceScope::Line, "l2\n");
    const auto l3 = targetFor(file, SourceSide::Stage2, ChoiceScope::Line, "l3\n");
    const auto l4 = targetFor(file, SourceSide::Stage2, ChoiceScope::Line, "l4\n");
    const auto r1 = targetFor(file, SourceSide::Stage3, ChoiceScope::Line, "r1\n");
    const auto r2 = targetFor(file, SourceSide::Stage3, ChoiceScope::Line, "r2\n");
    const auto range = [&](const ChoiceTarget &target) {
        const auto found = anchor(file, target.source.contributionId);
        CHECK(found.state == AnchorState::Mapped && found.result.has_value());
        return engaged(found.result);
    };
    const auto at = [&](std::string_view text) {
        const auto found = file.current().draft.text.find(text);
        CHECK(found != std::string::npos);
        return found;
    };
    // Four left lines against three result lines: the fourth inserts at the end.
    CHECK(range(l1) == ByteRange{at("b1\n"), at("b2\n")});
    CHECK(range(l2) == ByteRange{at("b2\n"), at("b3\n")});
    CHECK(range(l3) == ByteRange{at("b3\n"), at("tail\n")});
    CHECK(range(l4) == ByteRange{at("tail\n"), at("tail\n")});
    CHECK(!anchor(file, l4.source.contributionId).manualOverlap);
    // Two right lines against three: the last one keeps the extra result line.
    CHECK(range(r1) == ByteRange{at("b1\n"), at("b2\n")});
    CHECK(range(r2) == ByteRange{at("b2\n"), at("tail\n")});
    std::vector<Draft> steps{file.current().draft};

    for (const auto *target : {&l1, &r2}) {
        const auto preview =
            previewChoice(file, file.current().id, target->source.contributionId, ChoiceAction::Accept);
        CHECK(preview.state == Transition::ConfirmationRequired);
        CHECK(applyChoice(file, preview, true) == Transition::Applied);
        steps.push_back(file.current().draft);
    }
    CHECK(file.current().draft.text == "header\nl1\nr2\ntail\n");
    // Accepting the right line replaced the result lines the second and
    // third left lines held, but the insertion point stays at the end.
    CHECK(anchor(file, l2.source.contributionId).state == AnchorState::Invalidated);
    CHECK(range(l4) == ByteRange{at("tail\n"), at("tail\n")});
    const auto insert = previewChoice(file, file.current().id, l4.source.contributionId, ChoiceAction::Accept);
    CHECK(insert.state == Transition::Applied && insert.before.empty() && insert.replacement == "l4\n");
    CHECK(applyChoice(file, insert) == Transition::Applied);
    CHECK(file.current().draft.text == "header\nl1\nr2\nl4\ntail\n");
    for (const auto &chosen : {l1, r2, l4}) expectDecision(file, chosen, Decision{DecisionValue::Accepted, {}});
    for (const auto &other : {l2, l3, r1}) expectDecision(file, other, kUndecided);
    steps.push_back(file.current().draft);
    replay(file, steps);
    CHECK(sourcesOf(file) == sources);

    // Several surplus lines accepted in source order keep that order.
    auto surplus =
        test_support::commonVersionFile("header\nl1\nl2\nl3\ntail\n", "header\nr1\ntail\n", "header\nb1\ntail\n");
    for (const auto *text : {"l2\n", "l3\n"}) {
        const auto target = targetFor(surplus, SourceSide::Stage2, ChoiceScope::Line, text);
        const auto preview =
            previewChoice(surplus, surplus.current().id, target.source.contributionId, ChoiceAction::Accept);
        CHECK(preview.state == Transition::Applied);
        CHECK(applyChoice(surplus, preview) == Transition::Applied);
    }
    CHECK(surplus.current().draft.text == "header\nb1\nl2\nl3\ntail\n");
}

void repeatedIdenticalLinesInAMultiLineConflictStayUnmapped()
{
    auto file = test_support::commonVersionFile("head\nrepeat\nrepeat\nmid\nleft one\nleft two\ntail\n",
                                                "head\nright\ntail\n", "head\nrepeat\nmid\nbase one\nbase two\ntail\n");
    const auto sources = sourcesOf(file);
    const std::string_view text = capturedSide(file, SourceSide::Stage2).worktree.utf8;
    const ChoiceTarget *section = nullptr;
    int repeats = 0;
    for (const auto &target : file.choices()) {
        if (target.source.side != SourceSide::Stage2) continue;
        if (target.scope == ChoiceScope::Section) {
            section = &target;
            continue;
        }
        if (text.substr(target.source.range.begin, target.source.range.end - target.source.range.begin) != "repeat\n")
            continue;
        ++repeats;
        CHECK(anchor(file, target.source.contributionId).state == AnchorState::Ambiguous);
        for (const auto action : {ChoiceAction::Accept, ChoiceAction::Ignore}) {
            const auto before = observe(file);
            const auto rejected = previewChoice(file, before.id, target.source.contributionId, action);
            CHECK(rejected.state == Transition::AmbiguousTarget);
            CHECK(rejected.note == ChoiceNote::UnmappedScope);
            CHECK(applyChoice(file, rejected, true) == Transition::AmbiguousTarget);
            expectUnchanged(file, before);
        }
    }
    CHECK(repeats == 2);
    // Every other line of the same section still has its own target.
    CHECK(section != nullptr && section->decisions.size() == 5);
    const auto mid = targetFor(file, SourceSide::Stage2, ChoiceScope::Line, "mid\n");
    CHECK(anchor(file, mid.source.contributionId).state == AnchorState::Mapped);
    CHECK(anchor(file, targetFor(file, SourceSide::Stage2, ChoiceScope::Line, "left one\n").source.contributionId).state
          == AnchorState::Mapped);
    const auto leftTwo = targetFor(file, SourceSide::Stage2, ChoiceScope::Line, "left two\n");
    const auto accept = previewChoice(file, file.current().id, leftTwo.source.contributionId, ChoiceAction::Accept);
    CHECK(accept.state == Transition::ConfirmationRequired && accept.before == "base two\n");
    CHECK(applyChoice(file, accept, true) == Transition::Applied);
    CHECK(file.current().draft.text == "head\nrepeat\nmid\nbase one\nleft two\ntail\n");
    expectDecision(file, leftTwo, Decision{DecisionValue::Accepted, {}});
    CHECK(sourcesOf(file) == sources);
}

void orderedPartialCombinationsPreserveUnrelatedContentAndManualEdits()
{
    const std::string prefix = "manual prefix\n";
    auto file = test_support::combinationFile(ChoiceScope::Line);
    const auto sources = sourcesOf(file);
    const auto leftFirst = targetFor(file, SourceSide::Stage2, ChoiceScope::Line, "left first\n");
    const auto rightFirst = targetFor(file, SourceSide::Stage3, ChoiceScope::Line, "right first\n");
    const auto leftLater = targetFor(file, SourceSide::Stage2, ChoiceScope::Line, "left later\n");
    const auto rightLater = targetFor(file, SourceSide::Stage3, ChoiceScope::Line, "right later\n");
    CHECK(file.current().draft.decisions.size() == 4);
    std::vector<Draft> steps{file.current().draft};
    CHECK(file.editResult(1, {{0, 0}, prefix}) == Transition::Applied);
    steps.push_back(file.current().draft);
    const auto ignore = previewChoice(file, file.current().id, leftLater.source.contributionId, ChoiceAction::Ignore);
    CHECK(ignore.state == Transition::Applied);
    CHECK(applyChoice(file, ignore) == Transition::Applied);
    CHECK(file.current().draft.text == prefix + "header\nleft first\nbridge\nleft later\ntail\n");
    expectDecision(file, leftLater, kIgnored);
    expectDecision(file, rightLater, kUndecided);
    expectDecision(file, leftFirst, kUndecided);
    expectDecision(file, rightFirst, kUndecided);
    steps.push_back(file.current().draft);

    const auto preview =
        previewChoice(file, file.current().id, leftFirst.source.contributionId, ChoiceAction::CombineRightFirst);
    CHECK(preview.state == Transition::ConfirmationRequired);
    CHECK(preview.replacement == "right first\nleft first\n");
    CHECK(preview.sourceOrder.size() == 2);
    CHECK(preview.sourceOrder[0].side == SourceSide::Stage3);
    CHECK(preview.sourceOrder[1].side == SourceSide::Stage2);
    CHECK(applyChoice(file, preview) == Transition::ConfirmationRequired);
    expectCurrent(file, steps.back());
    CHECK(applyChoice(file, preview, true) == Transition::Applied);
    CHECK(file.current().draft.text == prefix + "header\nright first\nleft first\nbridge\nleft later\ntail\n");
    expectDecision(file, rightFirst, Decision{DecisionValue::Accepted, 1});
    expectDecision(file, leftFirst, Decision{DecisionValue::Accepted, 2});
    expectDecision(file, leftLater, kIgnored);
    expectDecision(file, rightLater, kUndecided);
    steps.push_back(file.current().draft);
    replay(file, steps);
    CHECK(sourcesOf(file) == sources);
}

void batchesAndWholeFileReplacementUndoAndRedoInActionOrder()
{
    const std::string prefix = "manual prefix\n";
    const std::string earlier = "header\nbase left\nbridge one\nbase right\nbridge two\nbase conflict\ntail\n";
    const std::string batched = prefix + "header\nleft only\nbridge one\nright only\nbridge two\nbase conflict\ntail\n";
    auto file = test_support::batchFile();
    const auto sources = sourcesOf(file);
    const auto &right = capturedSide(file, SourceSide::Stage3).worktree.utf8;
    const auto conflict = targetFor(file, SourceSide::Stage2, ChoiceScope::Section, "left conflict\n");
    std::vector<Draft> steps{file.current().draft};
    CHECK(steps.back().text == earlier);
    CHECK(file.editResult(1, {{0, 0}, prefix}) == Transition::Applied);
    CHECK(file.current().draft.text == prefix + earlier);
    steps.push_back(file.current().draft);

    const auto ignore = previewChoice(file, file.current().id, conflict.source.contributionId, ChoiceAction::Ignore);
    CHECK(ignore.state == Transition::Applied);
    CHECK(applyChoice(file, ignore) == Transition::Applied);
    CHECK(file.current().draft.text == steps.back().text);
    expectDecision(file, conflict, kIgnored);
    steps.push_back(file.current().draft);
    const auto ignoredDraft = steps.back();

    const auto batch = previewNonConflicting(file, file.current().id, NonConflictingScope::Both);
    CHECK(batch.state == Transition::ConfirmationRequired);
    CHECK(batch.changes.size() == 2);
    CHECK(batch.changes[0].replacement == "left only\n");
    CHECK(batch.changes[1].replacement == "right only\n");
    CHECK(applyNonConflicting(file, batch, true) == Transition::Applied);
    CHECK(file.current().draft.text == batched);
    for (const auto &[id, value] : file.current().draft.decisions) {
        if (batch.command.decisions.contains(id))
            CHECK(value == Decision{DecisionValue::Accepted, {}});
        else
            CHECK(value == ignoredDraft.decisions.at(id));
    }
    expectDecision(file, conflict, kIgnored);
    steps.push_back(file.current().draft);

    const auto whole = previewWholeFile(file, file.current().id, SourceSide::Stage3);
    CHECK(whole.state == Transition::ConfirmationRequired);
    CHECK(whole.replacement == right);
    CHECK(applyWholeFile(file, whole, true) == Transition::Applied);
    CHECK(file.current().draft.text == right);
    CHECK(file.current().draft.text == "header\nbase left\nbridge one\nright only\nbridge two\nright conflict\ntail\n");
    for (const auto &contribution : whole.contributions) {
        const auto expected = Decision{
            contribution.source.side == SourceSide::Stage3 ? DecisionValue::Accepted : DecisionValue::Ignored, {}};
        CHECK(contribution.after == expected);
        CHECK(file.current().draft.decisions.at(contribution.source.contributionId) == expected);
    }
    steps.push_back(file.current().draft);
    replay(file, steps);

    CHECK(file.undo(file.current().id) == Transition::Applied);
    expectCurrent(file, steps[steps.size() - 2]);
    CHECK(file.canRedo());
    CHECK(applyWholeFile(file, whole, true) == Transition::StaleRevision);
    expectCurrent(file, steps[steps.size() - 2]);
    CHECK(file.editResult(file.current().id, {{0, 0}, "later\n"}) == Transition::Applied);
    CHECK(!file.canRedo());
    CHECK(file.current().draft.text == "later\n" + batched);
    expectDecision(file, conflict, kIgnored);
    CHECK(sourcesOf(file) == sources);
}

// The ways of deciding the first section of a marked file completely.
enum class FirstSection { ResolveLeft, ResolveRight, AcceptThenIgnore, IgnoreThenAccept, LeftFirst, RightFirst, Lines };

// Decides the first section of `file` along `path` and returns the text it keeps.
std::string decideFirstSection(FileSession &file, FirstSection path, const std::string &launch)
{
    switch (path) {
    case FirstSection::ResolveLeft:
        CHECK(choose(file, SourceSide::Stage2, ChoiceAction::ResolveUsingSide) == Transition::Applied);
        return "left first\nleft second\n";
    case FirstSection::ResolveRight:
        CHECK(choose(file, SourceSide::Stage3, ChoiceAction::ResolveUsingSide) == Transition::Applied);
        return "right first\nright second\n";
    case FirstSection::AcceptThenIgnore:
        CHECK(choose(file, SourceSide::Stage2, ChoiceAction::Accept) == Transition::Applied);
        CHECK(file.current().draft.text == launch);
        CHECK(choose(file, SourceSide::Stage3, ChoiceAction::Ignore) == Transition::Applied);
        return "left first\nleft second\n";
    case FirstSection::IgnoreThenAccept:
        CHECK(choose(file, SourceSide::Stage2, ChoiceAction::Ignore) == Transition::Applied);
        CHECK(file.current().draft.text == launch);
        CHECK(choose(file, SourceSide::Stage3, ChoiceAction::Accept) == Transition::Applied);
        return "right first\nright second\n";
    case FirstSection::LeftFirst:
        CHECK(choose(file, SourceSide::Stage2, ChoiceAction::CombineLeftFirst) == Transition::Applied);
        return "left first\nleft second\nright first\nright second\n";
    case FirstSection::RightFirst:
        CHECK(choose(file, SourceSide::Stage3, ChoiceAction::CombineRightFirst) == Transition::Applied);
        return "right first\nright second\nleft first\nleft second\n";
    case FirstSection::Lines: break;
    }
    CHECK(choose(file, SourceSide::Stage2, ChoiceAction::Accept, ChoiceScope::Line, 0) == Transition::Applied);
    CHECK(choose(file, SourceSide::Stage2, ChoiceAction::Ignore, ChoiceScope::Line, 1) == Transition::Applied);
    CHECK(choose(file, SourceSide::Stage3, ChoiceAction::Ignore, ChoiceScope::Line, 0) == Transition::Applied);
    CHECK(file.current().draft.text == launch);
    CHECK(choose(file, SourceSide::Stage3, ChoiceAction::Accept, ChoiceScope::Line, 1) == Transition::Applied);
    return "left first\nright second\n";
}

void eachFullDecisionRemovesItsSectionsMarkerLinesInOneStep()
{
    for (const bool base : {false, true}) {
        const auto launch = test_support::markedText(base);
        // The second block is still open, so its marker lines stay.
        const auto later = launch.substr(launch.find("bridge\n"));
        for (const auto path :
             {FirstSection::ResolveLeft, FirstSection::ResolveRight, FirstSection::AcceptThenIgnore,
              FirstSection::IgnoreThenAccept, FirstSection::LeftFirst, FirstSection::RightFirst, FirstSection::Lines}) {
            auto file = test_support::markedFile(base);
            CHECK(file.markerSections().size() == 2);
            std::string expected = "header\n";
            expected += decideFirstSection(file, path, launch);
            expected += later;
            const auto chosen = file.current().draft;
            CHECK(chosen.text == expected);
            CHECK(chosen.unresolvedMarkers == 1 && file.markers().blocks.size() == 1);
            CHECK(file.markerSections().size() == 1 && has(file, Blocker::UnresolvedMarkers));
            CHECK(file.undo(file.current().id) == Transition::Applied);
            CHECK(file.current().draft.text == launch && file.current().draft.unresolvedMarkers == 2);
            CHECK(file.current().draft.decisions != chosen.decisions && file.markerSections().size() == 2);
            CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == chosen);
        }
    }
    auto file = test_support::markedFile();
    CHECK(choose(file, SourceSide::Stage2, ChoiceAction::ResolveUsingSide) == Transition::Applied);
    CHECK(choose(file, SourceSide::Stage3, ChoiceAction::ResolveUsingSide, ChoiceScope::Section, 1)
          == Transition::Applied);
    CHECK(file.current().draft.text == "header\nleft first\nleft second\nbridge\nright later\ntail\n");
    CHECK(file.current().draft.unresolvedMarkers == 0 && file.markerSections().empty());
    CHECK(file.beginMarkResolved(file.current().id) == Transition::Applied);
}

void acceptAloneOrIgnoringBothSidesKeepsTheMarkerLines()
{
    auto file = test_support::markedFile();
    const auto &left = test_support::choiceTarget(file, SourceSide::Stage2, ChoiceScope::Section);
    const auto accept = previewChoice(file, 1, left.source.contributionId, ChoiceAction::Accept);
    CHECK(accept.note == ChoiceNote::Accept && accept.markers == MarkerEffect::None);
    CHECK(applyChoice(file, accept) == Transition::Applied);
    CHECK(file.current().draft.text == test_support::markedText());
    CHECK(file.current().draft.unresolvedMarkers == 2 && file.markerSections().size() == 2);
    const auto &right = test_support::choiceTarget(file, SourceSide::Stage3, ChoiceScope::Section);
    const auto ignore = previewChoice(file, file.current().id, right.source.contributionId, ChoiceAction::Ignore);
    CHECK(ignore.note == ChoiceNote::Ignore && ignore.markers == MarkerEffect::Removed);
    auto ignored = test_support::markedFile();
    CHECK(choose(ignored, SourceSide::Stage2, ChoiceAction::Ignore) == Transition::Applied);
    CHECK(choose(ignored, SourceSide::Stage3, ChoiceAction::Ignore) == Transition::Applied);
    CHECK(ignored.current().draft.text == test_support::markedText());
    CHECK(ignored.current().draft.unresolvedMarkers == 2 && has(ignored, Blocker::UnresolvedMarkers));
}

void aHandEditedMarkerLineOrphansItsBlock()
{
    for (const auto *line : {"<<<<<<< ours\n", "=======\n", ">>>>>>> theirs\n"}) {
        auto file = test_support::markedFile();
        // Still a marker line, but no longer the one Git wrote.
        const auto at = file.current().draft.text.find(line) + 7;
        CHECK(file.editResult(1, {{at, at}, " edited"}) == Transition::Applied);
        const auto edited = file.current().draft.text;
        CHECK(file.markers().blocks.size() == 2 && file.markerSections().size() == 1);
        CHECK(choose(file, SourceSide::Stage2, ChoiceAction::ResolveUsingSide) == Transition::Applied);
        CHECK(file.current().draft.text == edited && file.current().draft.unresolvedMarkers == 2);
    }
    auto removed = test_support::markedFile();
    CHECK(removed.editResult(1, {removed.markers().blocks.front().separator.bytes, ""}) == Transition::Applied);
    const auto broken = removed.current().draft.text;
    CHECK(removed.markers().orphans.size() == 2 && removed.markerSections().size() == 1);
    CHECK(choose(removed, SourceSide::Stage2, ChoiceAction::ResolveUsingSide) == Transition::Applied);
    CHECK(removed.current().draft.text == broken && removed.current().draft.unresolvedMarkers == 3);
    // Typing inside the side a decision ignores keeps that work and the markers around it.
    auto typed = test_support::markedFile();
    const auto at = typed.current().draft.text.find("right first") + 5;
    CHECK(typed.editResult(1, {{at, at}, " typed"}) == Transition::Applied);
    CHECK(choose(typed, SourceSide::Stage2, ChoiceAction::ResolveUsingSide) == Transition::Applied);
    CHECK(typed.current().draft.text.find("<<<<<<< ours\nleft first\nleft second\n=======\nright typed first\n")
          != std::string::npos);
    CHECK(typed.current().draft.unresolvedMarkers == 2);
}

// The ways of deciding both sections of a split block.
enum class SplitPath { AcceptThenIgnore, ResolveRight, LeftThenRight };

const char *name(SplitPath path)
{
    switch (path) {
    case SplitPath::AcceptThenIgnore: return "accept then ignore";
    case SplitPath::ResolveRight: return "resolve right";
    case SplitPath::LeftThenRight: break;
    }
    return "left then right";
}

void aSplitBlockResolvesWhenAllSectionsAreDecided(SplitPath path)
{
    const auto launch = test_support::splitText();
    auto file = test_support::splitFile();
    const auto sections = file.markerSections();
    const auto &first = test_support::choiceTarget(file, SourceSide::Stage2, ChoiceScope::Section, 0);
    const auto &second = test_support::choiceTarget(file, SourceSide::Stage2, ChoiceScope::Section, 1);
    CHECK(first.section != second.section && file.markers().blocks.size() == 1);
    CHECK(sections.size() == 1 && sections.front().block == file.markers().blocks.front());
    CHECK((sections.front().sections == std::vector<std::string>{first.section, second.section}));
    const auto partly = [&] {
        CHECK(file.current().draft.text == launch && file.current().draft.unresolvedMarkers == 1);
        CHECK(file.markerSections() == sections && has(file, Blocker::UnresolvedMarkers));
    };
    const auto lastPreview = [&](SourceSide side, std::size_t at, ChoiceAction action) {
        return previewChoice(file, file.current().id,
                             test_support::choiceTarget(file, side, ChoiceScope::Section, at).source.contributionId,
                             action);
    };
    std::string expected;
    std::optional<ChoicePreview> last;
    switch (path) {
    case SplitPath::AcceptThenIgnore:
        CHECK(choose(file, SourceSide::Stage2, ChoiceAction::Accept, ChoiceScope::Section, 0) == Transition::Applied);
        CHECK(choose(file, SourceSide::Stage2, ChoiceAction::Accept, ChoiceScope::Section, 1) == Transition::Applied);
        CHECK(choose(file, SourceSide::Stage3, ChoiceAction::Ignore, ChoiceScope::Section, 0) == Transition::Applied);
        partly();
        last = lastPreview(SourceSide::Stage3, 1, ChoiceAction::Ignore);
        CHECK(last->note == ChoiceNote::Ignore && last->markers == MarkerEffect::Removed);
        expected = "header\nleft one\nshared\nleft two\ntail\n";
        break;
    case SplitPath::ResolveRight:
        CHECK(choose(file, SourceSide::Stage3, ChoiceAction::ResolveUsingSide, ChoiceScope::Section, 1)
              == Transition::Applied);
        partly();
        last = lastPreview(SourceSide::Stage3, 0, ChoiceAction::ResolveUsingSide);
        expected = "header\nright one\nshared\nright two\ntail\n";
        break;
    case SplitPath::LeftThenRight:
        CHECK(choose(file, SourceSide::Stage2, ChoiceAction::ResolveUsingSide, ChoiceScope::Section, 0)
              == Transition::Applied);
        partly();
        last = lastPreview(SourceSide::Stage3, 1, ChoiceAction::ResolveUsingSide);
        expected = "header\nleft one\nshared\nright two\ntail\n";
        break;
    }
    CHECK(applyChoice(file, engaged(last)) == Transition::Applied);
    const auto chosen = file.current().draft;
    CHECK(chosen.text == expected && chosen.unresolvedMarkers == 0 && file.markerSections().empty());
    CHECK(!has(file, Blocker::UnresolvedMarkers) && !has(file, Blocker::Undecided));
    // One undo brings back the block, its text and the earlier decisions.
    CHECK(file.undo(file.current().id) == Transition::Applied);
    partly();
    CHECK(file.current().draft.decisions != chosen.decisions);
    CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == chosen);
    CHECK(file.beginMarkResolved(file.current().id) == Transition::Applied);
}

void aSplitBlockWithCrossedSidesResolvesInSectionOrder()
{
    const auto launch = test_support::splitText();
    // The first section keeps the right line and the second the left one, so
    // deleting cannot put them in order and the block is rebuilt.
    auto crossed = test_support::splitFile();
    CHECK(choose(crossed, SourceSide::Stage3, ChoiceAction::ResolveUsingSide, ChoiceScope::Section, 0)
          == Transition::Applied);
    const auto partly = crossed.current();
    CHECK(partly.draft.text == launch && partly.draft.unresolvedMarkers == 1);
    const auto last = previewChoice(
        crossed, crossed.current().id,
        test_support::choiceTarget(crossed, SourceSide::Stage2, ChoiceScope::Section, 1).source.contributionId,
        ChoiceAction::ResolveUsingSide);
    CHECK(last.note == ChoiceNote::ResolveUsingSide && last.markers == MarkerEffect::Removed);
    CHECK(applyChoice(crossed, last) == Transition::Applied);
    const auto chosen = crossed.current().draft;
    CHECK(chosen.text == "header\nright one\nshared\nleft two\ntail\n");
    CHECK(chosen.unresolvedMarkers == 0 && crossed.markerSections().empty());
    CHECK(!has(crossed, Blocker::UnresolvedMarkers) && !has(crossed, Blocker::Undecided));
    // Each section's scopes are bound in the rebuilt text: the kept one to its
    // line, the ignored one empty beside it, left before right.
    const auto range = [&](SourceSide side, std::size_t at) {
        return anchor(crossed,
                      test_support::choiceTarget(crossed, side, ChoiceScope::Section, at).source.contributionId)
            .result;
    };
    CHECK((range(SourceSide::Stage3, 0) == ByteRange{7, 17} && range(SourceSide::Stage2, 0) == ByteRange{7, 7}));
    CHECK((range(SourceSide::Stage2, 1) == ByteRange{24, 33} && range(SourceSide::Stage3, 1) == ByteRange{33, 33}));
    CHECK(crossed.correspondence()->handEditedAnchors().empty());
    // One undo brings back the block and the first section's decisions.
    CHECK(crossed.undo(crossed.current().id) == Transition::Applied);
    CHECK(crossed.current().draft == partly.draft && crossed.markerSections().size() == 1);
    CHECK(crossed.redo(crossed.current().id) == Transition::Applied && crossed.current().draft == chosen);
    CHECK(range(SourceSide::Stage2, 1) == ByteRange{24, 33});
    CHECK(crossed.beginMarkResolved(crossed.current().id) == Transition::Applied);
    // Recovery replays each draft with its hand-edit flags and derives the same text and count.
    auto replayed = test_support::splitFile();
    CHECK(test_support::replay(replayed, partly.draft) == Transition::Applied);
    CHECK(test_support::replay(replayed, chosen) == Transition::Applied);
    CHECK(replayed.current().draft == chosen && replayed.markers().count() == 0);
    CHECK(replayed.beginMarkResolved(replayed.current().id) == Transition::Applied);
}

void aSplitBlockKeepsItsMarkersWhileASectionKeepsNoSideOrSharedTextIsEdited()
{
    const auto launch = test_support::splitText();
    // Ignoring both sides of one section keeps the markers, as for a single section.
    auto ignored = test_support::splitFile();
    CHECK(choose(ignored, SourceSide::Stage2, ChoiceAction::ResolveUsingSide, ChoiceScope::Section, 0)
          == Transition::Applied);
    CHECK(choose(ignored, SourceSide::Stage2, ChoiceAction::Ignore, ChoiceScope::Section, 1) == Transition::Applied);
    CHECK(choose(ignored, SourceSide::Stage3, ChoiceAction::Ignore, ChoiceScope::Section, 1) == Transition::Applied);
    CHECK(ignored.current().draft.text == launch && ignored.current().draft.unresolvedMarkers == 1);
    // Typing in one copy of the shared line unties the block, and that work stays.
    auto typed = test_support::splitFile();
    const auto at = launch.rfind("shared") + 6;
    CHECK(typed.editResult(1, {{at, at}, " typed"}) == Transition::Applied);
    const auto edited = typed.current().draft.text;
    CHECK(typed.markers().blocks.size() == 1 && typed.markerSections().empty());
    CHECK(choose(typed, SourceSide::Stage2, ChoiceAction::ResolveUsingSide, ChoiceScope::Section, 0)
          == Transition::Applied);
    CHECK(choose(typed, SourceSide::Stage2, ChoiceAction::ResolveUsingSide, ChoiceScope::Section, 1)
          == Transition::Applied);
    CHECK(typed.current().draft.text == edited && typed.current().draft.unresolvedMarkers == 1);
    // Take both on a section works, but a single line of one half still cannot
    // be joined to a line of the other half, so that stays refused and the
    // text and decisions stay as they were.
    auto both = test_support::splitFile();
    const auto before = both.current();
    for (const auto side : {SourceSide::Stage2, SourceSide::Stage3})
        for (const std::size_t line : {0U, 1U})
            for (const auto action : {ChoiceAction::CombineLeftFirst, ChoiceAction::CombineRightFirst}) {
                const auto preview = previewChoice(
                    both, both.current().id,
                    test_support::choiceTarget(both, side, ChoiceScope::Line, line).source.contributionId, action);
                CHECK(preview.state == Transition::AmbiguousTarget && preview.command.edits.empty());
                CHECK(applyChoice(both, preview, true) != Transition::Applied);
            }
    CHECK(both.current().draft == before.draft && both.current().id == before.id);
}

// How the section that was not combined is decided.
enum class Other { Left, Right, LeftFirst, RightFirst };

constexpr std::array<const char *, 2> kSplitLeft{"left one\n", "left two\n"};
constexpr std::array<const char *, 2> kSplitRight{"right one\n", "right two\n"};

std::string joined(const char *first, const char *second)
{
    std::string text = first;
    text += second;
    return text;
}

// Decides split section `at` as `other` and returns the text it keeps.
std::string decideOther(FileSession &file, Other other, std::size_t at)
{
    switch (other) {
    case Other::Left:
        CHECK(choose(file, SourceSide::Stage2, ChoiceAction::ResolveUsingSide, ChoiceScope::Section, at)
              == Transition::Applied);
        return kSplitLeft.at(at);
    case Other::Right:
        CHECK(choose(file, SourceSide::Stage3, ChoiceAction::ResolveUsingSide, ChoiceScope::Section, at)
              == Transition::Applied);
        return kSplitRight.at(at);
    case Other::LeftFirst:
        CHECK(choose(file, SourceSide::Stage3, ChoiceAction::CombineLeftFirst, ChoiceScope::Section, at)
              == Transition::Applied);
        return joined(kSplitLeft.at(at), kSplitRight.at(at));
    case Other::RightFirst:
        CHECK(choose(file, SourceSide::Stage2, ChoiceAction::CombineRightFirst, ChoiceScope::Section, at)
              == Transition::Applied);
        return joined(kSplitRight.at(at), kSplitLeft.at(at));
    }
    return std::string{};
}

// Take both on the `side` scope of split section `at`, before or after the
// other section is decided as `other`.
struct SplitTakeBoth {
    bool combineFirst = true;
    std::size_t at = 0;
    SourceSide side = SourceSide::Stage2;
    ChoiceAction action = ChoiceAction::CombineLeftFirst;
    Other other = Other::Left;
    [[nodiscard]] std::size_t rest() const { return std::size_t{1} - at; }
};

Transition takeBoth(FileSession &file, const SplitTakeBoth &step)
{
    const auto &target = test_support::choiceTarget(file, step.side, ChoiceScope::Section, step.at);
    return applyChoice(file, previewChoice(file, file.current().id, target.source.contributionId, step.action), true);
}

// Recovery replays each draft and derives the same text and count, and a
// draft recovered between the two decisions finishes the same way.
void expectSplitRecovery(const SplitTakeBoth &step, const std::vector<MarkerSection> &sections, const Draft &partly,
                         const Draft &chosen, const std::string &rest)
{
    auto replayed = test_support::splitFile();
    CHECK(test_support::replay(replayed, partly) == Transition::Applied);
    CHECK(replayed.markerSections() == sections);
    CHECK(test_support::replay(replayed, chosen) == Transition::Applied);
    CHECK(replayed.current().draft == chosen && replayed.markers().count() == 0);
    CHECK(replayed.beginMarkResolved(replayed.current().id) == Transition::Applied);
    auto resumed = test_support::splitFile();
    CHECK(test_support::replay(resumed, partly) == Transition::Applied);
    if (step.combineFirst)
        CHECK(decideOther(resumed, step.other, step.rest()) == rest);
    else
        CHECK(takeBoth(resumed, step) == Transition::Applied);
    CHECK(resumed.current().draft == chosen);
}

void takeBothOnASplitSection(const SplitTakeBoth &step)
{
    const auto launch = test_support::splitText();
    const bool leftFirst = step.action == ChoiceAction::CombineLeftFirst;
    const auto at = step.at;
    const auto rest = step.rest();
    auto file = test_support::splitFile();
    const auto sections = file.markerSections();
    std::array<std::string, 2> kept;
    if (!step.combineFirst) kept.at(rest) = decideOther(file, step.other, rest);
    const auto earlier = file.current();
    CHECK(earlier.draft.text == launch && file.markerSections() == sections);
    const auto preview = previewChoice(
        file, earlier.id, test_support::choiceTarget(file, step.side, ChoiceScope::Section, at).source.contributionId,
        step.action);
    kept.at(at) =
        leftFirst ? joined(kSplitLeft.at(at), kSplitRight.at(at)) : joined(kSplitRight.at(at), kSplitLeft.at(at));
    CHECK(preview.state == Transition::ConfirmationRequired && preview.replacement == kept.at(at));
    CHECK(preview.markers == (step.combineFirst ? MarkerEffect::KeptUntilDecided : MarkerEffect::Removed));
    // Only this section's two scopes are edited, each with its own side's text,
    // so the shared line and the other section's lines are outside every edit.
    CHECK(preview.command.edits.size() == 2);
    for (const auto &edit : preview.command.edits) {
        CHECK(earlier.draft.text.substr(edit.range.begin, edit.range.end - edit.range.begin) == edit.replacement);
        CHECK((edit.replacement == kSplitLeft.at(at) || edit.replacement == kSplitRight.at(at)));
    }
    CHECK(applyChoice(file, preview) == Transition::ConfirmationRequired && file.current().id == earlier.id);
    CHECK(applyChoice(file, preview, true) == Transition::Applied);
    for (const auto &[combined, order] :
         {std::pair{SourceSide::Stage2, leftFirst ? 1U : 2U}, std::pair{SourceSide::Stage3, leftFirst ? 2U : 1U}})
        for (const auto &id : test_support::choiceTarget(file, combined, ChoiceScope::Section, at).decisions)
            CHECK((file.current().draft.decisions.at(id) == Decision{DecisionValue::Accepted, order}));
    if (step.combineFirst) {
        // The block keeps its markers and text until the other section is decided.
        CHECK(file.current().draft.text == launch && file.current().draft.unresolvedMarkers == 1);
        CHECK(file.markerSections() == sections && has(file, Blocker::UnresolvedMarkers));
    }
    const auto partly = step.combineFirst ? file.current().draft : earlier.draft;
    if (step.combineFirst) kept.at(rest) = decideOther(file, step.other, rest);
    const auto chosen = file.current().draft;
    std::string expected = "header\n";
    expected += kept[0];
    expected += "shared\n";
    expected += kept[1];
    expected += "tail\n";
    CHECK(chosen.text == expected && chosen.unresolvedMarkers == 0 && file.markerSections().empty());
    CHECK(!has(file, Blocker::UnresolvedMarkers) && !has(file, Blocker::Undecided));
    // One undo brings back the block and the first section's decisions.
    CHECK(file.undo(file.current().id) == Transition::Applied);
    CHECK(file.current().draft == partly && file.markerSections() == sections);
    CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == chosen);
    CHECK(file.beginMarkResolved(file.current().id) == Transition::Applied);
    expectSplitRecovery(step, sections, partly, chosen, kept.at(rest));
}

void takeBothOnASectionOfASplitBlockResolvesTheBlockInSectionOrder()
{
    for (const bool combineFirst : {true, false})
        for (const std::size_t at : {0U, 1U})
            for (const auto side : {SourceSide::Stage2, SourceSide::Stage3})
                for (const auto action : {ChoiceAction::CombineLeftFirst, ChoiceAction::CombineRightFirst})
                    for (const auto other : {Other::Left, Other::Right, Other::LeftFirst, Other::RightFirst})
                        takeBothOnASplitSection({combineFirst, at, side, action, other});
    // A combination that would overwrite an unselected contribution is still refused:
    // the right side's second line maps onto the left line its first line also maps to.
    auto overlapping = test_support::choiceFile("header\nleft\ntail\n", "header\nright one\nright two\ntail\n");
    const auto before = overlapping.current();
    const auto refused = previewChoice(
        overlapping, before.id,
        test_support::choiceTarget(overlapping, SourceSide::Stage3, ChoiceScope::Line, 1).source.contributionId,
        ChoiceAction::CombineLeftFirst);
    CHECK(refused.state == Transition::AmbiguousTarget && refused.command.edits.empty());
    CHECK(refused.note == ChoiceNote::ReplacesUnselected);
    CHECK(applyChoice(overlapping, refused, true) != Transition::Applied
          && overlapping.current().draft == before.draft);
}

void theDeletingSideOfAMarkerSectionCanBeAcceptedAndResolved()
{
    const auto launch = test_support::deletionFile().current().draft.text;
    const std::string accepted = "header\n<<<<<<< ours\n=======\n>>>>>>> theirs\nbridge\ntail\n";
    enum class Path { Resolve, AcceptThenIgnore, IgnoreThenAccept };
    for (const auto path : {Path::Resolve, Path::AcceptThenIgnore, Path::IgnoreThenAccept}) {
        auto file = test_support::deletionFile();
        CHECK(file.markerSections().size() == 1);
        const auto &left = test_support::choiceTarget(file, SourceSide::Stage2, ChoiceScope::Section);
        CHECK(left.source.range.begin == left.source.range.end);
        std::string before = launch;
        switch (path) {
        case Path::Resolve: {
            const auto preview = previewChoice(file, 1, left.source.contributionId, ChoiceAction::ResolveUsingSide);
            CHECK(preview.state == Transition::Applied && preview.before == "right first\n"
                  && preview.replacement.empty());
            CHECK(applyChoice(file, preview) == Transition::Applied);
            break;
        }
        case Path::AcceptThenIgnore:
            CHECK(choose(file, SourceSide::Stage2, ChoiceAction::Accept) == Transition::Applied);
            CHECK(file.current().draft.text == accepted && file.current().draft.unresolvedMarkers == 1);
            CHECK(file.markerSections().size() == 1);
            before = accepted;
            CHECK(choose(file, SourceSide::Stage3, ChoiceAction::Ignore) == Transition::Applied);
            break;
        case Path::IgnoreThenAccept:
            CHECK(choose(file, SourceSide::Stage3, ChoiceAction::Ignore) == Transition::Applied);
            CHECK(file.current().draft.text == launch);
            CHECK(choose(file, SourceSide::Stage2, ChoiceAction::Accept) == Transition::Applied);
            break;
        }
        const auto chosen = file.current().draft;
        CHECK(chosen.text == "header\nbridge\ntail\n" && chosen.unresolvedMarkers == 0);
        CHECK(file.markerSections().empty() && !has(file, Blocker::UnresolvedMarkers));
        for (const auto &id : left.decisions) CHECK(chosen.decisions.at(id).value == DecisionValue::Accepted);
        for (const auto &id : file.catalogue().opposite(left)->decisions)
            CHECK(chosen.decisions.at(id).value == DecisionValue::Ignored);
        CHECK(file.undo(file.current().id) == Transition::Applied && file.current().draft.text == before);
        CHECK(file.markerSections().size() == 1);
        CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == chosen);
    }
}

void aDeletionOutsideAMarkerSectionKeepsTheScopeCheck()
{
    // A hand-edited end marker orphans the block, so the deletion
    // anchor after it no longer identifies the competing range.
    auto file = test_support::deletionFile();
    const auto at = file.current().draft.text.find(">>>>>>> theirs") + 7;
    CHECK(file.editResult(1, {{at, at}, " edited"}) == Transition::Applied);
    CHECK(file.markerSections().empty());
    const auto edited = file.current();
    const auto &left = test_support::choiceTarget(file, SourceSide::Stage2, ChoiceScope::Section);
    for (const auto action : {ChoiceAction::Accept, ChoiceAction::ResolveUsingSide}) {
        const auto refused = previewChoice(file, edited.id, left.source.contributionId, action);
        CHECK(refused.state == Transition::AmbiguousTarget);
        CHECK(refused.note == ChoiceNote::UncertainDeletion);
        CHECK(applyChoice(file, refused, true) == Transition::AmbiguousTarget);
        CHECK(file.current().id == edited.id && file.current().draft == edited.draft);
    }
}

void aHeadingUnderlineInTheDiff3BaseIsContent()
{
    auto file = test_support::underlinedBaseFile();
    const auto launch = file.current().draft;
    const auto scan = file.markers();
    CHECK(scan.orphans.empty() && scan.blocks.size() == 1);
    CHECK(engaged(scan.blocks.front().base).line == 5 && scan.blocks.front().separator.line == 9);
    CHECK(launch.unresolvedMarkers == 1 && file.markerSections().size() == 1);
    CHECK(choose(file, SourceSide::Stage2, ChoiceAction::Accept) == Transition::Applied);
    CHECK(file.current().draft.text == launch.text && file.markerSections().size() == 1);
    CHECK(choose(file, SourceSide::Stage3, ChoiceAction::Ignore) == Transition::Applied);
    const auto chosen = file.current().draft;
    CHECK(chosen.text == "intro\nLeft title\n-----------\nleft body\ntail\n");
    CHECK(chosen.unresolvedMarkers == 0 && file.markerSections().empty());
    CHECK(!has(file, Blocker::UnresolvedMarkers) && !has(file, Blocker::Undecided));
    // One undo brings back the whole block with its base text.
    CHECK(file.undo(file.current().id) == Transition::Applied);
    CHECK(file.current().draft.text == launch.text && file.markers() == scan);
    CHECK(file.current().draft.unresolvedMarkers == 1 && file.markerSections().size() == 1);
    CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == chosen);
    CHECK(file.beginMarkResolved(file.current().id) == Transition::Applied);
    // Without the earlier version the underline still takes the separator's place.
    auto plain = test_support::underlinedBaseFile(false);
    CHECK(plain.markers() == scanMarkers(launch.text));
    CHECK(plain.markers().blocks.front().separator.line == 7 && plain.markers().orphans.size() == 1);
    CHECK(plain.current().draft.unresolvedMarkers == 2 && plain.markerSections().empty());
}

void aDiff3BlockWithoutMarkerLikeBaseTextIsAssembledAsBefore()
{
    const std::string left = "intro\nleft line\ntail\n";
    const std::string launch =
        "intro\n<<<<<<< ours\nleft line\n||||||| base\nbase line\n=======\nright line\n>>>>>>> theirs\ntail\n";
    auto file = test_support::diff3File(left, "intro\nright line\ntail\n", "intro\nbase line\ntail\n", launch);
    auto plain = test_support::diff3File(left, "intro\nright line\ntail\n", "intro\nbase line\ntail\n", launch, false);
    CHECK(file.markers() == plain.markers() && file.markers() == scanMarkers(launch));
    CHECK(file.markerSections() == plain.markerSections() && file.markerSections().size() == 1);
    CHECK(choose(file, SourceSide::Stage2, ChoiceAction::ResolveUsingSide) == Transition::Applied);
    CHECK(file.current().draft.text == left && file.current().draft.unresolvedMarkers == 0);
    CHECK(file.beginMarkResolved(file.current().id) == Transition::Applied);
}

void aDiff3BaseEndingInAHeadingUnderlineIsClosedByGitsSeparator(bool deleted)
{
    auto file = test_support::underlineEndedBaseFile(true, deleted);
    const auto launch = file.current().draft;
    const auto scan = file.markers();
    CHECK(scan.orphans.empty() && scan.blocks.size() == 1);
    CHECK(engaged(scan.blocks.front().base).line == 4 && scan.blocks.front().separator.line == 7);
    CHECK(launch.unresolvedMarkers == 1 && file.markerSections().size() == 1);
    CHECK(choose(file, SourceSide::Stage2, ChoiceAction::Accept) == Transition::Applied);
    CHECK(file.current().draft.text == launch.text && file.markerSections().size() == 1);
    CHECK(choose(file, SourceSide::Stage3, ChoiceAction::Ignore) == Transition::Applied);
    const auto chosen = file.current().draft;
    CHECK(chosen.text == "intro\nLeft\n-----\ntail\n");
    CHECK(chosen.unresolvedMarkers == 0 && file.markerSections().empty());
    CHECK(!has(file, Blocker::UnresolvedMarkers) && !has(file, Blocker::Undecided));
    CHECK(file.undo(file.current().id) == Transition::Applied);
    CHECK(file.current().draft.text == launch.text && file.markers() == scan);
    CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == chosen);
    CHECK(file.beginMarkResolved(file.current().id) == Transition::Applied);
    // Without the earlier version the underline still takes the separator's place.
    auto plain = test_support::underlineEndedBaseFile(false, deleted);
    CHECK(plain.markers() == scanMarkers(launch.text));
    CHECK(plain.markers().blocks.front().separator.line == 6 && plain.markers().orphans.size() == 1);
    CHECK(plain.current().draft.unresolvedMarkers == 2 && plain.markerSections().empty());
}

void gitsSeparatorLiesBetweenABaseEndingAndARightSideStartingWithASeparatorLine()
{
    // The base's underline sits directly above Git's separator and the right
    // side's first line directly below it, three identical lines in a row.
    const std::string left = "intro\nLeft\n-----\ntail\n";
    const std::string earlier = "intro\nTitle\n=======\ntail\n";
    const std::string right = "intro\n=======\nRight\ntail\n";
    const auto merged = test_support::gitDiff3(left, earlier, right);
    CHECK(merged
          == "intro\n<<<<<<< ours\nLeft\n-----\n||||||| base\nTitle\n=======\n=======\n=======\nRight\n"
             ">>>>>>> theirs\ntail\n");
    resolvesDiff3Block(left, earlier, right, merged, 7);
}

void gitsSeparatorLiesBetweenABaseEndingInASeparatorLineAndLeftText()
{
    // The right side starts with a line the left side has too, so both copies
    // of it are equally good matches for the right side's line, and the left
    // side's deletion lies between two lines its region shares with the right.
    const std::string left = "intro\nLeft\n-----\ntail\n";
    const std::string earlier = "intro\nTitle\n=======\ntail\n";
    const std::string right = "intro\nLeft\nRight\n-----\ntail\n";
    const auto merged = test_support::gitDiff3(left, earlier, right);
    CHECK(merged
          == "intro\n<<<<<<< ours\nLeft\n-----\n||||||| base\nTitle\n=======\n=======\nLeft\nRight\n-----\n"
             ">>>>>>> theirs\ntail\n");
    resolvesDiff3Block(left, earlier, right, merged, 7);
}
}

int main()
{
    test_support::Suite suite("EditingCommandsTests");
    for (const auto scope : {ChoiceScope::Line, ChoiceScope::Section})
        for (const auto action : {ChoiceAction::CombineLeftFirst, ChoiceAction::CombineRightFirst})
            suite.run(
                titled("confirmed ordered scopes preserve unrelated text and decisions", {name(scope), name(action)}),
                [=] { confirmedOrderedScopesPreserveUnrelatedTextAndDecisions(scope, action); });
    suite.run("partial combinations can be reordered repeatedly without duplicating another section",
              partialCombinationsCanBeReorderedRepeatedly);
    suite.run("overlapping manual work is displayed and cancel stale or altered previews preserve it",
              overlappingManualWorkIsDisplayedAndPreservedByStaleOrAlteredPreviews);
    suite.run("ordered partial combinations preserve unrelated content and manual edits",
              orderedPartialCombinationsPreserveUnrelatedContentAndManualEdits);
    for (const auto scope : {NonConflictingScope::Left, NonConflictingScope::Right, NonConflictingScope::Both})
        suite.run(titled("file batches apply only eligible contributions after confirmation", {name(scope)}),
                  [=] { fileBatchesApplyOnlyEligibleContributionsAfterConfirmation(scope); });
    suite.run("manual overlap and existing decisions are excluded while unrelated edits survive",
              manualOverlapAndExistingDecisionsAreExcludedFromBatches);
    suite.run("a previously ignored eligible contribution stays ignored while the other side is batched",
              aPreviouslyIgnoredContributionStaysIgnoredInABatch);
    suite.run("absent earlier context stale and altered batches never apply",
              absentEarlierContextStaleAndAlteredBatchesNeverApply);
    suite.run("insertions touching a competing changed range stay outstanding",
              insertionsTouchingACompetingChangedRangeStayOutstanding);
    suite.run("ambiguous repeated result passages stay outstanding", ambiguousRepeatedResultPassagesStayOutstanding);
    for (const auto side : {SourceSide::Stage2, SourceSide::Stage3})
        suite.run(titled("whole-file previews describe and replace exactly the selected source", {name(side)}),
                  [=] { wholeFilePreviewsReplaceExactlyTheSelectedSource(side); });
    suite.run("altered or stale whole-file confirmation cannot change the draft",
              alteredOrStaleWholeFileConfirmationCannotChangeTheDraft);
    suite.run("whole-file replacement retains recovered decisions without source scopes",
              wholeFileReplacementRetainsRecoveredDecisionsWithoutSourceScopes);
    suite.run("non-conflicting batches and whole-file replacement undo and redo in action order",
              batchesAndWholeFileReplacementUndoAndRedoInActionOrder);
    for (const auto scope : {ChoiceScope::Line, ChoiceScope::Section})
        suite.run(titled("scoped Accept preserves unrelated edits and restores atomic history", {name(scope)}),
                  [=] { scopedAcceptPreservesUnrelatedEditsAndRestoresAtomicHistory(scope); });
    for (const auto scope : {ChoiceScope::Line, ChoiceScope::Section})
        suite.run(titled("Ignore preserves manual text and competing decisions through undo and redo", {name(scope)}),
                  [=] { ignorePreservesManualTextAndCompetingDecisions(scope); });
    suite.run("Ignore and scoped acceptance preserve unrelated text and decisions",
              ignoreAndScopedAcceptancePreserveUnrelatedTextAndDecisions);
    suite.run("resolve using one side decides only its section and its competitors",
              resolveUsingOneSideDecidesOnlyItsSectionAndItsCompetitors);
    suite.run("resolve using one line leaves the other section and manual edits unchanged",
              resolveUsingOneLineLeavesTheOtherSectionAndManualEditsUnchanged);
    suite.run("stale ambiguous and cancelled commands leave text and decisions untouched",
              staleAmbiguousAndCancelledCommandsLeaveTextAndDecisionsUntouched);
    suite.run("lines inside a multi-line conflict pair by index and keep lines from both sides",
              linesInsideAMultiLineConflictPairByIndex);
    suite.run("surplus lines insert at the end of an uneven block and the last line keeps extra result lines",
              surplusLinesInsertAtTheEndOfAnUnevenBlock);
    suite.run("repeated identical lines in a multi-line conflict stay unmapped with the existing reason",
              repeatedIdenticalLinesInAMultiLineConflictStayUnmapped);
    suite.run("each section can be decided with a different side", eachSectionCanBeDecidedWithADifferentSide);
    suite.run("an accept still refuses accepted text it may replace", anAcceptStillRefusesAcceptedTextItMayReplace);
    suite.run("each full decision removes its section's marker lines in one undoable step",
              eachFullDecisionRemovesItsSectionsMarkerLinesInOneStep);
    suite.run("accept alone or ignoring both sides keeps the marker lines",
              acceptAloneOrIgnoringBothSidesKeepsTheMarkerLines);
    suite.run("a hand-edited marker line orphans its block, which is never removed",
              aHandEditedMarkerLineOrphansItsBlock);
    for (const auto path : {SplitPath::AcceptThenIgnore, SplitPath::ResolveRight, SplitPath::LeftThenRight})
        suite.run(
            titled("a block around two sections split by a shared line resolves when all are decided", {name(path)}),
            [=] { aSplitBlockResolvesWhenAllSectionsAreDecided(path); });
    suite.run("a split block whose sections keep opposite sides out of order resolves in section order",
              aSplitBlockWithCrossedSidesResolvesInSectionOrder);
    suite.run("a split block keeps its markers while a section keeps no side or shared text is edited",
              aSplitBlockKeepsItsMarkersWhileASectionKeepsNoSideOrSharedTextIsEdited);
    suite.run("Take both on a section of a split block resolves the block in section order",
              takeBothOnASectionOfASplitBlockResolvesTheBlockInSectionOrder);
    suite.run("the deleting side of a marker section can be accepted and resolved",
              theDeletingSideOfAMarkerSectionCanBeAcceptedAndResolved);
    suite.run("a deletion outside a marker section keeps the scope check",
              aDeletionOutsideAMarkerSectionKeepsTheScopeCheck);
    suite.run("a heading underline in the diff3 base is content and the block resolves",
              aHeadingUnderlineInTheDiff3BaseIsContent);
    suite.run("a diff3 block without marker-like base text is assembled as before",
              aDiff3BlockWithoutMarkerLikeBaseTextIsAssembledAsBefore);
    for (const bool deleted : {false, true})
        suite.run(titled("a diff3 base ending in a heading underline is closed by Git's separator",
                         {deleted ? "right side deletes the hunk" : "right side changes the hunk"}),
                  [=] { aDiff3BaseEndingInAHeadingUnderlineIsClosedByGitsSeparator(deleted); });
    suite.run("Git's separator lies between a base ending and a right side starting with a separator line",
              gitsSeparatorLiesBetweenABaseEndingAndARightSideStartingWithASeparatorLine);
    suite.run("Git's separator lies between a base ending in a separator line and left text",
              gitsSeparatorLiesBetweenABaseEndingInASeparatorLineAndLeftText);
    return suite.report();
}
