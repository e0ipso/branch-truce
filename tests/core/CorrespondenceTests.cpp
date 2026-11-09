// Correspondence: the algorithm contract and the map's input checks.
#include "TestHarness.h"
#include "core/Correspondence.h"
#include "core/ResourceLimits.h"
#include <array>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

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
    return {1, sources(source, result), result};
}

ResultAnchor bound(CorrespondenceMap &map, SourceAnchor source)
{
    const auto id = source.contributionId;
    CHECK(map.bind(std::move(source)));
    return *map.anchor(id);
}

// The text after a choice replaces the anchor's range.
CorrespondenceMap chosen(const CorrespondenceMap &map, const std::string &id, const std::string &replacement)
{
    return map.edited(map.revision() + 1, {*map.anchor(id)->result, replacement}, false, id);
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
    const auto &current = *map.anchor("choice");
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
    CHECK(typed.anchor("choice")->manualOverlap && typed.anchor("choice")->handEdited);
    CHECK(chosen(typed, "choice", "accepted\n").text() == "head\naccepted\nend\n");
}

void deletedAnchorsNeverReattachToAnotherRepeat()
{
    auto map = mapped("head\nrepeat\nrepeat\nend\n", "head\nrepeat\nrepeat\nend\n");
    (void)bound(map, {"choice", SourceSide::Stage2, {12, 19}});
    const auto deleted = map.edited(2, {{12, 19}, {}});
    CHECK(deleted.anchor("choice")->state == AnchorState::Invalidated && !deleted.anchor("choice")->result);
    const auto whole = map.recomputed(3, "head\nrepeat\nend\n");
    CHECK(whole.anchor("choice")->state == AnchorState::Ambiguous && !whole.anchor("choice")->result);
}

void insertionAnchorsRespectAffinity()
{
    auto map = mapped("head\ntail\n", "head\ntail\n");
    (void)bound(map, {"choice", SourceSide::Stage2, {5, 5}, InsertionAffinity::Before});
    (void)bound(map, {"after", SourceSide::Stage2, {5, 5}, InsertionAffinity::After});
    const auto edited = map.edited(2, {{5, 5}, "manual\n"});
    CHECK(edited.anchor("choice")->result == ByteRange{5, 5} && edited.anchor("after")->result == ByteRange{12, 12});
    const auto choice = chosen(edited, "choice", "choice\n");
    CHECK(choice.text() == "head\nchoice\nmanual\ntail\n");
    // Undo and redo restore a map at a fresh revision with the same anchors.
    const auto restored = edited.atRevision(9);
    CHECK(restored.anchor("choice")->revision == 9 && restored.anchor("choice")->result == ByteRange{5, 5});
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
    suite.run("the map checks every text before the algorithm sees it", theMapChecksEveryTextBeforeTheAlgorithmSeesIt);
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
