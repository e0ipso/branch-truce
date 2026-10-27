#include "core/ResourceLimits.h"
#include "core/SessionState.h"
#include "TestHarness.h"
#include "fixtures/ChoiceFile.h"
#include <algorithm>
#include <array>
#include <new>

namespace allocation_fixture {
inline bool bounded = false;
inline std::size_t largest = 0;
constexpr std::size_t ceiling = 1024 * 1024;
struct Bound {
    Bound() { largest = 0; bounded = true; }
    ~Bound() { bounded = false; }
};
}

extern "C" void *__real__Znwm(std::size_t size);
extern "C" void *__wrap__Znwm(std::size_t size)
{
    if (allocation_fixture::bounded) {
        allocation_fixture::largest = std::max(allocation_fixture::largest, size);
        if (size > allocation_fixture::ceiling) throw std::bad_alloc();
    }
    return __real__Znwm(size);
}

using namespace branch_truce;

namespace {
Draft draft(std::string text = "result\n")
{
    return {std::move(text), {{"left", {DecisionValue::Accepted, 1}},
                             {"right", {DecisionValue::Ignored, {}}}}, 0};
}
FileSnapshots capturedSources()
{
    FileSnapshots captured;
    captured.path = ":literal\tfile.txt";
    captured.provenance = {"session", "/worktree", "/git/worktrees/session",
        SnapshotProvenance::Operation::Rebase, std::string(64, '2'), std::string(64, '3'),
        std::string(64, '1'), "refs/heads/topic", "original-head", "onto"};
    captured.conversion.attributes = {{"text", "set"}, {"eol", "crlf"}};
    captured.conversion.configuration = {{"core.autocrlf", "false"}, {"core.eol", "crlf"}};
    captured.resultMode = "100755";
    captured.earlier = SourceSnapshot{"base-blob", "100644", "base\n",
        {"base\n", "base\r\n", {SnapshotFormat::Encoding::Utf8, false, SnapshotFormat::Newline::CrLf, true}},
        "base-commit", "Common earlier version"};
    captured.stage2 = SourceSnapshot{"left-blob", "100755", "main\n",
        {"main\n", "\xef\xbb\xbfmain\r\n", {SnapshotFormat::Encoding::Utf8, true, SnapshotFormat::Newline::CrLf, true}},
        "left-commit", "Current rebased state"};
    captured.stage3 = SourceSnapshot{"right-blob", "100755", "side\n",
        {"side\n", std::string("\0s\0i\0d\0e\0\r\0\n", 12),
            {SnapshotFormat::Encoding::Utf16BigEndian, false, SnapshotFormat::Newline::CrLf, true}},
        "right-commit", "Commit being replayed"};
    captured.launchResult = {{"manual\nkept", "\xef\xbb\xbfmanual\r\nkept",
        {SnapshotFormat::Encoding::Utf8, true, SnapshotFormat::Newline::CrLf, false}},
        {11, 22, 0100755, 15, 33, 44, 55, 66}};
    return captured;
}
FileSession mappedFile(std::string source, std::string result)
{
    FileSnapshots captured;
    captured.path = "mapped.txt";
    captured.provenance.sessionId = "anchor-session";
    captured.stage2 = SourceSnapshot{"source-object", "100644", source,
        {source, source, {}}, "source-commit", "Source"};
    captured.launchResult.text = {result, result, {}};
    return FileSession("mapped", captured.path, {result, {{"choice", {}}}, 0},
        Support::Supported, {}, captured);
}
ResultAnchor anchor(const FileSession &file, const std::string &id = "choice")
{
    CHECK(file.correspondence() && file.correspondence()->anchor(id));
    return *file.correspondence()->anchor(id);
}
bool has(const FileSession &file, Blocker blocker)
{
    const auto blockers = file.facts().blockers;
    return std::find(blockers.begin(), blockers.end(), blocker) != blockers.end();
}
Transition choose(FileSession &file, SourceSide side, ChoiceAction action,
                  ChoiceScope scope = ChoiceScope::Section, std::size_t occurrence = 0)
{
    const auto &target = test_support::choiceTarget(file, side, scope, occurrence);
    return applyChoice(file, previewChoice(file, file.current().id, target.source.contributionId, action), true);
}
void resolve(FileSession &file)
{
    const auto revision = file.current().id;
    CHECK(file.beginMarkResolved(revision) == Transition::Applied);
    CHECK(file.recordCheckpoint(revision, Outcome::Succeeded) == Transition::Applied);
    CHECK(file.recordWrite(revision, Outcome::Succeeded) == Transition::Applied);
    CHECK(file.recordStage(revision, Outcome::Succeeded, "verified-object", "100644") == Transition::Applied);
    CHECK(file.facts().resolved);
    CHECK(file.facts().blockers.empty());
}
}

