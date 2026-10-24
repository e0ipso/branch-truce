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
