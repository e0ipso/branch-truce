#include "core/EditingAssertions.h"

namespace {
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
    CHECK(applyChoice(combination, altered, true) == Transition::PreviewChanged);
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

}

int main()
{
    test_support::Suite suite("EditingCommandsTests");
    for (const auto scope : {ChoiceScope::Line, ChoiceScope::Section})
        suite.run(titled("scoped Accept preserves unrelated edits and restores atomic history", {name(scope)}),
                  [=] { scopedAcceptPreservesUnrelatedEditsAndRestoresAtomicHistory(scope); });
    for (const auto scope : {ChoiceScope::Line, ChoiceScope::Section})
        suite.run(titled("Ignore preserves manual text and competing decisions through undo and redo", {name(scope)}),
                  [=] { ignorePreservesManualTextAndCompetingDecisions(scope); });
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
    return suite.report();
}
