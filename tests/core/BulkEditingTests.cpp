#include "core/EditingAssertions.h"
#include "core/internal/ComputationInput.h"

namespace {
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
    for (const auto &change : preview.changes)
        for (const auto &source : change.sources)
            for (const auto &id : file.catalogue().find(source.contributionId)->decisions)
                CHECK(chosen.decisions.at(id) == Decision{DecisionValue::Accepted, {}});
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
    CHECK(applyNonConflicting(file, altered, true) == Transition::PreviewChanged && file.current().id == 1);
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
    FileSession file("choices", captured.path, {earlier, {}, 0}, {.snapshots = captured, .initializeChoices = true});
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
    FileSession file("choices", source.path(), {earlier + earlier, {}, 0},
                     {.snapshots = captured, .initializeChoices = true});
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
    for (const int field : {0, 1, 2}) {
        auto altered = preview;
        if (field == 0) altered.replacement += "hidden text\n";
        if (field == 1) altered.before = "different displayed result";
        if (field == 2) altered.contributions.front().after = {DecisionValue::Undecided, {}};
        CHECK(applyWholeFile(file, altered, true) == Transition::PreviewChanged);
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
    FileSession file(source.id(), source.path(), legacy, {.snapshots = captured});
    const auto preview = previewWholeFile(file, 1, SourceSide::Stage3);
    CHECK(preview.state == Transition::ConfirmationRequired && preview.retainedDecisions == legacy.decisions.size());
    CHECK(preview.contributions.empty());
    CHECK(applyWholeFile(file, preview, true) == Transition::Applied);
    CHECK(file.current().draft.text == capturedSide(source, SourceSide::Stage3).worktree.utf8);
    CHECK(file.current().draft.decisions == legacy.decisions);
    CHECK(file.undo(file.current().id) == Transition::Applied && file.current().draft == legacy);
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

    const auto batch = previewNonConflicting(file, file.current().id, NonConflictingScope::Both);
    CHECK(batch.state == Transition::ConfirmationRequired);
    CHECK(batch.changes.size() == 2);
    CHECK(batch.changes[0].replacement == "left only\n");
    CHECK(batch.changes[1].replacement == "right only\n");
    CHECK(applyNonConflicting(file, batch, true) == Transition::Applied);
    CHECK(file.current().draft.text == batched);
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

}

int main()
{
    test_support::Suite suite("BulkEditingTests");
    suite.run("batch admission refuses aggregate work without partial changes", [] {
        auto file = test_support::batchFile();
        const detail::ComputationInput input(file);
        for (const auto limits : {detail::BatchLimits{0, 33554432, 4000000}, detail::BatchLimits{8192, 1, 4000000},
                                  detail::BatchLimits{8192, 33554432, 0}}) {
            const auto refused = detail::planNonConflicting(input.planning(), input.targets, input.revision.id,
                                                            NonConflictingScope::Both, limits);
            CHECK(refused.state == Transition::ResourceLimit && refused.changes.empty());
            CHECK(refused.command().edits.empty() && refused.command().decisions.empty());
        }
        std::vector<NonConflictingTarget> many(8193, input.targets.front());
        CHECK(detail::planNonConflicting(input.planning(), many, 1, NonConflictingScope::Both).state
              == Transition::ResourceLimit);
        auto identifiers = input.targets;
        identifiers.front().source.contributionId.assign(4096, 'x');
        const auto metadata = detail::planNonConflicting(input.planning(), identifiers, 1, NonConflictingScope::Both,
                                                         {8192, 8192, 4000000});
        CHECK(metadata.state == Transition::ResourceLimit && metadata.changes.empty());
        many.resize(1024);
        const auto indexed = detail::planNonConflicting(input.planning(), many, 1, NonConflictingScope::Both);
        CHECK(indexed.state == Transition::ConfirmationRequired && indexed.changes.size() == 1);
        CHECK(indexed.changes.front().sources.size() == many.size() && indexed.skipped == 0);
        CHECK(file.current().id == 1);
    });
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
    return suite.report();
}
