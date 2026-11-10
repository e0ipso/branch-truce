// Correspondence: the algorithm contract, exact line passages, anchors and the map's input checks.
#include "TestHarness.h"
#include "core/Correspondence.h"
#include "core/ExactLineCorrespondence.h"
#include "core/ResourceLimits.h"
#include <array>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace branch_truce;

namespace {
// The least an implementation may do: the whole of both texts as one
// passage, Equal when they match and otherwise Ambiguous.
class WholeTextCorrespondence final : public CorrespondenceAlgorithm {
public:
    [[nodiscard]] std::vector<Passage> compare(std::string_view source, std::string_view result) const override
    {
        ++calls;
        return {
            {{0, source.size()}, {0, result.size()}, source == result ? PassageKind::Equal : PassageKind::Ambiguous}};
    }
    [[nodiscard]] std::string_view identity() const noexcept override { return "whole-text"; }
    mutable int calls = 0;
};

// The algorithm contract of Correspondence.h, for any implementation.
void keepsTheContract(const CorrespondenceAlgorithm &algorithm)
{
    const std::array<std::pair<std::string_view, std::string_view>, 7> cases{{
        {"", ""},
        {"same\n", "same\n"},
        {"head\nold\nend\n", "head\nnew\nend\n"},
        {"head\nend\n", "head\ninserted\nend\n"},
        {"head\nrepeat\nrepeat\nend\n", "head\nrepeat\nend\n"},
        {"no final newline", "no final newline\n"},
        {"\xc3\xa9t\xc3\xa9\n", "\xf0\x9f\x98\x80\n"},
    }};
    for (const auto &[source, result] : cases) {
        const auto passages = algorithm.compare(source, result);
        std::size_t sourceEnd = 0;
        std::size_t resultEnd = 0;
        for (const auto &passage : passages) {
            CHECK(passage.source.begin == sourceEnd && passage.result.begin == resultEnd);
            CHECK(validByteRange(source, passage.source) && validByteRange(result, passage.result));
            if (passage.kind == PassageKind::Equal)
                CHECK(slice(source, passage.source) == slice(result, passage.result));
            sourceEnd = passage.source.end;
            resultEnd = passage.result.end;
        }
        CHECK(sourceEnd == source.size() && resultEnd == result.size());
        CHECK(algorithm.compare(source, result) == passages);
    }
    // Two equally good places for one repeated line are never a guess.
    for (const auto &passage : algorithm.compare("repeat\n", "repeat\nrepeat\n"))
        CHECK(passage.kind != PassageKind::Equal || passage.result.end - passage.result.begin != 7);
}

void implementationsKeepTheAlgorithmContract()
{
    keepsTheContract(ExactLineCorrespondence{});
    keepsTheContract(WholeTextCorrespondence{});
}

const char *name(PassageKind kind)
{
    switch (kind) {
    case PassageKind::Equal: return "equal";
    case PassageKind::Changed: return "changed";
    case PassageKind::SourceOnly: return "source only";
    case PassageKind::ResultOnly: return "result only";
    case PassageKind::Ambiguous: break;
    }
    return "ambiguous";
}

std::string show(const std::vector<Passage> &passages)
{
    std::string text;
    for (const auto &passage : passages) {
        text += name(passage.kind);
        text += ' ';
        text += std::to_string(passage.source.begin);
        text += '-';
        text += std::to_string(passage.source.end);
        text += ' ';
        text += std::to_string(passage.result.begin);
        text += '-';
        text += std::to_string(passage.result.end);
        text += '\n';
    }
    return text;
}

// ExactLineCorrespondence gives exactly `expected`, every time, as a partition
// of both texts whose equal passages cover identical bytes.
void expectPassages(std::string_view source, std::string_view result, const std::vector<Passage> &expected)
{
    const ExactLineCorrespondence algorithm;
    const auto actual = algorithm.compare(source, result);
    if (actual != expected) {
        throw std::runtime_error("passages differ\nactual\n" + show(actual) + "expected\n" + show(expected));
    }
    CHECK(actual == expected);
    for (int repeat = 0; repeat < 4; ++repeat) CHECK(algorithm.compare(source, result) == actual);
    std::size_t sourceEnd = 0;
    std::size_t resultEnd = 0;
    for (const auto &passage : actual) {
        CHECK(passage.source.begin == sourceEnd && passage.result.begin == resultEnd);
        CHECK(validByteRange(source, passage.source) && validByteRange(result, passage.result));
        if (passage.kind == PassageKind::Equal) CHECK(slice(source, passage.source) == slice(result, passage.result));
        sourceEnd = passage.source.end;
        resultEnd = passage.result.end;
    }
    CHECK(sourceEnd == source.size() && resultEnd == result.size());
}

void partitionsAreDeterministicForEachKindOfPassage()
{
    expectPassages("repeat\nrepeat\n", "repeat\nrepeat\n", {{{0, 14}, {0, 14}, PassageKind::Equal}});
    expectPassages("head\nold\nend\n", "head\nnew\nend\n",
                   {{{0, 5}, {0, 5}, PassageKind::Equal},
                    {{5, 9}, {5, 9}, PassageKind::Changed},
                    {{9, 13}, {9, 13}, PassageKind::Equal}});
    expectPassages("head\nend\n", "head\ninserted\nend\n",
                   {{{0, 5}, {0, 5}, PassageKind::Equal},
                    {{5, 5}, {5, 14}, PassageKind::ResultOnly},
                    {{5, 9}, {14, 18}, PassageKind::Equal}});
    expectPassages("head\ndeleted\nend\n", "head\nend\n",
                   {{{0, 5}, {0, 5}, PassageKind::Equal},
                    {{5, 13}, {5, 5}, PassageKind::SourceOnly},
                    {{13, 17}, {5, 9}, PassageKind::Equal}});
    expectPassages("head\nrepeat\nrepeat\nend\n", "head\nrepeat\nend\n",
                   {{{0, 5}, {0, 5}, PassageKind::Equal},
                    {{5, 19}, {5, 12}, PassageKind::Ambiguous},
                    {{19, 23}, {12, 16}, PassageKind::Equal}});
    expectPassages("head\none\ntwo\nend\n", "head\ntwo\none\nend\n",
                   {{{0, 5}, {0, 5}, PassageKind::Equal},
                    {{5, 13}, {5, 13}, PassageKind::Ambiguous},
                    {{13, 17}, {13, 17}, PassageKind::Equal}});
}

void aLongUniqueTextAlignsAsOneEqualPassage()
{
    std::string source;
    for (int line = 0; line < 3500; ++line) {
        source += "unique line ";
        source += std::to_string(line);
        source += '\n';
    }
    const auto result = "manual prefix\n" + source + "manual suffix\n";
    const auto passages = ExactLineCorrespondence{}.compare(source, result);
    CHECK(passages
          == std::vector<Passage>{
              {{0, 0}, {0, 14}, PassageKind::ResultOnly},
              {{0, source.size()}, {14, 14 + source.size()}, PassageKind::Equal},
              {{source.size(), source.size()}, {14 + source.size(), result.size()}, PassageKind::ResultOnly}});
}

void textsOverTheCellBudgetAreOneAmbiguousPassage()
{
    // 8193 lines against 8193 is just over kMaxAlignmentCells.
    std::string source;
    for (int line = 0; line < 8193; ++line) source += "unique line " + std::to_string(line) + '\n';
    CHECK(8193U * 8193U > kMaxAlignmentCells && 8192U * 8192U <= kMaxAlignmentCells);
    auto result = source;
    result.replace(0, 14, "changed line\n");
    CHECK(ExactLineCorrespondence{}.compare(source, result)
          == std::vector<Passage>{{{0, source.size()}, {0, result.size()}, PassageKind::Ambiguous}});
    // Identical texts need no alignment, whatever their size.
    CHECK(ExactLineCorrespondence{}.compare(source, source)
          == std::vector<Passage>{{{0, source.size()}, {0, source.size()}, PassageKind::Equal}});
}

std::shared_ptr<const FileSnapshots> sources(const std::string &text, const std::string &result)
{
    FileSnapshots captured;
    captured.path = "contract.txt";
    captured.provenance.sessionId = "contract";
    captured.stage2 = SourceSnapshot{"object", "100644", text, {text, text, {}}, "commit", "Source"};
    captured.launchResult.text = {result, result, {}};
    return std::make_shared<const FileSnapshots>(std::move(captured));
}

std::shared_ptr<const FileSnapshots> sources(const std::string &text) { return sources(text, text); }

bool rejected(const std::function<void()> &build)
{
    try {
        build();
    } catch (const std::invalid_argument &) {
        return true;
    }
    return false;
}

CorrespondenceMap mapped(const std::string &source, const std::string &result)
{
    return {1, sources(source, result), result, std::make_shared<const ExactLineCorrespondence>()};
}

// The anchor bound to `id`. A missing one fails the case instead of crashing it.
ResultAnchor anchorOf(const CorrespondenceMap &map, const std::string &id)
{
    const auto *anchor = map.anchor(id);
    CHECK(anchor != nullptr);
    return *anchor;
}

ResultAnchor bound(CorrespondenceMap &map, SourceAnchor source)
{
    const auto id = source.contributionId;
    CHECK(map.bind(std::move(source)));
    return anchorOf(map, id);
}

// The text after a choice replaces the anchor's range.
CorrespondenceMap chosen(const CorrespondenceMap &map, const std::string &id, const std::string &replacement)
{
    const auto *anchor = map.anchor(id);
    if (anchor == nullptr || !anchor->result) throw std::runtime_error("the anchor has no result range");
    return map.edited(map.revision() + 1, {*anchor->result, replacement}, false, id);
}

void aDeferredEditKeepsThePassagesAroundTheEditedLines()
{
    const std::string text = "a\nb\nc\nd\n";
    auto map = mapped(text, text);
    const auto anchor = bound(map, {"c", SourceSide::Stage2, {4, 6}});
    const ResultEdit edit{{4, 4}, "x"};
    const auto deferred = map.edited(2, edit, true, {}, Realignment::Deferred);
    const auto now = map.edited(2, edit);
    CHECK(!deferred.aligned() && now.aligned() && map.aligned());
    CHECK(deferred.text() == "a\nb\nxc\nd\n" && deferred.sameAnchors(now));
    CHECK(*deferred.passages(SourceSide::Stage2)
          == std::vector<Passage>({{{0, 4}, {0, 4}, PassageKind::Equal},
                                   {{4, 6}, {4, 7}, PassageKind::Ambiguous},
                                   {{6, 8}, {7, 9}, PassageKind::Equal}}));
    const auto realigned = deferred.realigned();
    CHECK(realigned.aligned() && realigned.sameAnchors(now));
    CHECK(*realigned.passages(SourceSide::Stage2) == *now.passages(SourceSide::Stage2));
    CHECK(anchorOf(realigned, "c") == anchorOf(now, "c") && anchor.result == ByteRange{4, 6});
    // Every edit leaves a partition of both texts, at either end too.
    for (const ResultEdit other : {ResultEdit{{0, 0}, "start\n"}, ResultEdit{{8, 8}, "end"}, ResultEdit{{1, 7}, ""},
                                   ResultEdit{{0, 8}, "all new\n"}}) {
        const auto provisional = map.edited(2, other, true, {}, Realignment::Deferred);
        std::size_t source = 0;
        std::size_t result = 0;
        for (const auto &passage : *provisional.passages(SourceSide::Stage2)) {
            CHECK(passage.source.begin == source && passage.result.begin == result);
            if (passage.kind == PassageKind::Equal)
                CHECK(slice(text, passage.source) == slice(provisional.text(), passage.result));
            source = passage.source.end;
            result = passage.result.end;
        }
        CHECK(source == text.size() && result == provisional.text().size());
        CHECK(*provisional.realigned().passages(SourceSide::Stage2)
              == *map.edited(2, other).passages(SourceSide::Stage2));
    }
}

void repeatedAndReorderedPassagesLeaveAnchorsAmbiguous()
{
    for (const auto &[source, result] : {std::pair{"head\nrepeat\nrepeat\nend\n", "head\nrepeat\nend\n"},
                                         std::pair{"head\none\ntwo\nend\n", "head\ntwo\none\nend\n"}}) {
        auto map = mapped(source, result);
        const auto anchor = bound(map, {"choice", SourceSide::Stage2, {5, 12}});
        CHECK(anchor.state == AnchorState::Ambiguous && !anchor.result);
    }
}

void equalRepeatedPassagesKeepTheSelectedOccurrence()
{
    auto map = mapped("repeat\nmiddle\nrepeat\n", "manual prefix\nrepeat\nmiddle\nrepeat\n");
    const auto anchor = bound(map, {"choice", SourceSide::Stage2, {14, 21}});
    CHECK(anchor.state == AnchorState::Mapped && anchor.result == ByteRange{28, 35});
    CHECK(chosen(map, "choice", "selected\n").text() == "manual prefix\nrepeat\nmiddle\nselected\n");
}

void anchorsBindOnlyOnCharacterBoundaries()
{
    const std::string text = "start\n\xc3\xa9\xf0\x9f\x98\x80\nend\n";
    auto map = mapped(text, text);
    CHECK(!map.bind({"inside-codepoint", SourceSide::Stage2, {7, 13}}));
    CHECK(!map.bind({"", SourceSide::Stage2, {6, 13}}) && !map.bind({"other", SourceSide::Earlier, {0, 0}}));
    CHECK(bound(map, {"choice", SourceSide::Stage2, {6, 13}}).result == ByteRange{6, 13});
    CHECK(!map.bind({"choice", SourceSide::Stage2, {0, 6}}));
    CHECK(rejected([&] { (void)map.edited(2, {{7, 8}, "x"}); }));
    CHECK(rejected([&] { (void)map.edited(2, {{6, 13}, "\x80"}); }));
    CHECK(chosen(map, "choice", "valid\n").text() == "start\nvalid\nend\n");
}

void sourceOnlyAnchorsUseTheDeletionSite()
{
    auto map = mapped("head\nremoved\ntail\n", "manual prefix\nhead\ntail\nmanual suffix\n");
    const auto deleted = bound(map, {"choice", SourceSide::Stage2, {5, 13}});
    CHECK(deleted.state == AnchorState::Mapped && deleted.result == ByteRange{19, 19} && !deleted.manualOverlap);
    CHECK(chosen(map, "choice", "restored\n").text() == "manual prefix\nhead\nrestored\ntail\nmanual suffix\n");
}

void anchorsFollowManualInsertionsAndDeletions()
{
    auto map = mapped("head\nleft\nright\nend\n", "head\nleft\nright\nend\n");
    (void)bound(map, {"choice", SourceSide::Stage2, {5, 10}});
    const std::string prefix = "manual \xc3\xa9\n";
    const std::string suffix = "unrelated manual tail\n";
    map = map.edited(2, {{0, 0}, prefix});
    map = map.edited(3, {{prefix.size(), prefix.size() + 5}, {}});
    map = map.edited(4, {{map.text().size(), map.text().size()}, suffix});
    const auto &current = anchorOf(map, "choice");
    CHECK(current.revision == 4 && current.result == ByteRange{prefix.size(), prefix.size() + 5});
    CHECK(!current.manualOverlap && !current.handEdited);
    CHECK(chosen(map, "choice", "accepted\n").text() == prefix + "accepted\nright\nend\n" + suffix);
}

void launchTextThatDiffersFromItsSourceNeedsConfirmation()
{
    auto map = mapped("head\nsource\nend\n", "head\nmanual\nend\n");
    const auto anchor = bound(map, {"choice", SourceSide::Stage2, {5, 12}});
    CHECK(anchor.manualOverlap && !anchor.handEdited && anchor.result == ByteRange{5, 12});
    const auto typed = map.edited(2, {{6, 7}, "X"});
    CHECK(anchorOf(typed, "choice").manualOverlap && anchorOf(typed, "choice").handEdited);
    CHECK(chosen(typed, "choice", "accepted\n").text() == "head\naccepted\nend\n");
}

void deletedAnchorsNeverReattachToAnotherRepeat()
{
    auto map = mapped("head\nrepeat\nrepeat\nend\n", "head\nrepeat\nrepeat\nend\n");
    (void)bound(map, {"choice", SourceSide::Stage2, {12, 19}});
    const auto deleted = map.edited(2, {{12, 19}, {}});
    CHECK(anchorOf(deleted, "choice").state == AnchorState::Invalidated && !anchorOf(deleted, "choice").result);
    const auto whole = map.recomputed(3, "head\nrepeat\nend\n");
    CHECK(anchorOf(whole, "choice").state == AnchorState::Ambiguous && !anchorOf(whole, "choice").result);
}

void insertionAnchorsRespectAffinity()
{
    auto map = mapped("head\ntail\n", "head\ntail\n");
    (void)bound(map, {"choice", SourceSide::Stage2, {5, 5}, InsertionAffinity::Before});
    (void)bound(map, {"after", SourceSide::Stage2, {5, 5}, InsertionAffinity::After});
    const auto edited = map.edited(2, {{5, 5}, "manual\n"});
    CHECK(anchorOf(edited, "choice").result == ByteRange{5, 5}
          && anchorOf(edited, "after").result == ByteRange{12, 12});
    const auto choice = chosen(edited, "choice", "choice\n");
    CHECK(choice.text() == "head\nchoice\nmanual\ntail\n");
    // Undo and redo restore a map at a fresh revision with the same anchors.
    const auto restored = edited.atRevision(9);
    CHECK(anchorOf(restored, "choice").revision == 9 && anchorOf(restored, "choice").result == ByteRange{5, 5});
}

void theMapChecksEveryTextBeforeTheAlgorithmSeesIt()
{
    const auto algorithm = std::make_shared<WholeTextCorrespondence>();
    const std::string tooLong(kMaxLineBytes + 1, 'a');
    CHECK(rejected([&] { CorrespondenceMap(1, sources("ok\n"), "bad \xff\n", algorithm); }));
    CHECK(rejected([&] { CorrespondenceMap(1, sources("bad \xff\n"), "ok\n", algorithm); }));
    CHECK(rejected([&] { CorrespondenceMap(1, sources(tooLong), "ok\n", algorithm); }));
    CHECK(rejected([&] { CorrespondenceMap(1, sources("ok\n"), tooLong, algorithm); }));
    CHECK(algorithm->calls == 0);
    const CorrespondenceMap map(1, sources("ok\n"), "ok\n", algorithm);
    CHECK(algorithm->calls == 1);
    CHECK(rejected([&] { (void)map.recomputed(2, "bad \xff\n"); }));
    CHECK(rejected([&] { (void)map.recomputed(2, tooLong); }));
    CHECK(algorithm->calls == 1);
    CHECK(map.recomputed(2, "changed\n").text() == "changed\n" && algorithm->calls == 3);
}
}