class SessionStateTests {
public:
    int run()
    {
        test_support::Suite suite("SessionStateTests");
        for (const auto scope : {ChoiceScope::Line, ChoiceScope::Section}) {
            for (const auto action : {ChoiceAction::CombineLeftFirst, ChoiceAction::CombineRightFirst}) {
                suite.run("TRUCE-0017 confirmed ordered scopes preserve unrelated text and decisions "
                    + std::to_string(static_cast<int>(scope)) + "/" + std::to_string(static_cast<int>(action)), [=] {
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
                    text.replace(preview.affectedRange->begin,
                        preview.affectedRange->end - preview.affectedRange->begin, expected);
                    const auto chosen = file.current().draft;
                    CHECK(chosen.text == text && chosen.text.starts_with("manual prefix\nheader\n"));
                    CHECK(chosen.text.ends_with("bridge\nleft later\ntail\n"));
                    for (const auto &[id, value] : chosen.decisions) {
                        const auto expectedDecision = preview.command.decisions.find(id);
                        CHECK(value == (expectedDecision == preview.command.decisions.end()
                            ? before.draft.decisions.at(id) : expectedDecision->second));
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
                });
            }
        }
        suite.run("TRUCE-0017 partial combinations can be reordered repeatedly without duplicating another section", [] {
            auto file = test_support::choiceFile();
            const auto target = test_support::choiceTarget(file, SourceSide::Stage2, ChoiceScope::Section, 1);
            for (const auto action : {ChoiceAction::CombineLeftFirst, ChoiceAction::CombineRightFirst,
                     ChoiceAction::CombineLeftFirst}) {
                const auto preview = previewChoice(file, file.current().id, target.source.contributionId, action);
                CHECK(preview.state == Transition::ConfirmationRequired);
                CHECK(applyChoice(file, preview, true) == Transition::Applied);
                const auto suffix = action == ChoiceAction::CombineLeftFirst
                    ? "left later\nright later\ntail\n" : "right later\nleft later\ntail\n";
                CHECK(file.current().draft.text == std::string("header\nleft first\nleft second\nbridge\n") + suffix);
            }
        });
        suite.run("TRUCE-0017 overlapping manual work is displayed and cancel stale or altered previews preserve it", [] {
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
        });
        for (const auto scope : {NonConflictingScope::Left, NonConflictingScope::Right, NonConflictingScope::Both}) {
            suite.run("TRUCE-0018 file batches apply only eligible contributions after confirmation "
                + std::to_string(static_cast<int>(scope)), [=] {
                auto file = test_support::batchFile();
                CHECK(file.editResult(1, {{0, 0}, "manual prefix\n"}) == Transition::Applied);
                const auto before = file.current();
                const auto preview = previewNonConflicting(file, before.id, scope);
                CHECK(preview.state == Transition::ConfirmationRequired && preview.scope == scope && preview.path == file.path());
                CHECK(preview.detail.find("in this file") != std::string::npos);
                CHECK(preview.changes.size() == (scope == NonConflictingScope::Both ? 2 : 1));
                CHECK(applyNonConflicting(file, preview) == Transition::ConfirmationRequired);
                CHECK(file.current().id == before.id && file.current().draft == before.draft);
                CHECK(applyNonConflicting(file, preview, true) == Transition::Applied);
                const auto chosen = file.current().draft;
                const auto left = scope == NonConflictingScope::Right ? "base left\n" : "left only\n";
                const auto right = scope == NonConflictingScope::Left ? "base right\n" : "right only\n";
                CHECK(chosen.text == std::string("manual prefix\nheader\n") + left + "bridge one\n" + right
                    + "bridge two\nbase conflict\ntail\n");
                CHECK(!preview.command.decisions.empty());
                for (const auto &[id, value] : chosen.decisions)
                    CHECK(value == (preview.command.decisions.contains(id)
                        ? Decision{DecisionValue::Accepted, {}} : before.draft.decisions.at(id)));
                const auto competing = test_support::choiceTarget(file, SourceSide::Stage2, ChoiceScope::Section, 2);
                for (const auto &id : competing.decisions) CHECK(chosen.decisions.at(id).value == DecisionValue::Undecided);
                CHECK(file.undo(file.current().id) == Transition::Applied && file.current().draft == before.draft);
                CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == chosen);
                CHECK(previewNonConflicting(file, file.current().id, scope).state == Transition::NoChange);
            });
        }
        suite.run("TRUCE-0018 manual overlap and existing decisions are excluded while unrelated edits survive", [] {
            auto file = test_support::batchFile();
            const auto position = file.current().draft.text.find("base right\n");
            CHECK(file.editResult(1, {{position, position + 11}, "manual right\n"}) == Transition::Applied);
            const auto preview = previewNonConflicting(file, file.current().id);
            CHECK(preview.state == Transition::ConfirmationRequired && preview.changes.size() == 1 && preview.skipped > 0);
            CHECK(preview.changes.front().replacement == "left only\n");
            CHECK(applyNonConflicting(file, preview, true) == Transition::Applied);
            CHECK(file.current().draft.text == "header\nleft only\nbridge one\nmanual right\nbridge two\nbase conflict\ntail\n");
            const auto chosen = file.current().draft;
            CHECK(previewNonConflicting(file, file.current().id).state == Transition::NoChange);
            CHECK(file.current().draft == chosen);
        });
        suite.run("TRUCE-0018 a previously ignored eligible contribution stays ignored while the other side is batched", [] {
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
        });
        suite.run("TRUCE-0018 absent earlier context stale and altered batches never apply", [] {
            auto missing = test_support::choiceFile();
            const auto blocked = previewNonConflicting(missing, 1);
            CHECK(blocked.state == Transition::Blocked && blocked.detail.find("earlier version is unavailable") != std::string::npos);
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
        });
        suite.run("TRUCE-0018 insertions touching a competing changed range stay outstanding", [] {
            auto captured = *test_support::choiceFile("head\ninserted\nbase\ntail\n", "head\nchanged\ntail\n").snapshots();
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
            for (const auto &[id, value] : before.decisions) { (void)id; CHECK(value.value == DecisionValue::Undecided); }
        });
        suite.run("TRUCE-0018 ambiguous repeated result passages stay outstanding", [] {
            const auto source = test_support::batchFile();
            const auto earlier = source.snapshots()->earlier->worktree.utf8;
            auto captured = *source.snapshots();
            captured.launchResult.text = test_support::choiceText(earlier + earlier);
            FileSession file("choices", source.path(), {earlier + earlier, {}, 0},
                Support::Supported, {}, captured, true);
            const auto before = file.current().draft;
            const auto preview = previewNonConflicting(file, 1);
            CHECK(preview.state == Transition::NoChange && preview.changes.empty() && preview.skipped > 0);
            CHECK(applyNonConflicting(file, preview, true) == Transition::NoChange);
            CHECK(file.current().id == 1 && file.current().draft == before);
        });
        for (const auto side : {SourceSide::Stage2, SourceSide::Stage3}) {
            suite.run("TRUCE-0019 whole-file previews describe and replace exactly the selected source "
                + std::to_string(static_cast<int>(side)), [=] {
                auto file = test_support::choiceFile();
                CHECK(file.editResult(1, {{0, 0}, "manual prefix\n"}) == Transition::Applied);
                const auto before = file.current();
                const auto preview = previewWholeFile(file, before.id, side);
                const auto &source = side == SourceSide::Stage2 ? file.snapshots()->stage2 : file.snapshots()->stage3;
                CHECK(preview.state == Transition::ConfirmationRequired && preview.before == before.draft.text);
                CHECK(preview.replacement == source->worktree.utf8 && preview.fileId == file.id());
                CHECK(preview.origin.sourceObjectId == source->objectId && !preview.contributions.empty() && !preview.differences.empty());
                CHECK(preview.detail.find("manual edits") != std::string::npos && preview.retainedDecisions == 0);
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
                    CHECK(contribution.after == Decision{contribution.source.side == side
                        ? DecisionValue::Accepted : DecisionValue::Ignored, {}});
                }
                CHECK(!file.reviewed() && !file.written() && !file.staged());
                CHECK(file.undo(file.current().id) == Transition::Applied && file.current().draft == before.draft);
                CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == chosen);
            });
        }
        suite.run("TRUCE-0019 altered or stale whole-file confirmation cannot change the draft", [] {
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
        });
        suite.run("TRUCE-0019 whole-file replacement retains recovered decisions without source scopes", [] {
            const auto source = test_support::choiceFile();
            const Draft legacy{"legacy manual result\n", {{"legacy-left", {DecisionValue::Accepted, 3}},
                {"legacy-right", {DecisionValue::Ignored, {}}}}, 0};
            auto captured = *source.snapshots();
            captured.launchResult.text = test_support::choiceText(legacy.text);
            FileSession file(source.id(), source.path(), legacy, Support::Supported, {}, captured);
            const auto preview = previewWholeFile(file, 1, SourceSide::Stage3);
            CHECK(preview.state == Transition::ConfirmationRequired && preview.retainedDecisions == legacy.decisions.size());
            CHECK(preview.contributions.empty() && preview.command.decisions.empty());
            CHECK(applyWholeFile(file, preview, true) == Transition::Applied);
            CHECK(file.current().draft.text == source.snapshots()->stage3->worktree.utf8);
            CHECK(file.current().draft.decisions == legacy.decisions);
            CHECK(file.undo(file.current().id) == Transition::Applied && file.current().draft == legacy);
        });
        for (const auto scope : {ChoiceScope::Line, ChoiceScope::Section}) {
            suite.run("TRUCE-0016 scoped Accept preserves unrelated edits and restores atomic history "
                + std::to_string(static_cast<int>(scope)), [=] {
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
                CHECK(accepted.text == "manual prefix\n" + (scope == ChoiceScope::Section ? original
                    : "header\nleft first\nmanual second\nbridge\nleft later\ntail\n") + "manual suffix\n");
                for (const auto &[id, decision] : accepted.decisions) {
                    const bool selected = std::find(target.decisions.begin(), target.decisions.end(), id) != target.decisions.end();
                    CHECK(decision.value == (selected ? DecisionValue::Accepted : DecisionValue::Undecided));
                }
                CHECK(file.undo(file.current().id) == Transition::Applied && file.current().draft == before);
                CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == accepted);
                CHECK(applyChoice(file, preview, true) == Transition::StaleRevision);
                CHECK(file.current().draft == accepted);
            });
        }
        for (const auto scope : {ChoiceScope::Line, ChoiceScope::Section}) {
            suite.run("TRUCE-0016 Ignore preserves manual text and competing decisions through undo and redo "
                + std::to_string(static_cast<int>(scope)), [=] {
                auto file = test_support::choiceFile();
                CHECK(file.editResult(1, {{7, 18}, "manual target\n"}) == Transition::Applied);
                const auto before = file.current().draft;
                const auto target = test_support::choiceTarget(file, SourceSide::Stage2, scope);
                const auto preview = previewChoice(file, file.current().id, target.source.contributionId, ChoiceAction::Ignore);
                CHECK(preview.state == Transition::Applied);
                CHECK(applyChoice(file, preview) == Transition::Applied);
                const auto ignored = file.current().draft;
                CHECK(ignored.text == before.text);
                for (const auto &[id, decision] : ignored.decisions) {
                    const bool selected = std::find(target.decisions.begin(), target.decisions.end(), id) != target.decisions.end();
                    CHECK(decision.value == (selected ? DecisionValue::Ignored : DecisionValue::Undecided));
                }
                CHECK(previewChoice(file, file.current().id, target.source.contributionId, ChoiceAction::Accept).state == Transition::Blocked);
                CHECK(file.undo(file.current().id) == Transition::Applied && file.current().draft == before);
                CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == ignored);
            });
        }
        suite.run("TRUCE-0016 resolve using one side decides only its section and its competitors", [] {
            auto file = test_support::choiceFile();
            CHECK(file.editResult(1, {{0, 0}, "manual prefix\n"}) == Transition::Applied);
            const auto before = file.current().draft;
            const auto target = test_support::choiceTarget(file, SourceSide::Stage3, ChoiceScope::Section);
            const auto preview = previewChoice(file, file.current().id, target.source.contributionId, ChoiceAction::ResolveUsingSide);
            CHECK(preview.state == Transition::Applied || preview.state == Transition::ConfirmationRequired);
            CHECK(applyChoice(file, preview, true) == Transition::Applied);
            const auto resolved = file.current().draft;
            CHECK(resolved.text == "manual prefix\nheader\nright first\nright second\nbridge\nleft later\ntail\n");
            for (const auto &[id, decision] : resolved.decisions) {
                const bool own = std::find(target.decisions.begin(), target.decisions.end(), id) != target.decisions.end();
                const bool competitor = std::find(target.competitors.begin(), target.competitors.end(), id) != target.competitors.end();
                CHECK(decision.value == (own ? DecisionValue::Accepted : competitor ? DecisionValue::Ignored : DecisionValue::Undecided));
            }
            CHECK(file.undo(file.current().id) == Transition::Applied && file.current().draft == before);
            CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == resolved);
        });
        suite.run("TRUCE-0016 stale and ambiguous scoped choices leave text decisions and history unchanged", [] {
            auto file = test_support::choiceFile();
            const auto target = test_support::choiceTarget(file, SourceSide::Stage2, ChoiceScope::Line);
            const auto preview = previewChoice(file, 1, target.source.contributionId, ChoiceAction::Accept);
            CHECK(file.editResult(1, {{0, 0}, "new manual edit\n"}) == Transition::Applied);
            const auto before = file.current();
            CHECK(applyChoice(file, preview, true) == Transition::StaleRevision);
            CHECK(file.current().id == before.id && file.current().draft == before.draft && !file.canRedo());
            auto ambiguous = test_support::choiceFile("head\nrepeat\nrepeat\ntail\n",
                "head\nchanged\nrepeat\ntail\n", "head\nrepeat\ntail\n");
            const auto repeated = test_support::choiceTarget(ambiguous, SourceSide::Stage2, ChoiceScope::Line);
            const auto rejected = previewChoice(ambiguous, 1, repeated.source.contributionId, ChoiceAction::Accept);
            CHECK(rejected.state == Transition::AmbiguousTarget);
            const auto unchanged = ambiguous.current().draft;
            CHECK(applyChoice(ambiguous, rejected, true) == Transition::AmbiguousTarget);
            CHECK(ambiguous.current().id == 1 && ambiguous.current().draft == unchanged && !ambiguous.canUndo());
        });
        suite.run("TRUCE-0016 resolve using one line leaves the other section and manual edits unchanged", [] {
            auto file = test_support::choiceFile("header\nleft\nbridge\nleft later\ntail\n",
                "header\nright\nbridge\nright later\ntail\n");
            CHECK(file.editResult(1, {{0, 0}, "manual prefix\n"}) == Transition::Applied);
            const auto before = file.current().draft;
            const auto target = test_support::choiceTarget(file, SourceSide::Stage3, ChoiceScope::Line);
            const auto competitor = test_support::choiceTarget(file, SourceSide::Stage2, ChoiceScope::Line);
            const auto preview = previewChoice(file, file.current().id, target.source.contributionId, ChoiceAction::ResolveUsingSide);
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
        });
        suite.run("TRUCE-0188 each section can be decided with a different side", [] {
            for (const bool marked : {false, true}) {
                for (const auto action : {ChoiceAction::ResolveUsingSide, ChoiceAction::Accept}) {
                    for (const auto first : {SourceSide::Stage2, SourceSide::Stage3}) {
                        auto file = marked ? test_support::markedFile() : test_support::choiceFile();
                        const auto second = first == SourceSide::Stage2 ? SourceSide::Stage3 : SourceSide::Stage2;
                        CHECK(choose(file, first, ChoiceAction::ResolveUsingSide) == Transition::Applied);
                        const auto before = file.current().draft;
                        CHECK(choose(file, second, action, ChoiceScope::Section, 1) == Transition::Applied);
                        const auto later = second == SourceSide::Stage2 ? "left later\n" : "right later\n";
                        const auto chosen = file.current().draft;
                        if (marked && action == ChoiceAction::Accept) {
                            CHECK(chosen.text == before.text && chosen.unresolvedMarkers == 1);
                        } else {
                            CHECK(chosen.text == (first == SourceSide::Stage2 ? "header\nleft first\nleft second\nbridge\n"
                                : "header\nright first\nright second\nbridge\n") + std::string(later) + "tail\n");
                            CHECK(chosen.unresolvedMarkers == 0);
                        }
                        const auto &picked = test_support::choiceTarget(file, second, ChoiceScope::Section, 1);
                        for (const auto &id : picked.decisions) CHECK(chosen.decisions.at(id).value == DecisionValue::Accepted);
                        CHECK(file.undo(file.current().id) == Transition::Applied && file.current().draft == before);
                    }
                }
            }
        });
        suite.run("TRUCE-0188 an accept still refuses accepted text it may replace", [] {
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
            CHECK(refused.detail == "Accepted text may overlap this choice. Preview an ordered combination or undo its acceptance.");
        });
        suite.run("TRUCE-0012 correspondence is deterministic and partitions repeated inserted deleted and changed text", [] {
            const std::array cases{
                std::pair{"repeat\nrepeat\n", "repeat\nrepeat\n"},
                std::pair{"head\nold\nend\n", "head\nnew\nend\n"},
                std::pair{"head\nend\n", "head\ninserted\nend\n"},
                std::pair{"head\ndeleted\nend\n", "head\nend\n"},
                std::pair{"head\nrepeat\nrepeat\nend\n", "head\nrepeat\nend\n"}
            };
            const std::array expected{PassageKind::Equal, PassageKind::Changed,
                PassageKind::ResultOnly, PassageKind::SourceOnly, PassageKind::Ambiguous};
            ExactLineCorrespondence algorithm;
            for (std::size_t i = 0; i < cases.size(); ++i) {
                const std::string_view source = cases[i].first;
                const std::string_view result = cases[i].second;
                const auto passages = algorithm.compare(source, result);
                CHECK(!passages.empty());
                bool sawKind = false;
                std::size_t sourceEnd = 0;
                std::size_t resultEnd = 0;
                for (const auto &passage : passages) {
                    CHECK(passage.source.begin == sourceEnd && passage.result.begin == resultEnd);
                    CHECK(validByteRange(source, passage.source) && validByteRange(result, passage.result));
                    if (passage.kind == PassageKind::Equal)
                        CHECK(source.substr(passage.source.begin, passage.source.end - passage.source.begin)
                            == result.substr(passage.result.begin, passage.result.end - passage.result.begin));
                    sawKind = sawKind || passage.kind == expected[i];
                    sourceEnd = passage.source.end;
                    resultEnd = passage.result.end;
                }
                CHECK(sourceEnd == source.size() && resultEnd == result.size() && sawKind);
                for (int repeat = 0; repeat < 4; ++repeat) CHECK(algorithm.compare(source, result) == passages);
            }
        });
        suite.run("TRUCE-0012 ambiguous repeated and reordered passages reject choices atomically", [] {
            for (const auto &texts : {std::pair{"head\nrepeat\nrepeat\nend\n", "head\nrepeat\nend\n"},
                                     std::pair{"head\none\ntwo\nend\n", "head\ntwo\none\nend\n"}}) {
                auto file = mappedFile(texts.first, texts.second);
                CHECK(file.bindSourceAnchor(1, {"choice", SourceSide::Stage2, {5, 12}}) == Transition::Applied);
                const auto issued = anchor(file);
                CHECK(issued.state == AnchorState::Ambiguous && !issued.result);
                const auto before = file.current();
                CHECK(file.replaceMapped(1, issued, "unsafe replacement\n", Decision{DecisionValue::Accepted, 1}, true)
                    == Transition::AmbiguousTarget);
                CHECK(file.current().id == before.id && file.current().draft == before.draft && !file.canUndo());
            }
        });
        suite.run("TRUCE-0012 equal repeated passages keep the selected occurrence", [] {
            auto file = mappedFile("repeat\nmiddle\nrepeat\n", "manual prefix\nrepeat\nmiddle\nrepeat\n");
            CHECK(file.bindSourceAnchor(1, {"choice", SourceSide::Stage2, {14, 21}}) == Transition::Applied);
            const auto issued = anchor(file);
            CHECK(issued.state == AnchorState::Mapped && issued.result == ByteRange{28, 35});
            CHECK(file.replaceMapped(1, issued, "selected\n", Decision{DecisionValue::Accepted, 1}) == Transition::Applied);
            CHECK(file.current().draft.text == "manual prefix\nrepeat\nmiddle\nselected\n");
            CHECK(file.current().draft.decisions.at("choice") == Decision{DecisionValue::Accepted, 1});
        });
        suite.run("TRUCE-0012 UTF-8 boundaries and forged anchor identities cannot mutate a transaction", [] {
            const std::string text = "start\n\xc3\xa9\xf0\x9f\x98\x80\nend\n";
            auto file = mappedFile(text, text);
            CHECK(file.bindSourceAnchor(1, {"inside-codepoint", SourceSide::Stage2, {7, 13}}) == Transition::InvalidAnchor);
            CHECK(file.bindSourceAnchor(1, {"choice", SourceSide::Stage2, {6, 13}}) == Transition::Applied);
            const auto issued = anchor(file);
            const auto before = file.current().draft;
            CHECK(file.replaceMapped(0, issued, "stale command", Decision{DecisionValue::Accepted, 1}, true)
                == Transition::StaleRevision);
            CHECK(file.editResult(1, {{7, 8}, "x"}) == Transition::InvalidAnchor);
            CHECK(file.replaceMapped(1, issued, "\x80", Decision{DecisionValue::Accepted, 1}) == Transition::InvalidAnchor);
            for (int field = 0; field < 5; ++field) {
                auto forged = issued;
                if (field == 0) forged.origin.sessionId = "other session";
                if (field == 1) forged.origin.path = "another.txt";
                if (field == 2) forged.origin.sourceObjectId = "other object";
                if (field == 3) forged.source.range.begin = 0;
                if (field == 4) forged.result = ByteRange{0, 5};
                CHECK(file.replaceMapped(1, forged, "wrong", Decision{DecisionValue::Accepted, 1}) == Transition::InvalidAnchor);
                CHECK(file.current().id == 1 && file.current().draft == before && !file.canUndo());
            }
            CHECK(file.replaceMapped(1, issued, "valid\n", Decision{DecisionValue::Accepted, 1}) == Transition::Applied);
            CHECK(file.current().draft.text == "start\nvalid\nend\n");
        });
        suite.run("TRUCE-0012 source-only anchors use the deletion site and preserve inserted result passages", [] {
            auto file = mappedFile("head\nremoved\ntail\n", "manual prefix\nhead\ntail\nmanual suffix\n");
            CHECK(file.bindSourceAnchor(1, {"choice", SourceSide::Stage2, {5, 13}}) == Transition::Applied);
            const auto deleted = anchor(file);
            CHECK(deleted.state == AnchorState::Mapped && deleted.result == ByteRange{19, 19});
            CHECK(!deleted.manualOverlap);
            CHECK(file.replaceMapped(1, deleted, "restored\n", Decision{DecisionValue::Accepted, 1})
                == Transition::Applied);
            CHECK(file.current().draft.text == "manual prefix\nhead\nrestored\ntail\nmanual suffix\n");
        });
        suite.run("TRUCE-0012 anchors transform around manual insertions and deletions without losing unrelated edits", [] {
            auto file = mappedFile("head\nleft\nright\nend\n", "head\nleft\nright\nend\n");
            CHECK(file.bindSourceAnchor(1, {"choice", SourceSide::Stage2, {5, 10}}) == Transition::Applied);
            const auto stale = anchor(file);
            const std::string prefix = "manual \xc3\xa9\n";
            const std::string suffix = "unrelated manual tail\n";
            CHECK(file.editResult(1, {{0, 0}, prefix}) == Transition::Applied);
            CHECK(file.editResult(file.current().id, {{prefix.size(), prefix.size() + 5}, {}}) == Transition::Applied);
            const auto end = file.current().draft.text.size();
            CHECK(file.editResult(file.current().id, {{end, end}, suffix}) == Transition::Applied);
            const auto current = anchor(file);
            CHECK(current.result == ByteRange{prefix.size(), prefix.size() + 5} && !current.manualOverlap);
            const auto before = file.current().draft;
            CHECK(file.replaceMapped(file.current().id, stale, "stale", Decision{DecisionValue::Accepted, 1}, true)
                == Transition::StaleRevision);
            CHECK(file.current().draft == before);
            CHECK(file.replaceMapped(file.current().id, current, "accepted\n", Decision{DecisionValue::Accepted, 1})
                == Transition::Applied);
            CHECK(file.current().draft.text == prefix + "accepted\nright\nend\n" + suffix);
        });
        suite.run("TRUCE-0012 changed manual passages require confirmation bound to the current preview", [] {
            auto file = mappedFile("head\nsource\nend\n", "head\nmanual\nend\n");
            CHECK(file.bindSourceAnchor(1, {"choice", SourceSide::Stage2, {5, 12}}) == Transition::Applied);
            const auto preview = anchor(file);
            CHECK(preview.manualOverlap && preview.result == ByteRange{5, 12});
            const auto before = file.current().draft;
            CHECK(file.replaceMapped(1, preview, "accepted\n", Decision{DecisionValue::Accepted, 1})
                == Transition::ConfirmationRequired);
            CHECK(file.current().draft == before && !file.canUndo());
            CHECK(file.editResult(1, {{6, 7}, "X"}) == Transition::Applied);
            CHECK(file.replaceMapped(file.current().id, preview, "accepted\n", Decision{DecisionValue::Accepted, 1}, true)
                == Transition::StaleRevision);
            CHECK(file.current().draft.decisions == before.decisions);
            CHECK(file.replaceMapped(file.current().id, anchor(file), "accepted\n", Decision{DecisionValue::Accepted, 1}, true)
                == Transition::Applied);
            CHECK(file.current().draft.text == "head\naccepted\nend\n");
        });
        suite.run("TRUCE-0012 deleted anchors and ambiguous whole-buffer edits never reattach to another repeat", [] {
            auto file = mappedFile("head\nrepeat\nrepeat\nend\n", "head\nrepeat\nrepeat\nend\n");
            CHECK(file.bindSourceAnchor(1, {"choice", SourceSide::Stage2, {12, 19}}) == Transition::Applied);
            CHECK(file.editResult(1, {{12, 19}, {}}) == Transition::Applied);
            CHECK(anchor(file).state == AnchorState::Invalidated && !anchor(file).result);
            auto before = file.current().draft;
            CHECK(file.replaceMapped(file.current().id, anchor(file), "wrong\n", Decision{DecisionValue::Accepted, 1}, true)
                == Transition::InvalidAnchor);
            CHECK(file.current().draft == before);
            CHECK(file.undo(file.current().id) == Transition::Applied);
            CHECK(anchor(file).state == AnchorState::Mapped);
            before = file.current().draft;
            before.text = "head\nrepeat\nend\n";
            CHECK(file.replaceDraft(file.current().id, before) == Transition::Applied);
            CHECK(anchor(file).state == AnchorState::Ambiguous && !anchor(file).result);
            CHECK(file.replaceMapped(file.current().id, anchor(file), "wrong\n", Decision{DecisionValue::Accepted, 1}, true)
                == Transition::AmbiguousTarget);
            CHECK(file.current().draft == before);
        });
        suite.run("TRUCE-0012 insertion anchors respect affinity and history restores text decisions and mappings", [] {
            auto file = mappedFile("head\ntail\n", "head\ntail\n");
            CHECK(file.bindSourceAnchor(1, {"choice", SourceSide::Stage2, {5, 5}, InsertionAffinity::Before}) == Transition::Applied);
            CHECK(file.bindSourceAnchor(1, {"after", SourceSide::Stage2, {5, 5}, InsertionAffinity::After}) == Transition::Applied);
            const auto original = anchor(file);
            CHECK(file.editResult(1, {{5, 5}, "manual\n"}) == Transition::Applied);
            const auto edited = file.current().draft;
            CHECK(anchor(file).result == ByteRange{5, 5});
            CHECK(anchor(file, "after").result == ByteRange{12, 12});
            CHECK(file.replaceMapped(file.current().id, anchor(file), "choice\n", Decision{DecisionValue::Accepted, 1})
                == Transition::Applied);
            const auto chosen = file.current().draft;
            CHECK(chosen.text == "head\nchoice\nmanual\ntail\n");
            const auto chosenRange = anchor(file).result;
            CHECK(file.undo(file.current().id) == Transition::Applied);
            CHECK(file.current().draft == edited && anchor(file).result == ByteRange{5, 5});
            CHECK(anchor(file).revision == file.current().id);
            CHECK(file.redo(file.current().id) == Transition::Applied);
            CHECK(file.current().draft == chosen && anchor(file).result == chosenRange);
            CHECK(anchor(file).revision == file.current().id);
            CHECK(file.replaceMapped(file.current().id, original, "stale", {}, true) == Transition::StaleRevision);
        });
        suite.run("TRUCE-0052 correspondence succeeds when matrix-sized allocations are refused", [] {
            std::string source;
            for (int line = 0; line < 3500; ++line) source += "unique line " + std::to_string(line) + '\n';
            const auto result = "manual prefix\n" + source + "manual suffix\n";
            // A fixture fault budget, not a supported-file or operating limit.
            // Even a one-byte 3500 x 3502 matrix exceeds this allocation budget.
            allocation_fixture::Bound budget;
            const auto passages = ExactLineCorrespondence{}.compare(source, result);
            CHECK(passages == std::vector<Passage>{
                {{0, 0}, {0, 14}, PassageKind::ResultOnly},
                {{0, source.size()}, {14, 14 + source.size()}, PassageKind::Equal},
                {{source.size(), source.size()}, {14 + source.size(), result.size()}, PassageKind::ResultOnly}});
            CHECK(allocation_fixture::largest > 0 && allocation_fixture::largest < allocation_fixture::ceiling);
        });
        suite.run("source captures survive edits history checkpoints and staged discards", [] {
            auto input = capturedSources();
            const auto expected = input;
            FileSession file("id", input.path, draft(input.launchResult.text.utf8),
                             Support::Supported, {}, input);
            const auto *capture = file.snapshots();
            const auto unchanged = [&] {
                CHECK(capture != nullptr && file.snapshots() == capture);
                CHECK(*file.snapshots() == expected);
            };
            unchanged();
            input.stage2->rawBytes = "changed caller-owned source";
            input.provenance.sessionId = "another session";
            input.launchResult.text.bytes.clear();
            unchanged();
            CHECK(file.current().draft.text == "manual\nkept");
            CHECK(file.replaceDraft(file.current().id, draft("edited\n")) == Transition::Applied);
            CHECK(file.recordCheckpoint(file.current().id, Outcome::Succeeded) == Transition::Applied);
            unchanged();
            auto later = draft("later\n");
            later.decisions["right"] = {DecisionValue::Accepted, 2};
            CHECK(file.replaceDraft(file.current().id, later) == Transition::Applied);
            unchanged();
            CHECK(file.undo(file.current().id) == Transition::Applied);
            CHECK(file.current().draft == draft("edited\n"));
            unchanged();
            CHECK(file.redo(file.current().id) == Transition::Applied);
            CHECK(file.current().draft == later);
            unchanged();
            CHECK(file.undo(file.current().id) == Transition::Applied);
            CHECK(file.replaceDraft(file.current().id, draft("new history branch\n")) == Transition::Applied);
            CHECK(!file.canRedo());
            unchanged();
            CHECK(file.discardToCheckpoint(file.current().id) == Transition::Applied);
            CHECK(file.current().draft == draft("edited\n"));
            unchanged();
            resolve(file);
            CHECK(file.replaceDraft(file.current().id, draft("after staging\n")) == Transition::Applied);
            CHECK(file.discardToStaged(file.current().id) == Transition::Applied);
            CHECK(file.current().draft == draft("edited\n"));
            unchanged();
            const auto copy = file;
            auto moved = std::move(file);
            CHECK(copy.snapshots() == capture && moved.snapshots() == capture);
            CHECK(*copy.snapshots() == expected && *moved.snapshots() == expected);
        });
        suite.run("an unavailable earlier source remains distinct from a captured empty source", [] {
            for (const bool missing : {false, true}) {
                auto captured = capturedSources();
                if (missing) captured.earlier.reset();
                else {
                    captured.earlier->rawBytes.clear();
                    captured.earlier->worktree = {};
                }
                const auto expected = captured;
                FileSession file("id", captured.path, draft(captured.launchResult.text.utf8),
                                 Support::Supported, {}, captured);
                CHECK(file.snapshots() && file.snapshots()->earlier.has_value() == !missing);
                if (!missing) {
                    CHECK(file.snapshots()->earlier->rawBytes.empty());
                    CHECK(file.snapshots()->earlier->worktree.utf8.empty());
                    CHECK(file.snapshots()->earlier->worktree.bytes.empty());
                }
                CHECK(file.replaceDraft(file.current().id, draft("edited")) == Transition::Applied);
                CHECK(file.undo(file.current().id) == Transition::Applied);
                CHECK(file.redo(file.current().id) == Transition::Applied);
                CHECK(*file.snapshots() == expected);
            }
            FileSession uncaptured("id", "outside", {}, Support::OutsideApp, "Missing source");
            CHECK(uncaptured.snapshots() == nullptr);
        });
        suite.run("source captures reject a different path or launch result", [] {
            const auto captured = capturedSources();
            for (const bool wrongPath : {false, true}) {
                bool rejected = false;
                try {
                    FileSession invalid("id", wrongPath ? "another path" : captured.path,
                        draft(wrongPath ? captured.launchResult.text.utf8 : "regenerated source"),
                        Support::Supported, {}, captured);
                } catch (const std::invalid_argument &) { rejected = true; }
                CHECK(rejected);
            }
        });
        suite.run("identity and initial blockers", [] {
            for (const bool emptyId : {false, true}) {
                bool rejected = false;
                try { FileSession invalid(emptyId ? "" : "id", emptyId ? "path" : "", draft()); }
                catch (const std::invalid_argument &) { rejected = true; }
                CHECK(rejected);
            }
            FileSession file("id", ":literal", draft());
            CHECK(file.id() == "id" && file.path() == ":literal");
            CHECK(file.current().id == 1 && file.current().result == 1 && file.current().decisions == 1);
            CHECK(file.facts().unsaved && file.facts().needsReview && !file.facts().resolved);
            CHECK(has(file, Blocker::UnsavedDraft) && has(file, Blocker::ReviewNeeded));
            CHECK(has(file, Blocker::WriteRequired) && has(file, Blocker::StageRequired));
            CHECK(file.undo(1) == Transition::NoChange && file.redo(1) == Transition::NoChange);
        });
        suite.run("transaction dimensions and stale editing commands", [] {
            FileSession file("id", "file", draft());
            CHECK(file.replaceDraft(1, draft()) == Transition::NoChange);
            CHECK(file.replaceDraft(1, draft("changed")) == Transition::Applied);
            const auto textRevision = file.current();
            CHECK(textRevision.result == textRevision.id && textRevision.decisions == 1);
            auto decisions = textRevision.draft;
            decisions.decisions["right"] = {DecisionValue::Accepted, 2};
            CHECK(file.replaceDraft(textRevision.id, decisions) == Transition::Applied);
            CHECK(file.current().result == textRevision.result);
            CHECK(file.current().decisions == file.current().id);
            const auto current = file.current().id;
            CHECK(file.replaceDraft(1, draft("stale")) == Transition::StaleRevision);
            CHECK(file.undo(1) == Transition::StaleRevision && file.redo(1) == Transition::StaleRevision);
            CHECK(file.discardToCheckpoint(1) == Transition::StaleRevision);
            CHECK(file.discardToStaged(1) == Transition::StaleRevision);
            CHECK(file.beginMarkResolved(1) == Transition::StaleRevision);
            CHECK(file.current().id == current && file.current().draft == decisions);
            // The marker count comes from the text, so a supplied count changes nothing.
            decisions.unresolvedMarkers = 1;
            CHECK(file.replaceDraft(current, decisions) == Transition::NoChange);
            CHECK(file.current().id == current && file.current().draft.unresolvedMarkers == 0);
        });
        suite.run("Save checkpoints only and keeps command history", [] {
            FileSession file("id", "file", draft());
            CHECK(file.replaceDraft(1, draft("saved")) == Transition::Applied);
            const auto saved = file.current();
            CHECK(file.recordCheckpoint(saved.id, Outcome::Succeeded) == Transition::Applied);
            CHECK(!file.facts().unsaved && file.facts().needsReview && !file.facts().resolved);
            CHECK(!file.reviewed() && !file.written() && !file.staged());
            CHECK(file.facts().status == FileStatus::SavedDraft && file.canUndo());
            CHECK(file.replaceDraft(saved.id, draft("newer")) == Transition::Applied);
            CHECK(file.undo(file.current().id) == Transition::Applied);
            CHECK(file.current().id > saved.id && file.current().draft == saved.draft);
            CHECK(file.current().result == saved.result && file.current().decisions == saved.decisions);
            CHECK(!file.facts().unsaved && file.facts().needsReview && file.canRedo());
            CHECK(file.recordCheckpoint(saved.id - 1, Outcome::Succeeded) == Transition::StaleRevision);
            CHECK(file.checkpoint()->id == saved.id);
        });
        suite.run("history restores decisions and cuts the redo branch", [] {
            FileSession file("id", "file", draft());
            auto combined = draft("first then second");
            combined.decisions["right"] = {DecisionValue::Accepted, 2};
            CHECK(file.replaceDraft(1, combined) == Transition::Applied);
            const auto combinedRevision = file.current();
            resolve(file);
            CHECK(file.undo(file.current().id) == Transition::Applied);
            CHECK(file.current().draft == draft() && file.facts().needsReview && !file.facts().resolved);
            CHECK(file.redo(file.current().id) == Transition::Applied);
            CHECK(file.current().draft == combined && file.current().id != combinedRevision.id);
            CHECK(file.current().result == combinedRevision.result && file.current().decisions == combinedRevision.decisions);
            CHECK(file.facts().earlierStaged && !file.facts().resolved);
            CHECK(file.undo(file.current().id) == Transition::Applied);
            CHECK(file.replaceDraft(file.current().id, draft("branch")) == Transition::Applied);
            CHECK(!file.canRedo() && file.redo(file.current().id) == Transition::NoChange);
            CHECK(file.staged()->revision.draft == combined);
        });
        suite.run("confirmation, markers and support block publication", [] {
            auto undecided = draft();
            undecided.decisions["right"] = {};
            FileSession file("id", "file", undecided);
            CHECK(file.beginMarkResolved(1) == Transition::ConfirmationRequired);
            CHECK(file.current().draft == undecided && !file.reviewed());
            CHECK(file.beginMarkResolved(1, true) == Transition::Applied);
            CHECK(file.reviewed()->confirmedUndecided && !has(file, Blocker::Undecided));
            undecided.text = "<<<<<<< ours\nresult\n=======\nother\n>>>>>>> theirs\n";
            CHECK(file.replaceDraft(1, undecided) == Transition::Applied);
            CHECK(file.current().draft.unresolvedMarkers == 1);
            CHECK(file.beginMarkResolved(file.current().id, true) == Transition::Blocked);
            CHECK(has(file, Blocker::UnresolvedMarkers) && has(file, Blocker::Undecided));
            for (const auto support : {Support::OutsideApp, Support::Unreadable}) {
                CHECK(file.setSupport(support, "unavailable") == Transition::Applied);
                const auto id = file.current().id;
                CHECK(file.replaceDraft(id, draft()) == Transition::Blocked);
                CHECK(file.undo(id) == Transition::Blocked && file.redo(id) == Transition::Blocked);
                CHECK(file.discardToCheckpoint(id) == Transition::Blocked);
                CHECK(file.discardToStaged(id) == Transition::Blocked);
                CHECK(file.beginMarkResolved(id, true) == Transition::Blocked && !file.facts().resolved);
                CHECK(file.supportDetail() == "unavailable");
                // TRUCE-0098: no app draft blockers for a file the app cannot edit.
                const auto expected = support == Support::OutsideApp ? Blocker::OutsideApp : Blocker::Unreadable;
                CHECK(file.facts().blockers == std::vector<Blocker>{expected});
                CHECK(file.facts().status == (support == Support::OutsideApp ? FileStatus::OutsideApp : FileStatus::Unreadable));
            }
        });
        suite.run("TRUCE-0172 a complete block is one typed range with line and byte bounds", [] {
            const std::string text = "a\n<<<<<<< ours\nleft\n=======\nright\n>>>>>>> theirs\nz\n";
            const auto scan = scanMarkers(text);
            CHECK(scan.orphans.empty() && scan.blocks.size() == 1 && scan.count() == 1);
            const auto &block = scan.blocks.front();
            CHECK(block.begin == (MarkerLine{MarkerKind::Begin, 1, {2, 15}}));
            CHECK(!block.base);
            CHECK(block.separator == (MarkerLine{MarkerKind::Separator, 3, {20, 28}}));
            CHECK(block.end == (MarkerLine{MarkerKind::End, 5, {34, 49}}));
            CHECK(text.substr(block.end.bytes.begin, block.end.bytes.end - block.end.bytes.begin) == ">>>>>>> theirs\n");
            CHECK(scan.first() == block.begin);
        });
        suite.run("TRUCE-0172 a block with a base section keeps the base line", [] {
            const std::string text = "<<<<<<< ours\nleft\n||||||| base\nbase\n=======\nright\n>>>>>>> theirs";
            const auto scan = scanMarkers(text);
            CHECK(scan.orphans.empty() && scan.blocks.size() == 1);
            const auto &block = scan.blocks.front();
            CHECK(block.base == (MarkerLine{MarkerKind::Base, 2, {18, 31}}));
            CHECK(block.separator.line == 4 && block.end.line == 6);
            // The last line has no line ending.
            CHECK(block.end.bytes == (ByteRange{50, text.size()}));
        });
        suite.run("TRUCE-0172 each marker line alone is an orphan", [] {
            const std::array<std::pair<std::string, MarkerKind>, 4> lines{{
                {"<<<<<<< ours", MarkerKind::Begin}, {"||||||| base", MarkerKind::Base},
                {"=======", MarkerKind::Separator}, {">>>>>>> theirs", MarkerKind::End}}};
            for (const auto &[line, kind] : lines) {
                const auto text = "before\n" + line + "\nafter\n";
                const auto scan = scanMarkers(text);
                CHECK(scan.blocks.empty() && scan.count() == 1);
                CHECK(scan.orphans == (std::vector<MarkerLine>{{kind, 1, {7, 8 + line.size()}}}));
                FileSession file("id", "file", {text, {}, 0});
                CHECK(file.current().draft.unresolvedMarkers == 1);
                CHECK(file.beginMarkResolved(1) == Transition::Blocked && has(file, Blocker::UnresolvedMarkers));
            }
        });
        suite.run("TRUCE-0172 marker-like text that is not a whole marker is ignored", [] {
            const std::string text = "x <<<<<<< ours\n  =======\n<<<<<<<< eight\n======== heading\n"
                                     ">>>>>>>theirs\n|||||| six\nend >>>>>>> theirs\n";
            CHECK(scanMarkers(text) == MarkerScan{});
            FileSession file("id", "file", {text, {}, 7});
            CHECK(file.current().draft.unresolvedMarkers == 0);
            CHECK(file.beginMarkResolved(1) == Transition::Applied);
        });
        suite.run("TRUCE-0172 CRLF line endings keep the carriage return inside the marker line", [] {
            const std::string text = "<<<<<<< ours\r\nleft\r\n=======\r\nright\r\n>>>>>>> theirs\r\n";
            const auto scan = scanMarkers(text);
            CHECK(scan.orphans.empty() && scan.blocks.size() == 1);
            const auto &block = scan.blocks.front();
            CHECK(block.begin.bytes == (ByteRange{0, 14}));
            CHECK(block.separator == (MarkerLine{MarkerKind::Separator, 2, {20, 29}}));
            CHECK(block.end == (MarkerLine{MarkerKind::End, 4, {36, text.size()}}));
        });
        suite.run("TRUCE-0172 broken and nested blocks leave every marker line counted", [] {
            const std::string text = "<<<<<<< a\n<<<<<<< b\nleft\n=======\nright\n>>>>>>> b\n"
                                     "=======\n>>>>>>> a\n||||||| late\n";
            const auto scan = scanMarkers(text);
            CHECK(scan.blocks.size() == 1 && scan.blocks.front().begin.line == 1);
            std::vector<std::size_t> orphanLines;
            for (const auto &line : scan.orphans) orphanLines.push_back(line.line);
            CHECK(orphanLines == (std::vector<std::size_t>{0, 6, 7, 8}));
            CHECK(scan.count() == 5 && scan.first()->line == 0);
        });
        suite.run("TRUCE-0172 deleting one marker line keeps the publish blocker", [] {
            const std::string text = "a\n<<<<<<< ours\nleft\n||||||| base\nbase\n=======\nright\n>>>>>>> theirs\nz\n";
            const auto block = scanMarkers(text).blocks.at(0);
            for (const auto &line : {block.begin, *block.base, block.separator, block.end}) {
                FileSession file("id", "file", {text, {}, 0});
                CHECK(file.beginMarkResolved(1) == Transition::Blocked);
                CHECK(file.editResult(1, {line.bytes, ""}) == Transition::Applied);
                const auto remaining = file.markers();
                CHECK(remaining.blocks.size() == (line.kind == MarkerKind::Base ? 1u : 0u));
                CHECK(file.current().draft.unresolvedMarkers == remaining.count());
                CHECK(file.current().draft.unresolvedMarkers != 0);
                CHECK(file.beginMarkResolved(file.current().id, true) == Transition::Blocked);
                CHECK(has(file, Blocker::UnresolvedMarkers) && !file.reviewed());
            }
            FileSession cleared("id", "file", {text, {}, 0});
            CHECK(cleared.replaceDraft(1, {"a\nleft\nz\n", {}, 3}) == Transition::Applied);
            CHECK(cleared.current().draft.unresolvedMarkers == 0 && !has(cleared, Blocker::UnresolvedMarkers));
            CHECK(cleared.beginMarkResolved(cleared.current().id) == Transition::Applied);
        });
        suite.run("TRUCE-0177 each full decision removes its section's marker lines in one undoable step", [] {
            enum class Path { ResolveLeft, ResolveRight, AcceptThenIgnore, IgnoreThenAccept, LeftFirst, RightFirst, Lines };
            for (const bool base : {false, true}) {
                const auto launch = test_support::markedText(base);
                // The second block is still open, so its marker lines stay.
                const auto later = launch.substr(launch.find("bridge\n"));
                for (const auto path : {Path::ResolveLeft, Path::ResolveRight, Path::AcceptThenIgnore,
                                        Path::IgnoreThenAccept, Path::LeftFirst, Path::RightFirst, Path::Lines}) {
                    auto file = test_support::markedFile(base);
                    CHECK(file.markerSections().size() == 2);
                    std::string first;
                    switch (path) {
                    case Path::ResolveLeft:
                        CHECK(choose(file, SourceSide::Stage2, ChoiceAction::ResolveUsingSide) == Transition::Applied);
                        first = "left first\nleft second\n";
                        break;
                    case Path::ResolveRight:
                        CHECK(choose(file, SourceSide::Stage3, ChoiceAction::ResolveUsingSide) == Transition::Applied);
                        first = "right first\nright second\n";
                        break;
                    case Path::AcceptThenIgnore:
                        CHECK(choose(file, SourceSide::Stage2, ChoiceAction::Accept) == Transition::Applied);
                        CHECK(file.current().draft.text == launch);
                        CHECK(choose(file, SourceSide::Stage3, ChoiceAction::Ignore) == Transition::Applied);
                        first = "left first\nleft second\n";
                        break;
                    case Path::IgnoreThenAccept:
                        CHECK(choose(file, SourceSide::Stage2, ChoiceAction::Ignore) == Transition::Applied);
                        CHECK(file.current().draft.text == launch);
                        CHECK(choose(file, SourceSide::Stage3, ChoiceAction::Accept) == Transition::Applied);
                        first = "right first\nright second\n";
                        break;
                    case Path::LeftFirst:
                        CHECK(choose(file, SourceSide::Stage2, ChoiceAction::CombineLeftFirst) == Transition::Applied);
                        first = "left first\nleft second\nright first\nright second\n";
                        break;
                    case Path::RightFirst:
                        CHECK(choose(file, SourceSide::Stage3, ChoiceAction::CombineRightFirst) == Transition::Applied);
                        first = "right first\nright second\nleft first\nleft second\n";
                        break;
                    case Path::Lines:
                        CHECK(choose(file, SourceSide::Stage2, ChoiceAction::Accept, ChoiceScope::Line, 0) == Transition::Applied);
                        CHECK(choose(file, SourceSide::Stage2, ChoiceAction::Ignore, ChoiceScope::Line, 1) == Transition::Applied);
                        CHECK(choose(file, SourceSide::Stage3, ChoiceAction::Ignore, ChoiceScope::Line, 0) == Transition::Applied);
                        CHECK(file.current().draft.text == launch);
                        CHECK(choose(file, SourceSide::Stage3, ChoiceAction::Accept, ChoiceScope::Line, 1) == Transition::Applied);
                        first = "left first\nright second\n";
                        break;
                    }
                    const auto chosen = file.current().draft;
                    CHECK(chosen.text == "header\n" + first + later);
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
            CHECK(choose(file, SourceSide::Stage3, ChoiceAction::ResolveUsingSide, ChoiceScope::Section, 1) == Transition::Applied);
            CHECK(file.current().draft.text == "header\nleft first\nleft second\nbridge\nright later\ntail\n");
            CHECK(file.current().draft.unresolvedMarkers == 0 && file.markerSections().empty());
            CHECK(file.beginMarkResolved(file.current().id) == Transition::Applied);
        });
        suite.run("TRUCE-0177 accept alone or ignoring both sides keeps the marker lines", [] {
            auto file = test_support::markedFile();
            const auto &left = test_support::choiceTarget(file, SourceSide::Stage2, ChoiceScope::Section);
            const auto accept = previewChoice(file, 1, left.source.contributionId, ChoiceAction::Accept);
            CHECK(accept.detail == "Accept changes only this scope and leaves the other side undecided.");
            CHECK(applyChoice(file, accept) == Transition::Applied);
            CHECK(file.current().draft.text == test_support::markedText());
            CHECK(file.current().draft.unresolvedMarkers == 2 && file.markerSections().size() == 2);
            const auto &right = test_support::choiceTarget(file, SourceSide::Stage3, ChoiceScope::Section);
            const auto ignore = previewChoice(file, file.current().id, right.source.contributionId, ChoiceAction::Ignore);
            CHECK(ignore.detail == "This decides the last open side of the conflict, so its marker lines and ignored text are removed.");
            auto ignored = test_support::markedFile();
            CHECK(choose(ignored, SourceSide::Stage2, ChoiceAction::Ignore) == Transition::Applied);
            CHECK(choose(ignored, SourceSide::Stage3, ChoiceAction::Ignore) == Transition::Applied);
            CHECK(ignored.current().draft.text == test_support::markedText());
            CHECK(ignored.current().draft.unresolvedMarkers == 2 && has(ignored, Blocker::UnresolvedMarkers));
        });
        suite.run("TRUCE-0177 a hand-edited marker line orphans its block, which is never removed", [] {
            for (const std::string line : {"<<<<<<< ours\n", "=======\n", ">>>>>>> theirs\n"}) {
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
            CHECK(typed.current().draft.text.find("<<<<<<< ours\nleft first\nleft second\n=======\nright typed first\n") != std::string::npos);
            CHECK(typed.current().draft.unresolvedMarkers == 2);
        });
        suite.run("TRUCE-0189 the deleting side of a marker section can be accepted and resolved", [] {
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
                    CHECK(preview.state == Transition::Applied && preview.before == "right first\n" && preview.replacement.empty());
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
                for (const auto &id : left.competitors) CHECK(chosen.decisions.at(id).value == DecisionValue::Ignored);
                CHECK(file.undo(file.current().id) == Transition::Applied && file.current().draft.text == before);
                CHECK(file.markerSections().size() == 1);
                CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == chosen);
            }
        });
        suite.run("TRUCE-0189 a deletion outside a marker section keeps the scope check", [] {
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
                CHECK(refused.detail == "The deletion scope is uncertain. Edit the result directly.");
                CHECK(applyChoice(file, refused, true) == Transition::AmbiguousTarget);
                CHECK(file.current().id == edited.id && file.current().draft == edited.draft);
            }
        });
        suite.run("TRUCE-0187 scanMarkers skips marker-like lines inside known source text", [] {
            const std::string text = "Title\n=======\n<<<<<<< ours\nleft\n=======\nright\n>>>>>>> theirs\n";
            const auto plain = scanMarkers(text);
            CHECK(plain.blocks.size() == 1 && plain.orphans.size() == 1 && plain.orphans.front().line == 1);
            // Unordered, overlapping ranges merge, and the heading underline inside them is content.
            const auto filtered = scanMarkers(text, {{6, 14}, {0, 10}});
            CHECK(filtered.orphans.empty() && filtered.blocks == plain.blocks);
            // Only a range covering the whole line makes it content.
            CHECK(scanMarkers(text, {{6, 13}}) == plain);
            // Blocks are assembled from the lines left over.
            const std::string inside = "<<<<<<< ours\nTitle\n=======\n=======\nright\n>>>>>>> theirs\n";
            CHECK(scanMarkers(inside).orphans.size() == 1 && scanMarkers(inside).blocks.front().separator.line == 2);
            const auto assembled = scanMarkers(inside, {{13, 27}});
            CHECK(assembled.orphans.empty() && assembled.blocks.size() == 1 && assembled.blocks.front().separator.line == 3);
        });
        suite.run("TRUCE-0187 a heading underline both sides keep does not block Mark resolved", [] {
            auto chosen = test_support::headingFile();
            // Only the conflict block counts at launch.
            CHECK(chosen.current().draft.unresolvedMarkers == 1 && chosen.markers().orphans.empty());
            CHECK(chosen.markers().blocks.size() == 1 && chosen.markers().first()->line == 3);
            CHECK(chosen.markerSections().size() == 1);
            CHECK(choose(chosen, SourceSide::Stage2, ChoiceAction::ResolveUsingSide) == Transition::Applied);
            CHECK(chosen.current().draft.text == "Changes\n=======\nintro\nleft line\ntail\n");
            CHECK(chosen.current().draft.unresolvedMarkers == 0 && !has(chosen, Blocker::UnresolvedMarkers));
            CHECK(chosen.beginMarkResolved(chosen.current().id) == Transition::Applied);
            // Resolving the block by hand clears the blocker too, and the heading stays.
            auto edited = test_support::headingFile();
            auto draft = edited.current().draft;
            draft.text = "Changes\n=======\nintro\nright line\ntail\n";
            CHECK(edited.replaceDraft(1, draft) == Transition::Applied);
            CHECK(edited.current().draft.unresolvedMarkers == 0 && edited.markers().count() == 0);
            CHECK(edited.beginMarkResolved(edited.current().id, true) == Transition::Applied);
            // Without captured sources the plain scan still counts the underline.
            FileSession plain("id", "file", {test_support::headingFile().current().draft.text, {}, 0});
            CHECK(plain.current().draft.unresolvedMarkers == 2 && plain.markers().orphans.front().line == 1);
        });
        suite.run("TRUCE-0187 marker lines Git inserted still block, whole or orphaned", [] {
            for (const auto kind : {MarkerKind::Begin, MarkerKind::Separator, MarkerKind::End}) {
                auto file = test_support::headingFile();
                const auto block = file.markers().blocks.front();
                const auto line = kind == MarkerKind::Begin ? block.begin
                    : kind == MarkerKind::Separator ? block.separator : block.end;
                CHECK(file.editResult(1, {line.bytes, ""}) == Transition::Applied);
                const auto remaining = file.markers();
                CHECK(remaining.blocks.empty() && remaining.orphans.size() == 2);
                CHECK(std::none_of(remaining.orphans.begin(), remaining.orphans.end(),
                                   [](const auto &orphan) { return orphan.line == 1; }));
                CHECK(file.current().draft.unresolvedMarkers == 2);
                CHECK(file.beginMarkResolved(file.current().id, true) == Transition::Blocked);
                CHECK(has(file, Blocker::UnresolvedMarkers) && !file.reviewed());
            }
        });
        suite.run("TRUCE-0193 scanMarkers counts earlier text as content only inside a base region", [] {
            const std::string text = "<<<<<<< ours\nleft\n||||||| base\nTitle\n=======\n=======\nright\n>>>>>>> theirs\n";
            const auto plain = scanMarkers(text);
            CHECK(plain.blocks.front().separator.line == 4 && plain.orphans.size() == 1 && plain.orphans.front().line == 5);
            // The underline lies in the earlier text, so the next separator closes the base.
            const auto assembled = scanMarkers(text, {}, {{31, 45}});
            CHECK(assembled.orphans.empty() && assembled.blocks.size() == 1);
            CHECK(assembled.blocks.front().base->line == 2 && assembled.blocks.front().separator.line == 5);
            // Earlier text before the base line or after the separator is not content.
            const std::string heading = "Title\n=======\n<<<<<<< ours\nleft\n=======\nright\n>>>>>>> theirs\n";
            CHECK(scanMarkers(heading, {}, {{0, 14}}) == scanMarkers(heading));
            CHECK(scanMarkers(heading, {}, {{0, heading.size()}}) == scanMarkers(heading));
            CHECK(scanMarkers(text, {}, {{0, 31}, {31, 45}, {53, text.size()}}) == assembled);
        });
        suite.run("TRUCE-0193 a heading underline in the diff3 base is content and the block resolves", [] {
            auto file = test_support::underlinedBaseFile();
            const auto launch = file.current().draft;
            const auto scan = file.markers();
            CHECK(scan.orphans.empty() && scan.blocks.size() == 1);
            CHECK(scan.blocks.front().base->line == 5 && scan.blocks.front().separator.line == 9);
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
        });
        suite.run("TRUCE-0193 a diff3 block without marker-like base text is assembled as before", [] {
            const std::string left = "intro\nleft line\ntail\n";
            const std::string launch = "intro\n<<<<<<< ours\nleft line\n||||||| base\nbase line\n=======\nright line\n>>>>>>> theirs\ntail\n";
            auto file = test_support::diff3File(left, "intro\nright line\ntail\n", "intro\nbase line\ntail\n", launch);
            auto plain = test_support::diff3File(left, "intro\nright line\ntail\n", "intro\nbase line\ntail\n", launch, false);
            CHECK(file.markers() == plain.markers() && file.markers() == scanMarkers(launch));
            CHECK(file.markerSections() == plain.markerSections() && file.markerSections().size() == 1);
            CHECK(choose(file, SourceSide::Stage2, ChoiceAction::ResolveUsingSide) == Transition::Applied);
            CHECK(file.current().draft.text == left && file.current().draft.unresolvedMarkers == 0);
            CHECK(file.beginMarkResolved(file.current().id) == Transition::Applied);
        });
        suite.run("TRUCE-0098 an outside-app file keeps its support blocker and recorded failures only", [] {
            FileSession file("id", "file", draft());
            CHECK(file.beginMarkResolved(1) == Transition::Applied);
            CHECK(file.recordCheckpoint(1, Outcome::Succeeded) == Transition::Applied);
            CHECK(file.recordWrite(1, Outcome::Failed, "disk full") == Transition::Applied);
            CHECK(file.setSupport(Support::OutsideApp, "binary now") == Transition::Applied);
            const auto facts = file.facts();
            CHECK((facts.blockers == std::vector<Blocker>{Blocker::OutsideApp, Blocker::WriteFailed}));
            CHECK(!facts.resolved && facts.status == FileStatus::OutsideApp);
            FileSession uncertain("id", "file", draft());
            CHECK(uncertain.beginMarkResolved(1) == Transition::Applied);
            CHECK(uncertain.recordCheckpoint(1, Outcome::Succeeded) == Transition::Applied);
            CHECK(uncertain.recordWrite(1, Outcome::Uncertain, "lost reply") == Transition::Applied);
            CHECK(uncertain.setSupport(Support::Unreadable, "gone") == Transition::Applied);
            CHECK((uncertain.facts().blockers == std::vector<Blocker>{Blocker::Unreadable, Blocker::UncertainOutcome}));
            CHECK(!uncertain.facts().resolved && uncertain.facts().status == FileStatus::Unreadable);
        });
        suite.run("outcomes require the reviewed saved and written transaction", [] {
            FileSession file("id", "file", draft());
            CHECK(file.recordWrite(1, Outcome::Succeeded) == Transition::StaleRevision);
            CHECK(file.recordStage(1, Outcome::Succeeded, "object", "100644") == Transition::StaleRevision);
            CHECK(file.recordCheckpoint(999, Outcome::Succeeded) == Transition::StaleRevision);
            CHECK(file.beginMarkResolved(1) == Transition::Applied);
            CHECK(file.recordWrite(1, Outcome::Succeeded) == Transition::InvalidOutcome);
            for (const auto outcome : {Outcome::Failed, Outcome::Uncertain}) {
                CHECK(file.recordCheckpoint(1, outcome, "save diagnostic") == Transition::Applied);
                CHECK(file.recordWrite(1, Outcome::Succeeded) == Transition::InvalidOutcome);
                CHECK(file.reviewed()->revision.id == 1 && !file.facts().resolved);
                CHECK(file.checkpointOutcome()->detail == "save diagnostic");
            }
            CHECK(file.recordCheckpoint(1, Outcome::Succeeded) == Transition::Applied);
            CHECK(file.recordStage(1, Outcome::Succeeded, "object", "100644") == Transition::InvalidOutcome);
            CHECK(file.recordWrite(1, Outcome::Failed, "write failed") == Transition::Applied);
            CHECK(file.recordStage(1, Outcome::Succeeded, "object", "100644") == Transition::InvalidOutcome);
            CHECK(file.recordWrite(1, Outcome::Succeeded) == Transition::Applied);
            CHECK(file.recordStage(1, Outcome::Succeeded, "", "100644") == Transition::InvalidOutcome);
            CHECK(file.recordStage(1, Outcome::Succeeded, "object", "") == Transition::InvalidOutcome);
            CHECK(!file.staged() && !file.facts().resolved);
            CHECK(file.recordStage(1, Outcome::Succeeded, "object", "100644") == Transition::Applied);
            CHECK(file.facts().resolved && file.facts().status == FileStatus::Resolved);
        });
        for (const auto outcome : {Outcome::Failed, Outcome::Uncertain}) {
            for (int step = 0; step != 3; ++step) {
                suite.run("failed or uncertain outcome retains snapshots " + std::to_string(step)
                          + '/' + std::to_string(static_cast<int>(outcome)), [=] {
                    FileSession file("id", "file", draft());
                    resolve(file);
                    const std::array failures{Blocker::CheckpointFailed, Blocker::WriteFailed, Blocker::StageFailed};
                    if (step == 0) CHECK(file.recordCheckpoint(1, outcome, "diagnostic") == Transition::Applied);
                    if (step == 1) CHECK(file.recordWrite(1, outcome, "diagnostic") == Transition::Applied);
                    if (step == 2) CHECK(file.recordStage(1, outcome, {}, {}, "diagnostic") == Transition::Applied);
                    CHECK(file.checkpoint()->id == 1 && file.written()->id == 1 && file.staged()->revision.id == 1);
                    CHECK(!file.facts().resolved);
                    CHECK(has(file, outcome == Outcome::Uncertain ? Blocker::UncertainOutcome
                                                                : failures[static_cast<std::size_t>(step)]));
                    const std::array statuses{FileStatus::CheckpointFailed, FileStatus::WriteFailed, FileStatus::StageFailed};
                    CHECK(file.facts().status == (outcome == Outcome::Uncertain ? FileStatus::OutcomeUncertain
                                                                : statuses[static_cast<std::size_t>(step)]));
                });
            }
        }
        suite.run("checkpoint discard fences delayed callbacks even after undo", [] {
            FileSession file("id", "file", draft());
            CHECK(file.recordCheckpoint(1, Outcome::Succeeded) == Transition::Applied);
            CHECK(file.replaceDraft(1, draft("discarded")) == Transition::Applied);
            const auto discarded = file.current().id;
            CHECK(file.beginMarkResolved(discarded) == Transition::Applied);
            CHECK(file.undo(discarded) == Transition::Applied);
            CHECK(file.current().draft == draft());
            CHECK(file.discardToCheckpoint(file.current().id) == Transition::Applied);
            CHECK(file.recordCheckpoint(discarded, Outcome::Succeeded) == Transition::StaleRevision);
            CHECK(file.recordWrite(discarded, Outcome::Succeeded) == Transition::StaleRevision);
            CHECK(file.recordStage(discarded, Outcome::Succeeded, "object", "100644") == Transition::StaleRevision);
            CHECK(file.checkpoint()->id == 1 && !file.facts().unsaved && file.facts().needsReview);
        });
        suite.run("discard without checkpoint restores initial draft and rejects delayed Save", [] {
            FileSession file("id", "file", draft());
            CHECK(file.discardToCheckpoint(1) == Transition::NoChange);
            CHECK(file.replaceDraft(1, draft("discarded")) == Transition::Applied);
            const auto discarded = file.current().id;
            CHECK(file.discardToCheckpoint(discarded) == Transition::Applied);
            CHECK(file.current().draft == draft() && !file.checkpoint());
            CHECK(file.recordCheckpoint(discarded, Outcome::Succeeded) == Transition::StaleRevision);
            CHECK(file.facts().unsaved && !file.facts().resolved);
        });
        suite.run("staged discard deliberately restores verified identity and fences newer callbacks", [] {
            FileSession file("id", "file", draft());
            CHECK(file.discardToStaged(1) == Transition::Blocked);
            resolve(file);
            CHECK(file.discardToStaged(1) == Transition::NoChange);
            CHECK(file.replaceDraft(1, draft("discarded")) == Transition::Applied);
            const auto discarded = file.current().id;
            CHECK(file.recordCheckpoint(discarded, Outcome::Succeeded) == Transition::Applied);
            CHECK(file.discardToStaged(discarded) == Transition::Applied);
            CHECK(file.current().id == 1 && file.checkpoint()->id == 1 && file.facts().resolved);
            CHECK(file.recordCheckpoint(discarded, Outcome::Failed) == Transition::StaleRevision);
            CHECK(file.recordWrite(discarded, Outcome::Succeeded) == Transition::StaleRevision);
            CHECK(file.recordStage(discarded, Outcome::Succeeded, "other", "100644") == Transition::StaleRevision);
            CHECK(file.facts().resolved);
        });
        for (const auto outcome : {Outcome::Succeeded, Outcome::Uncertain}) {
            suite.run("staged discard cannot pretend a newer write did not happen "
                      + std::to_string(static_cast<int>(outcome)), [=] {
                FileSession file("id", "file", draft());
                resolve(file);
                CHECK(file.replaceDraft(1, draft("new")) == Transition::Applied);
                const auto newer = file.current().id;
                CHECK(file.beginMarkResolved(newer) == Transition::Applied);
                CHECK(file.recordCheckpoint(newer, Outcome::Succeeded) == Transition::Applied);
                CHECK(file.recordWrite(newer, outcome) == Transition::Applied);
                CHECK(file.discardToStaged(newer) == Transition::Blocked);
                CHECK(file.current().id == newer && file.staged()->revision.id == 1);
            });
        }
        for (const auto outcome : {Outcome::Succeeded, Outcome::Failed, Outcome::Uncertain}) {
            for (const bool saveBeforeReview : {false, true}) {
                suite.run("TRUCE-0042 delayed latest-review write after newer Save "
                          + std::to_string(static_cast<int>(outcome)) + '/' + std::to_string(saveBeforeReview), [=] {
                    FileSession file("id", "file", draft());
                    if (saveBeforeReview) CHECK(file.recordCheckpoint(1, Outcome::Succeeded) == Transition::Applied);
                    CHECK(file.beginMarkResolved(1) == Transition::Applied);
                    if (!saveBeforeReview) CHECK(file.recordCheckpoint(1, Outcome::Succeeded) == Transition::Applied);
                    CHECK(file.replaceDraft(1, draft("new saved draft")) == Transition::Applied);
                    const auto newer = file.current().id;
                    CHECK(file.recordCheckpoint(newer, Outcome::Succeeded) == Transition::Applied);
                    CHECK(file.recordWrite(1, outcome, "delayed write diagnostic") == Transition::Applied);
                    CHECK(file.writeOutcome()->revision == 1 && file.writeOutcome()->outcome == outcome);
                    CHECK(file.writeOutcome()->detail == "delayed write diagnostic");
                    CHECK(file.checkpoint()->id == newer && file.current().id == newer);
                    CHECK(!file.facts().unsaved && file.facts().needsReview && !file.facts().resolved);
                    CHECK(file.reviewed()->revision.id == 1);
                    if (outcome == Outcome::Succeeded) {
                        CHECK(file.written()->id == 1 && file.written()->draft == draft());
                        CHECK(file.facts().status == FileStatus::WrittenUnresolved);
                        CHECK(file.recordStage(1, Outcome::Succeeded, "earlier", "100644") == Transition::Applied);
                        CHECK(file.facts().earlierStaged && !file.facts().resolved);
                    } else {
                        CHECK(!file.written());
                        CHECK(file.facts().status == (outcome == Outcome::Failed ? FileStatus::WriteFailed : FileStatus::OutcomeUncertain));
                    }
                });
            }
        }
        suite.run("a newer successful Save does not authorize an unsuccessful review checkpoint", [] {
            for (const auto outcome : {Outcome::Failed, Outcome::Uncertain}) {
                FileSession file("id", "file", draft());
                CHECK(file.beginMarkResolved(1) == Transition::Applied);
                CHECK(file.recordCheckpoint(1, outcome) == Transition::Applied);
                CHECK(file.replaceDraft(1, draft("newer")) == Transition::Applied);
                CHECK(file.recordCheckpoint(file.current().id, Outcome::Succeeded) == Transition::Applied);
                CHECK(file.recordWrite(1, Outcome::Succeeded) == Transition::InvalidOutcome);
                CHECK(!file.written() && !file.facts().resolved);
            }
        });
        suite.run("a newer review rejects superseded publication callbacks", [] {
            FileSession file("id", "file", draft());
            CHECK(file.beginMarkResolved(1) == Transition::Applied);
            CHECK(file.recordCheckpoint(1, Outcome::Succeeded) == Transition::Applied);
            CHECK(file.replaceDraft(1, draft("newer")) == Transition::Applied);
            const auto newer = file.current().id;
            CHECK(file.beginMarkResolved(newer) == Transition::Applied);
            CHECK(file.recordWrite(1, Outcome::Succeeded) == Transition::StaleRevision);
            CHECK(file.recordStage(1, Outcome::Succeeded, "earlier", "100644") == Transition::StaleRevision);
            CHECK(!file.written() && !file.staged() && file.reviewed()->revision.id == newer);
        });
        suite.run("TRUCE-0029 text envelope rejects the next file, line and payload without dropping the buffer", [] {
            const auto lines = [](std::size_t count) {
                std::string text;
                text.reserve(count * 2);
                for (std::size_t i = 0; i < count; ++i) text += "x\n";
                return text;
            };
            const auto admit = [](const std::string &text) {
                FileSnapshots captured;
                captured.path = "limit.txt";
                captured.provenance.sessionId = "limits";
                captured.stage2 = SourceSnapshot{"source-object", "100644", text,
                    {text, text, {}}, "source-commit", "Source"};
                captured.launchResult.text = {text, text, {}};
                return FileSession("limit", captured.path, {text, {}, 0}, Support::Supported, {}, captured);
            };
            const auto belowLine = std::string(262143, 'a');
            const auto atLine = std::string(262144, 'a');
            const auto aboveLine = std::string(262145, 'a');
            CHECK(assessText(lines(3999)).limit == TextLimit::Within);
            CHECK(assessText(lines(4000)).limit == TextLimit::Within);
            CHECK(assessText(lines(4001)).limit == TextLimit::FileLines);
            CHECK(assessText(belowLine).limit == TextLimit::Within);
            CHECK(assessText(atLine).limit == TextLimit::Within);
            CHECK(assessText(aboveLine).limit == TextLimit::LineBytes);
            CHECK(admit(lines(3999)).support() == Support::Supported);
            CHECK(admit(lines(4000)).support() == Support::Supported);
            CHECK(admit(lines(4000)).correspondence());
            const auto tooMany = admit(lines(4001));
            CHECK(tooMany.support() == Support::Unreadable && !tooMany.correspondence());
            CHECK(tooMany.current().draft.text == lines(4001));
            CHECK(tooMany.supportDetail().find("4000") != std::string::npos);
            CHECK(admit(belowLine).support() == Support::Supported);
            CHECK(admit(atLine).support() == Support::Supported);
            CHECK(admit(atLine).current().draft.text.size() == 262144);
            const auto tooLong = admit(aboveLine);
            CHECK(tooLong.support() == Support::Unreadable && !tooLong.correspondence());
            CHECK(tooLong.current().draft.text.size() == 262145);
            CHECK(tooLong.supportDetail().find("262144") != std::string::npos);
            const auto line = std::string(262143, 'p') + "\n";
            std::string atPayload;
            for (int i = 0; i < 32; ++i) atPayload += line;
            std::string abovePayload = atPayload + line;
            CHECK(atPayload.size() == 8388608 && abovePayload.size() == 8650752);
            CHECK(assessText(atPayload).limit == TextLimit::Within);
            CHECK(assessText(abovePayload).limit == TextLimit::RecoveryPayload);
            CHECK(admit(atPayload).support() == Support::Supported);
            const auto tooHeavy = admit(abovePayload);
            CHECK(tooHeavy.support() == Support::Unreadable);
            CHECK(tooHeavy.current().draft.text.size() == abovePayload.size());
            CHECK(tooHeavy.supportDetail().find("8388608") != std::string::npos);
            FileSession file("id", "file", {lines(4000), {}, 0});
            CHECK(file.support() == Support::Supported);
            CHECK(file.editResult(1, {{file.current().draft.text.size(), file.current().draft.text.size()}, "z\n"})
                == Transition::Blocked);
            CHECK(file.current().draft.text == lines(4000));
            CHECK(file.editResult(1, {{0, 1}, "y"}) == Transition::Applied);
            CHECK(file.current().draft.text.starts_with("y"));
            CHECK(file.beginMarkResolved(file.current().id) == Transition::Applied);
            FileSession narrow("id", "file", {atLine, {}, 0});
            CHECK(narrow.editResult(1, {{atLine.size(), atLine.size()}, "z"}) == Transition::Blocked);
            CHECK(narrow.current().draft.text == atLine);
            CHECK(narrow.beginMarkResolved(1) == Transition::Applied);
        });
        return suite.report();
    }
};

int main() { return SessionStateTests{}.run(); }
