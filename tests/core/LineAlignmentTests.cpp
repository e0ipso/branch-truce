// Line alignment: Myers' path within its changed-line cap, the banded fallback over it, and both limits.
#include "TestHarness.h"
#include "core/Correspondence.h"
#include "core/ExactLineCorrespondence.h"
#include "core/PassageTestText.h"
#include "core/ResourceLimits.h"
#include "core/internal/LineAlignment.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace branch_truce;

namespace {
std::vector<Passage> oneAmbiguousPassage(std::string_view source, std::string_view result)
{
    return {{{0, source.size()}, {0, result.size()}, PassageKind::Ambiguous}};
}

void pairsOverTheChangedLineCapFallBackToTheBandedAlignment()
{
    // 8000 fully different lines against 8000 are 16,000 changed lines, far
    // over Myers' cap of kMaxAlignmentCells / 16,000. The banded alignment's
    // 8000 squared cells fit, so it aligns the pair as before.
    const auto source = uniqueLines("source line ", 8000);
    const auto result = uniqueLines("result line ", 8000);
    CHECK(kMaxAlignmentCells / 16000 < 16000 && 8000UL * 8000UL <= kMaxAlignmentCells);
    // No line is shared, so the banded alignment makes both whole texts one
    // Changed passage, as 1.5.2 did.
    CHECK(!detail::myersPassages(source, result));
    CHECK(ExactLineCorrespondence{}.compare(source, result)
          == std::vector<Passage>{{{0, source.size()}, {0, result.size()}, PassageKind::Changed}});
    CHECK_EQ(ExactLineCorrespondence{}.cost(source, result), 16000UL);
}

void pairsOverBothLimitsAreOneAmbiguousPassage()
{
    // A shared first and last line around 8193 different lines against 8193
    // do not narrow the band. Its cells are still the 8195 squared line
    // pairs, over kMaxAlignmentCells, and the 16,386 changed lines are over
    // Myers' cap too.
    const auto source = "first\n" + uniqueLines("unique line ", 8193) + "last\n";
    const auto result = "first\n" + uniqueLines("other line ", 8193) + "last\n";
    CHECK(8195UL * 8195UL > kMaxAlignmentCells);
    CHECK(!detail::myersPassages(source, result));
    CHECK(ExactLineCorrespondence{}.compare(source, result) == oneAmbiguousPassage(source, result));
    CHECK_EQ(ExactLineCorrespondence{}.cost(source, result), 8193UL + 8193UL);
}

void myersAlignsUpToItsChangedLineCap()
{
    // 8192 lines against 8192 between a shared "head" and "tail": 2048 lines
    // only in the source, then 6144 in both, then 2048 only in the result.
    // The 4096 changed lines are exactly the cap.
    const auto sourceOnly = uniqueLines("source line ", 2048);
    const auto common = uniqueLines("common line ", 6144);
    const auto resultOnly = uniqueLines("result line ", 2048);
    const auto source = "head\n" + sourceOnly + common + "tail\n";
    const auto result = "head\n" + common + resultOnly + "tail\n";
    CHECK_EQ(kMaxAlignmentCells / (8192 + 8192), 4096UL);
    const auto middle = 5 + sourceOnly.size();
    const std::vector<Passage> expected{
        {{0, 5}, {0, 5}, PassageKind::Equal},
        {{5, middle}, {5, 5}, PassageKind::SourceOnly},
        {{middle, source.size() - 5}, {5, 5 + common.size()}, PassageKind::Equal},
        {{source.size() - 5, source.size() - 5}, {5 + common.size(), result.size() - 5}, PassageKind::ResultOnly},
        {{source.size() - 5, source.size()}, {result.size() - 5, result.size()}, PassageKind::Equal}};
    const auto atCap = detail::myersPassages(source, result);
    CHECK(atCap && *atCap == expected);
    // The banded fallback is over its limit for this pair, so these passages
    // come from Myers.
    CHECK(8194UL * 8194UL > kMaxAlignmentCells);
    CHECK(ExactLineCorrespondence{}.compare(source, result) == expected);
    // Without the last common line in the result, that line is changed too.
    // That makes 4097 changed lines, and 8192 against 8191 lines keep the
    // cap at 4096.
    // The search above went as deep and then traced both paths, so Myers'
    // work budget is not what stops this one. The banded fallback's 8194 by
    // 8193 cells are still over its limit, so the pair is one Ambiguous
    // passage.
    const auto shorter = "head\n" + uniqueLines("common line ", 6143) + resultOnly + "tail\n";
    CHECK_EQ(kMaxAlignmentCells / (8192 + 8191), 4096UL);
    CHECK(!detail::myersPassages(source, shorter));
    CHECK(8194UL * 8193UL > kMaxAlignmentCells);
    CHECK(ExactLineCorrespondence{}.compare(source, shorter) == oneAmbiguousPassage(source, shorter));
}

// The passages of Myers' path for a pair within its cap, after checking that
// the banded path gives the same kinds and byte ranges. A difference fails
// with both texts and both passage lists.
std::vector<Passage> passagesOfBothPaths(const std::string &source, const std::string &result)
{
    const auto banded = detail::hirschbergPassages(source, result);
    const auto myers = detail::myersPassages(source, result);
    if (myers && *myers == banded) return *myers;
    throw std::runtime_error("Myers and the banded alignment differ\nsource\n" + source + "result\n" + result
                             + "banded\n" + show(banded) + "myers\n" + (myers ? show(*myers) : "over its cap\n"));
}

void myersAndTheBandedAlignmentGiveTheSamePassages()
{
    // Every pair of the brute-force corpus that CorrespondenceTests.cpp checks
    // against the shared matches.
    const auto edges = allTexts(2);
    const auto middles = allTexts(4);
    for (const auto &prefix : edges)
        for (const auto &suffix : edges)
            for (const auto &sourceMiddle : middles)
                for (const auto &resultMiddle : middles) {
                    auto source = prefix;
                    auto result = prefix;
                    source.insert(source.end(), sourceMiddle.begin(), sourceMiddle.end());
                    result.insert(result.end(), resultMiddle.begin(), resultMiddle.end());
                    source.insert(source.end(), suffix.begin(), suffix.end());
                    result.insert(result.end(), suffix.begin(), suffix.end());
                    (void)passagesOfBothPaths(joined(source), joined(result));
                }
    // Long texts of two to four distinct lines, the result with edits spread
    // over the whole text, so ties and ambiguous gaps are everywhere and the
    // shared edges do not narrow the band. mt19937 gives the same numbers on
    // every platform, so the pairs are the same on every run.
    std::mt19937 random(20261119);
    const std::array<std::string, 4> alphabet{"a\n", "b\n", "c\n", "d\n"};
    for (int pair = 0; pair < 12; ++pair) {
        const auto letters = 2 + random() % 3;
        std::vector<std::string> source(400 + random() % 801);
        for (auto &line : source) line = alphabet[random() % letters];
        auto result = source;
        for (auto edits = 1 + random() % 80; edits > 0; --edits) {
            const auto at = random() % (result.size() + 1);
            const auto &line = alphabet[random() % letters];
            const auto position = result.begin() + static_cast<std::ptrdiff_t>(at);
            switch (random() % 3) {
            case 0: result.insert(position, line); break;
            case 1:
                if (at < result.size()) result.erase(position);
                break;
            default:
                if (at < result.size()) result[at] = line;
            }
        }
        // Within the banded alignment's limit whatever its band.
        CHECK(source.size() * result.size() <= kMaxAlignmentCells);
        const auto passages = passagesOfBothPaths(joined(source), joined(result));
        CHECK(std::any_of(passages.begin(), passages.end(),
                          [](const auto &passage) { return passage.kind == PassageKind::Ambiguous; }));
    }
}
}

int main()
{
    test_support::Suite suite("LineAlignmentTests");
    suite.run("pairs over the changed-line cap fall back to the banded alignment",
              pairsOverTheChangedLineCapFallBackToTheBandedAlignment);
    suite.run("pairs over both limits are one ambiguous passage", pairsOverBothLimitsAreOneAmbiguousPassage);
    suite.run("myers aligns up to its changed-line cap", myersAlignsUpToItsChangedLineCap);
    suite.run("myers and the banded alignment give the same passages", myersAndTheBandedAlignmentGiveTheSamePassages);
    return suite.report();
}
