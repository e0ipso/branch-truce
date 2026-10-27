#include "core/EditingCommands.h"
#include "TestHarness.h"
#include "fixtures/ChoiceFile.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace branch_truce;

namespace {
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

std::string show(const std::vector<Passage> &passages)
{
    std::string text;
    for (const auto &passage : passages) {
        text += std::to_string(static_cast<int>(passage.kind)) + ' '
            + std::to_string(passage.source.begin) + '-' + std::to_string(passage.source.end) + ' '
            + std::to_string(passage.result.begin) + '-' + std::to_string(passage.result.end) + '\n';
    }
    return text;
}

void expectPassages(std::string_view source, std::string_view result, const std::vector<Passage> &expected)
{
    ExactLineCorrespondence algorithm;
    const auto actual = algorithm.compare(source, result);
    if (actual != expected) {
        throw std::runtime_error("passages differ\nactual\n" + show(actual) + "expected\n" + show(expected));
    }
    CHECK(actual == expected);
    CHECK(algorithm.compare(source, result) == actual);
    std::size_t sourceEnd = 0;
    std::size_t resultEnd = 0;
    for (const auto &passage : actual) {
        CHECK(passage.source.begin == sourceEnd && passage.result.begin == resultEnd);
        sourceEnd = passage.source.end;
        resultEnd = passage.result.end;
    }
    CHECK(sourceEnd == source.size() && resultEnd == result.size());
}

FileSession mappedFile(std::string source, std::string result)
{
    FileSnapshots captured;
    captured.path = "mapped.txt";
    captured.provenance.sessionId = "anchor-session";
    captured.stage2 = SourceSnapshot{"source-object", "100644", source,
        {source, source, {}}, "source-commit", "Source"};
    captured.launchResult.text = {result, result, {}};
    const auto path = captured.path;
    return FileSession("mapped", path, {result, {{"choice", {}}}, 0},
        Support::Supported, {}, std::move(captured));
}

ResultAnchor anchor(const FileSession &file, const std::string &id = "choice")
{
    const auto *found = file.correspondence() ? file.correspondence()->anchor(id) : nullptr;
    CHECK(found != nullptr);
    return *found;
}

const ChoiceTarget &targetFor(const FileSession &file, SourceSide side, ChoiceScope scope, std::string_view line)
{
    const auto &sideText = side == SourceSide::Stage2 ? file.snapshots()->stage2 : file.snapshots()->stage3;
    const auto text = std::string_view(sideText->worktree.utf8);
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
    for (const auto &id : target.decisions)
        CHECK(file.current().draft.decisions.at(id) == expected);
}

const Decision ignored{DecisionValue::Ignored, {}};
const Decision undecided{};
}

