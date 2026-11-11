#include "core/EditingAssertions.h"

namespace {
void aPreviewCannotBeAppliedToAnotherCapture()
{
    auto file = test_support::choiceFile();
    const auto &target = test_support::choiceTarget(file, SourceSide::Stage2, ChoiceScope::Section);
    const auto preview = previewChoice(file, 1, target.source.contributionId, ChoiceAction::Ignore);
    auto sources = *file.snapshots();
    sources.provenance.sessionId = "other-session";
    FileSession other(file.id(), file.path(), file.current().draft, {.snapshots = std::move(sources)});
    CHECK(applyChoice(other, preview) == Transition::PreviewChanged);
    CHECK(other.current().id == 1 && other.current().draft == file.current().draft);
}

void confirmedOrderedScopesPreserveUnrelatedTextAndDecisions(ChoiceScope scope, ChoiceAction action)
{
    auto file = test_support::combinationFile(scope);
    const auto target = test_support::choiceTarget(file, SourceSide::Stage2, scope);
    CHECK(file.editResult(1, {{0, 0}, "manual prefix\n"}) == Transition::Applied);
    const auto later = test_support::choiceTarget(file, SourceSide::Stage2, ChoiceScope::Section, 1);
    CHECK(choose(file, SourceSide::Stage2, ChoiceAction::Ignore, ChoiceScope::Section, 1) == Transition::Applied);
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
    expectDecision(file, later, kIgnored);
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
    CHECK(applyChoice(file, altered, true) == Transition::PreviewChanged);
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

}

int main()
{
    test_support::Suite suite("CombinationTests");
    suite.run("a preview cannot be applied to another capture", aPreviewCannotBeAppliedToAnotherCapture);
    for (const auto scope : {ChoiceScope::Line, ChoiceScope::Section})
        for (const auto action : {ChoiceAction::CombineLeftFirst, ChoiceAction::CombineRightFirst})
            suite.run(
                titled("confirmed ordered scopes preserve unrelated text and decisions", {name(scope), name(action)}),
                [=] { confirmedOrderedScopesPreserveUnrelatedTextAndDecisions(scope, action); });
    suite.run("partial combinations can be reordered repeatedly without duplicating another section",
              partialCombinationsCanBeReorderedRepeatedly);
    suite.run("overlapping manual work is displayed and cancel stale or altered previews preserve it",
              overlappingManualWorkIsDisplayedAndPreservedByStaleOrAlteredPreviews);
    return suite.report();
}
