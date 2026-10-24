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
            decisions.unresolvedMarkers = 1;
            CHECK(file.replaceDraft(current, decisions) == Transition::Applied);
            CHECK(file.current().result == textRevision.result);
            CHECK(file.current().decisions == file.current().id);
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
            undecided.unresolvedMarkers = 1;
            CHECK(file.replaceDraft(1, undecided) == Transition::Applied);
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
            }
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
        return suite.report();
    }
};

int main() { return SessionStateTests{}.run(); }
