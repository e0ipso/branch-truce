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

FileSnapshots sources(const std::string &text)
{
    FileSnapshots captured;
    captured.path = "contract.txt";
    captured.provenance.sessionId = "contract";
    captured.stage2 = SourceSnapshot{"object", "100644", text, {text, text, {}}, "commit", "Source"};
    captured.launchResult.text = {text, text, {}};
    return captured;
}

bool rejected(const std::function<void()> &build)
{
    try {
        build();
    } catch (const std::invalid_argument &) {
        return true;
    }
    return false;
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
    return suite.report();
}