class CorrespondenceCommandFixtures {
public:
    int run()
    {
        test_support::Suite suite("CorrespondenceCommandFixtures");
        suite.run("TRUCE-0030 correspondence partitions are deterministic for equal changed inserted deleted and ambiguous text", [] {
            expectPassages("repeat\nrepeat\n", "repeat\nrepeat\n",
                {{{0, 14}, {0, 14}, PassageKind::Equal}});
            expectPassages("head\nold\nend\n", "head\nnew\nend\n", {
                {{0, 5}, {0, 5}, PassageKind::Equal},
                {{5, 9}, {5, 9}, PassageKind::Changed},
                {{9, 13}, {9, 13}, PassageKind::Equal}});
            expectPassages("head\nend\n", "head\ninserted\nend\n", {
                {{0, 5}, {0, 5}, PassageKind::Equal},
                {{5, 5}, {5, 14}, PassageKind::ResultOnly},
                {{5, 9}, {14, 18}, PassageKind::Equal}});
            expectPassages("head\ndeleted\nend\n", "head\nend\n", {
                {{0, 5}, {0, 5}, PassageKind::Equal},
                {{5, 13}, {5, 5}, PassageKind::SourceOnly},
                {{13, 17}, {5, 9}, PassageKind::Equal}});
            expectPassages("head\nrepeat\nrepeat\nend\n", "head\nrepeat\nend\n", {
                {{0, 5}, {0, 5}, PassageKind::Equal},
                {{5, 19}, {5, 12}, PassageKind::Ambiguous},
                {{19, 23}, {12, 16}, PassageKind::Equal}});
            expectPassages("head\none\ntwo\nend\n", "head\ntwo\none\nend\n", {
                {{0, 5}, {0, 5}, PassageKind::Equal},
                {{5, 13}, {5, 13}, PassageKind::Ambiguous},
                {{13, 17}, {13, 17}, PassageKind::Equal}});
        });
        suite.run("TRUCE-0030 repeated text keeps a selected occurrence and manual edits transform that anchor", [] {
            const std::string source = "repeat\nmiddle\nrepeat\n";
            const std::string result = "manual prefix\n" + source;
            auto file = mappedFile(source, result);
            const auto sources = *file.snapshots();
            const auto passages = ExactLineCorrespondence{}.compare(source, result);
            CHECK(file.correspondence()->passages(SourceSide::Stage2) != nullptr);
            CHECK(*file.correspondence()->passages(SourceSide::Stage2) == passages);
            CHECK(file.bindSourceAnchor(1, {"choice", SourceSide::Stage2, {14, 21}}) == Transition::Applied);
            CHECK(anchor(file).state == AnchorState::Mapped);
            CHECK(anchor(file).result == ByteRange{28, 35});
            CHECK(file.replaceMapped(1, anchor(file), "selected\n", Decision{DecisionValue::Accepted, 1})
                == Transition::Applied);
            CHECK(file.current().draft.text == "manual prefix\nrepeat\nmiddle\nselected\n");
            CHECK(file.current().draft.decisions.at("choice") == Decision{DecisionValue::Accepted, 1});

            const std::string same = "head\nleft\nright\nend\n";
            auto edited = mappedFile(same, same);
            CHECK(edited.bindSourceAnchor(1, {"choice", SourceSide::Stage2, {5, 10}}) == Transition::Applied);
            const auto issued = anchor(edited);
            CHECK(issued.state == AnchorState::Mapped && issued.result == ByteRange{5, 10} && !issued.manualOverlap);
            const std::string prefix = "manual \xc3\xa9\n";
            const std::string suffix = "unrelated manual tail\n";
            CHECK(edited.editResult(1, {{0, 0}, prefix}) == Transition::Applied);
            CHECK(edited.editResult(edited.current().id, {{prefix.size(), prefix.size() + 5}, {}}) == Transition::Applied);
            const auto end = edited.current().draft.text.size();
            CHECK(edited.editResult(edited.current().id, {{end, end}, suffix}) == Transition::Applied);
            const auto current = anchor(edited);
            CHECK(current.result == ByteRange{prefix.size(), prefix.size() + 5} && !current.manualOverlap);
            const auto before = observe(edited);
            CHECK(edited.replaceMapped(edited.current().id, issued, "stale", Decision{DecisionValue::Accepted, 1}, true)
                == Transition::StaleRevision);
            expectUnchanged(edited, before);
            CHECK(edited.replaceMapped(edited.current().id, current, "accepted\n", Decision{DecisionValue::Accepted, 1})
                == Transition::Applied);
            const auto chosen = edited.current().draft;
            CHECK(chosen.text == prefix + "accepted\nright\nend\n" + suffix);
            CHECK(chosen.decisions.at("choice") == Decision{DecisionValue::Accepted, 1});
            const auto chosenRange = anchor(edited).result;
            CHECK(edited.undo(edited.current().id) == Transition::Applied);
            CHECK(edited.current().draft == before.draft);
            CHECK(anchor(edited).result == ByteRange{prefix.size(), prefix.size() + 5});
            CHECK(anchor(edited).revision == edited.current().id);
            CHECK(edited.redo(edited.current().id) == Transition::Applied);
            CHECK(edited.current().draft == chosen && anchor(edited).result == chosenRange);
            CHECK(anchor(edited).revision == edited.current().id);
            CHECK(edited.replaceMapped(edited.current().id, issued, "stale", {}, true) == Transition::StaleRevision);
            CHECK(edited.current().draft == chosen);
            CHECK(*file.snapshots() == sources);
            CHECK(*edited.snapshots() == *mappedFile(same, same).snapshots());
        });
        suite.run("TRUCE-0030 stale ambiguous and cancelled commands leave text and decisions untouched", [] {
            auto ambiguous = test_support::choiceFile("head\nrepeat\nrepeat\ntail\n",
                "head\nchanged\nrepeat\ntail\n", "head\nrepeat\ntail\n");
            const auto ambiguousSources = *ambiguous.snapshots();
            int repeats = 0;
            for (const auto &target : ambiguous.choices()) {
                if (target.scope != ChoiceScope::Line || target.source.side != SourceSide::Stage2) continue;
                const auto text = std::string_view(ambiguous.snapshots()->stage2->worktree.utf8);
                const auto line = text.substr(target.source.range.begin,
                    target.source.range.end - target.source.range.begin);
                if (line != "repeat\n") continue;
                ++repeats;
                const auto before = observe(ambiguous);
                const auto rejected = previewChoice(ambiguous, before.id, target.source.contributionId, ChoiceAction::Accept);
                CHECK(rejected.state == Transition::AmbiguousTarget);
                CHECK(applyChoice(ambiguous, rejected, true) == Transition::AmbiguousTarget);
                expectUnchanged(ambiguous, before);
            }
            CHECK(repeats == 2);
            CHECK(*ambiguous.snapshots() == ambiguousSources);

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
            const auto combined = previewChoice(combination, combination.current().id,
                first.source.contributionId, ChoiceAction::CombineRightFirst);
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
            CHECK(wholePreview.replacement == whole.snapshots()->stage3->worktree.utf8);
            CHECK(wholePreview.replacement != wholeBefore.draft.text);
            CHECK(applyWholeFile(whole, wholePreview) == Transition::ConfirmationRequired);
            expectUnchanged(whole, wholeBefore);
        });
        suite.run("TRUCE-0030 Ignore and scoped acceptance preserve unrelated text and decisions", [] {
            const std::string prefix = "manual prefix\n";
            const std::string original = "header\nleft first\nleft second\nbridge\nleft later\ntail\n";
            auto file = test_support::choiceFile();
            const auto sources = *file.snapshots();
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

            const auto acceptPresent = previewChoice(file, file.current().id,
                leftFirst.source.contributionId, ChoiceAction::Accept);
            CHECK(acceptPresent.state == Transition::Applied);
            CHECK(applyChoice(file, acceptPresent) == Transition::Applied);
            CHECK(file.current().draft.text == prefix + original);
            expectDecision(file, leftFirst, Decision{DecisionValue::Accepted, {}});
            expectDecision(file, leftSecond, undecided);
            expectDecision(file, rightFirst, undecided);
            expectDecision(file, rightSecond, undecided);
            expectDecision(file, leftLater, undecided);
            expectDecision(file, rightLater, undecided);
            steps.push_back(file.current().draft);

            const auto acceptOther = previewChoice(file, file.current().id,
                rightLater.source.contributionId, ChoiceAction::Accept);
            CHECK(acceptOther.state == Transition::ConfirmationRequired);
            CHECK(acceptOther.replacement == "right later\n");
            CHECK(applyChoice(file, acceptOther) == Transition::ConfirmationRequired);
            expectCurrent(file, steps.back());
            CHECK(applyChoice(file, acceptOther, true) == Transition::Applied);
            CHECK(file.current().draft.text == prefix + "header\nleft first\nleft second\nbridge\nright later\ntail\n");
            expectDecision(file, leftFirst, Decision{DecisionValue::Accepted, {}});
            expectDecision(file, rightLater, Decision{DecisionValue::Accepted, {}});
            expectDecision(file, leftLater, undecided);
            expectDecision(file, leftSecond, undecided);
            expectDecision(file, rightFirst, undecided);
            expectDecision(file, rightSecond, undecided);
            steps.push_back(file.current().draft);

            const auto ignore = previewChoice(file, file.current().id,
                leftSecond.source.contributionId, ChoiceAction::Ignore);
            CHECK(ignore.state == Transition::Applied);
            CHECK(applyChoice(file, ignore) == Transition::Applied);
            CHECK(file.current().draft.text == steps.back().text);
            expectDecision(file, leftSecond, ignored);
            expectDecision(file, leftFirst, Decision{DecisionValue::Accepted, {}});
            expectDecision(file, rightLater, Decision{DecisionValue::Accepted, {}});
            expectDecision(file, leftLater, undecided);
            expectDecision(file, rightFirst, undecided);
            expectDecision(file, rightSecond, undecided);
            steps.push_back(file.current().draft);
            replay(file, steps);
            CHECK(*file.snapshots() == sources);
        });
        suite.run("TRUCE-0171 lines inside a multi-line conflict pair by index and keep lines from both sides", [] {
            auto file = test_support::commonVersionFile();
            const auto sources = *file.snapshots();
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
                return file.current().draft.text.substr(found.result->begin, found.result->end - found.result->begin);
            };
            // Each line maps to the result line at its own index, and replacing
            // that launch text needs confirmation. The section keeps the whole block.
            for (const auto &[target, expected] : {std::pair{leftOne, "base one\n"}, {leftTwo, "base two\n"},
                     {leftThree, "base three\n"}, {rightOne, "base one\n"}, {rightTwo, "base two\n"},
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
            for (const auto &other : {leftOne, leftThree, rightOne, rightTwo, rightThree}) expectDecision(file, other, undecided);
            // The competing line follows the text that replaced its own.
            CHECK(mapped(rightTwo) == "left two\n");
            steps.push_back(file.current().draft);

            const auto acceptRight = previewChoice(file, file.current().id, rightThree.source.contributionId, ChoiceAction::Accept);
            CHECK(acceptRight.state == Transition::ConfirmationRequired);
            CHECK(acceptRight.before == "base three\n" && acceptRight.replacement == "right three\n");
            CHECK(applyChoice(file, acceptRight, true) == Transition::Applied);
            CHECK(file.current().draft.text == "header\nbase one\nleft two\nright three\ntail\n");
            expectDecision(file, leftTwo, Decision{DecisionValue::Accepted, {}});
            expectDecision(file, rightThree, Decision{DecisionValue::Accepted, {}});
            for (const auto &other : {leftOne, leftThree, rightOne, rightTwo}) expectDecision(file, other, undecided);
            steps.push_back(file.current().draft);

            const auto ignore = previewChoice(file, file.current().id, leftOne.source.contributionId, ChoiceAction::Ignore);
            CHECK(ignore.state == Transition::Applied);
            CHECK(applyChoice(file, ignore) == Transition::Applied);
            CHECK(file.current().draft.text == steps.back().text);
            expectDecision(file, leftOne, ignored);
            expectDecision(file, rightOne, undecided);
            steps.push_back(file.current().draft);
            // The block now holds a line from each side and matches neither source.
            CHECK(file.current().draft.text != sources.stage2->worktree.utf8);
            CHECK(file.current().draft.text != sources.stage3->worktree.utf8);
            replay(file, steps);
            CHECK(*file.snapshots() == sources);
        });
        suite.run("TRUCE-0171 surplus lines insert at the end of an uneven block and the last line keeps extra result lines", [] {
            auto file = test_support::commonVersionFile("header\nl1\nl2\nl3\nl4\ntail\n", "header\nr1\nr2\ntail\n",
                "header\nb1\nb2\nb3\ntail\n");
            const auto sources = *file.snapshots();
            const auto l1 = targetFor(file, SourceSide::Stage2, ChoiceScope::Line, "l1\n");
            const auto l2 = targetFor(file, SourceSide::Stage2, ChoiceScope::Line, "l2\n");
            const auto l3 = targetFor(file, SourceSide::Stage2, ChoiceScope::Line, "l3\n");
            const auto l4 = targetFor(file, SourceSide::Stage2, ChoiceScope::Line, "l4\n");
            const auto r1 = targetFor(file, SourceSide::Stage3, ChoiceScope::Line, "r1\n");
            const auto r2 = targetFor(file, SourceSide::Stage3, ChoiceScope::Line, "r2\n");
            const auto range = [&](const ChoiceTarget &target) {
                const auto found = anchor(file, target.source.contributionId);
                CHECK(found.state == AnchorState::Mapped && found.result.has_value());
                return *found.result;
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
                const auto preview = previewChoice(file, file.current().id, target->source.contributionId, ChoiceAction::Accept);
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
            for (const auto &other : {l2, l3, r1}) expectDecision(file, other, undecided);
            steps.push_back(file.current().draft);
            replay(file, steps);
            CHECK(*file.snapshots() == sources);

            // Several surplus lines accepted in source order keep that order.
            auto surplus = test_support::commonVersionFile("header\nl1\nl2\nl3\ntail\n", "header\nr1\ntail\n", "header\nb1\ntail\n");
            for (const auto *text : {"l2\n", "l3\n"}) {
                const auto target = targetFor(surplus, SourceSide::Stage2, ChoiceScope::Line, text);
                const auto preview = previewChoice(surplus, surplus.current().id, target.source.contributionId, ChoiceAction::Accept);
                CHECK(preview.state == Transition::Applied);
                CHECK(applyChoice(surplus, preview) == Transition::Applied);
            }
            CHECK(surplus.current().draft.text == "header\nb1\nl2\nl3\ntail\n");
        });
        suite.run("TRUCE-0171 repeated identical lines in a multi-line conflict stay unmapped with the existing reason", [] {
            auto file = test_support::commonVersionFile("head\nrepeat\nrepeat\nmid\nleft one\nleft two\ntail\n",
                "head\nright\ntail\n", "head\nrepeat\nmid\nbase one\nbase two\ntail\n");
            const auto sources = *file.snapshots();
            const auto text = std::string_view(file.snapshots()->stage2->worktree.utf8);
            const ChoiceTarget *section = nullptr;
            int repeats = 0;
            for (const auto &target : file.choices()) {
                if (target.source.side != SourceSide::Stage2) continue;
                if (target.scope == ChoiceScope::Section) { section = &target; continue; }
                if (text.substr(target.source.range.begin, target.source.range.end - target.source.range.begin) != "repeat\n")
                    continue;
                ++repeats;
                CHECK(anchor(file, target.source.contributionId).state == AnchorState::Ambiguous);
                for (const auto action : {ChoiceAction::Accept, ChoiceAction::Ignore}) {
                    const auto before = observe(file);
                    const auto rejected = previewChoice(file, before.id, target.source.contributionId, action);
                    CHECK(rejected.state == Transition::AmbiguousTarget);
                    CHECK(rejected.detail == "This scope cannot be mapped safely. Edit the result directly.");
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
            CHECK(*file.snapshots() == sources);
        });
        suite.run("TRUCE-0030 ordered partial combinations preserve unrelated content and manual edits", [] {
            const std::string prefix = "manual prefix\n";
            auto file = test_support::combinationFile(ChoiceScope::Line);
            const auto sources = *file.snapshots();
            const auto leftFirst = targetFor(file, SourceSide::Stage2, ChoiceScope::Line, "left first\n");
            const auto rightFirst = targetFor(file, SourceSide::Stage3, ChoiceScope::Line, "right first\n");
            const auto leftLater = targetFor(file, SourceSide::Stage2, ChoiceScope::Line, "left later\n");
            const auto rightLater = targetFor(file, SourceSide::Stage3, ChoiceScope::Line, "right later\n");
            CHECK(file.current().draft.decisions.size() == 4);
            std::vector<Draft> steps{file.current().draft};
            CHECK(file.editResult(1, {{0, 0}, prefix}) == Transition::Applied);
            steps.push_back(file.current().draft);
            const auto ignore = previewChoice(file, file.current().id,
                leftLater.source.contributionId, ChoiceAction::Ignore);
            CHECK(ignore.state == Transition::Applied);
            CHECK(applyChoice(file, ignore) == Transition::Applied);
            CHECK(file.current().draft.text == prefix + "header\nleft first\nbridge\nleft later\ntail\n");
            expectDecision(file, leftLater, ignored);
            expectDecision(file, rightLater, undecided);
            expectDecision(file, leftFirst, undecided);
            expectDecision(file, rightFirst, undecided);
            steps.push_back(file.current().draft);

            const auto preview = previewChoice(file, file.current().id,
                leftFirst.source.contributionId, ChoiceAction::CombineRightFirst);
            CHECK(preview.state == Transition::ConfirmationRequired);
            CHECK(preview.replacement == "right first\nleft first\n");
            CHECK(preview.sourceOrder.size() == 2);
            CHECK(preview.sourceOrder[0].side == SourceSide::Stage3);
            CHECK(preview.sourceOrder[1].side == SourceSide::Stage2);
            CHECK(applyChoice(file, preview) == Transition::ConfirmationRequired);
            expectCurrent(file, steps.back());
            CHECK(applyChoice(file, preview, true) == Transition::Applied);
            CHECK(file.current().draft.text
                == prefix + "header\nright first\nleft first\nbridge\nleft later\ntail\n");
            expectDecision(file, rightFirst, Decision{DecisionValue::Accepted, 1});
            expectDecision(file, leftFirst, Decision{DecisionValue::Accepted, 2});
            expectDecision(file, leftLater, ignored);
            expectDecision(file, rightLater, undecided);
            steps.push_back(file.current().draft);
            replay(file, steps);
            CHECK(*file.snapshots() == sources);
        });
        suite.run("TRUCE-0030 non-conflicting batches and whole-file replacement undo and redo in action order", [] {
            const std::string prefix = "manual prefix\n";
            const std::string earlier = "header\nbase left\nbridge one\nbase right\nbridge two\nbase conflict\ntail\n";
            const std::string batched = prefix + "header\nleft only\nbridge one\nright only\nbridge two\nbase conflict\ntail\n";
            auto file = test_support::batchFile();
            const auto sources = *file.snapshots();
            const auto conflict = targetFor(file, SourceSide::Stage2, ChoiceScope::Section, "left conflict\n");
            std::vector<Draft> steps{file.current().draft};
            CHECK(steps.back().text == earlier);
            CHECK(file.editResult(1, {{0, 0}, prefix}) == Transition::Applied);
            CHECK(file.current().draft.text == prefix + earlier);
            steps.push_back(file.current().draft);

            const auto ignore = previewChoice(file, file.current().id,
                conflict.source.contributionId, ChoiceAction::Ignore);
            CHECK(ignore.state == Transition::Applied);
            CHECK(applyChoice(file, ignore) == Transition::Applied);
            CHECK(file.current().draft.text == steps.back().text);
            expectDecision(file, conflict, ignored);
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
                if (batch.command.decisions.contains(id)) CHECK(value == Decision{DecisionValue::Accepted, {}});
                else CHECK(value == ignoredDraft.decisions.at(id));
            }
            expectDecision(file, conflict, ignored);
            steps.push_back(file.current().draft);

            const auto whole = previewWholeFile(file, file.current().id, SourceSide::Stage3);
            CHECK(whole.state == Transition::ConfirmationRequired);
            CHECK(whole.replacement == file.snapshots()->stage3->worktree.utf8);
            CHECK(applyWholeFile(file, whole, true) == Transition::Applied);
            CHECK(file.current().draft.text == file.snapshots()->stage3->worktree.utf8);
            CHECK(file.current().draft.text == "header\nbase left\nbridge one\nright only\nbridge two\nright conflict\ntail\n");
            for (const auto &contribution : whole.contributions) {
                const auto expected = Decision{contribution.source.side == SourceSide::Stage3
                    ? DecisionValue::Accepted : DecisionValue::Ignored, {}};
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
            expectDecision(file, conflict, ignored);
            CHECK(*file.snapshots() == sources);
        });
        return suite.report();
    }
};

int main()
{
    return CorrespondenceCommandFixtures{}.run();
}