int main()
{
    test_support::Suite suite("CorrespondenceTests");
    suite.run("implementations keep the algorithm contract", implementationsKeepTheAlgorithmContract);
    suite.run("correspondence partitions are deterministic for equal changed inserted deleted and ambiguous text",
              partitionsAreDeterministicForEachKindOfPassage);
    suite.run("a long unique text aligns as one equal passage between inserted lines",
              aLongUniqueTextAlignsAsOneEqualPassage);
    suite.run("texts over the cell budget are one ambiguous passage", textsOverTheCellBudgetAreOneAmbiguousPassage);
    suite.run("the map checks every text before the algorithm sees it", theMapChecksEveryTextBeforeTheAlgorithmSeesIt);
    suite.run("a deferred edit keeps the passages around the edited lines",
              aDeferredEditKeepsThePassagesAroundTheEditedLines);
    suite.run("repeated and reordered passages leave anchors ambiguous",
              repeatedAndReorderedPassagesLeaveAnchorsAmbiguous);
    suite.run("equal repeated passages keep the selected occurrence", equalRepeatedPassagesKeepTheSelectedOccurrence);
    suite.run("anchors bind only on character boundaries", anchorsBindOnlyOnCharacterBoundaries);
    suite.run("source-only anchors use the deletion site", sourceOnlyAnchorsUseTheDeletionSite);
    suite.run("anchors follow manual insertions and deletions", anchorsFollowManualInsertionsAndDeletions);
    suite.run("launch text that differs from its source needs confirmation",
              launchTextThatDiffersFromItsSourceNeedsConfirmation);
    suite.run("deleted anchors never reattach to another repeat", deletedAnchorsNeverReattachToAnotherRepeat);
    suite.run("insertion anchors respect affinity", insertionAnchorsRespectAffinity);
    return suite.report();
}
