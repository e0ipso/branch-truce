#include "core/WordDifferences.h"
#include "TestHarness.h"
#include "fixtures/ChoiceFile.h"

#include <string>
#include <string_view>
#include <vector>

using namespace branch_truce;

namespace {
std::vector<std::string> words(std::string_view text, const std::vector<ByteRange> &ranges)
{
    std::vector<std::string> found;
    for (const auto &range : ranges) found.emplace_back(text.substr(range.begin, range.end - range.begin));
    return found;
}

const std::vector<ByteRange> &sourceRanges(const FileWordDifferences &found, SourceSide side)
{
    static const std::vector<ByteRange> none;
    const auto entry = found.sources.find(side);
    return entry == found.sources.end() ? none : entry->second;
}
}

int main()
{
    test_support::Suite suite("WordDifferencesTests");

    suite.run("a line pair with one substituted and one inserted token", [] {
        const std::string left = "alpha beta gamma\n";
        const std::string right = "alpha delta gamma epsilon\n";
        std::size_t budget = kMaxWordDifferenceCells;
        const auto found = wordDifferences(left, right, budget);
        CHECK(found.left == std::vector<ByteRange>{{6, 10}});
        CHECK(found.right == (std::vector<ByteRange>{{6, 11}, {18, 25}}));
        CHECK(words(left, found.left) == std::vector<std::string>{"beta"});
        CHECK(words(right, found.right) == (std::vector<std::string>{"delta", "epsilon"}));
        // Only the differing middle is compared: three tokens by five.
        CHECK(budget == kMaxWordDifferenceCells - 15);
    });

    suite.run("identical, inserted-only and multibyte pairs", [] {
        std::size_t budget = kMaxWordDifferenceCells;
        CHECK(wordDifferences("same line\n", "same line\n", budget) == WordDifferences{});
        const std::string grown = "call(x, y)\n";
        const auto inserted = wordDifferences("call(x)\n", grown, budget);
        CHECK(inserted.left.empty());
        CHECK(words(grown, inserted.right) == std::vector<std::string>{", y"});
        const std::string cafe = "caf\xc3\xa9 au lait\n";
        const std::string noir = "caf\xc3\xa9 noir\n";
        const auto multibyte = wordDifferences(cafe, noir, budget);
        CHECK(words(cafe, multibyte.left) == std::vector<std::string>{"au lait"});
        CHECK(words(noir, multibyte.right) == std::vector<std::string>{"noir"});
        // Indentation alone is a difference, kept although it is only blanks.
        const std::string indented = "    value\n";
        const auto blanks = wordDifferences("  value\n", indented, budget);
        CHECK(words(indented, blanks.right) == std::vector<std::string>{"    "});
    });

    suite.run("a line over the documented limit gets no word ranges", [] {
        const std::string longLeft(kMaxWordDifferenceLineBytes + 1, 'x');
        const std::string longRight(kMaxWordDifferenceLineBytes + 1, 'y');
        const auto left = longLeft + "\nshort one\n";
        const auto right = longRight + "\nshort two\n";
        std::size_t budget = kMaxWordDifferenceCells;
        const auto found = wordDifferences(left, right, budget);
        CHECK(words(left, found.left) == std::vector<std::string>{"one"});
        CHECK(words(right, found.right) == std::vector<std::string>{"two"});
        // A line of exactly the limit is still split into tokens.
        const auto atLimit = std::string(kMaxWordDifferenceLineBytes - 2, 'a') + " b";
        const auto other = std::string(kMaxWordDifferenceLineBytes - 2, 'a') + " c";
        const auto bounded = wordDifferences(atLimit, other, budget);
        CHECK(bounded.left == std::vector<ByteRange>{{kMaxWordDifferenceLineBytes - 1, kMaxWordDifferenceLineBytes}});
        CHECK(bounded.right == bounded.left);
    });

    suite.run("a pair over the comparison budget gets no ranges and keeps the budget", [] {
        std::string left;
        std::string right;
        // 300 short lines a side: over 4000 tokens each, far past the pair limit.
        for (int i = 0; i < 300; ++i) {
            left += "a a a a a a a\n";
            right += "b b b b b b b\n";
        }
        std::size_t budget = kMaxWordDifferenceCells;
        CHECK(wordDifferences(left, right, budget) == WordDifferences{});
        CHECK(budget == kMaxWordDifferenceCells);
        std::size_t small = 3;
        CHECK(wordDifferences("one two\n", "six ten\n", small) == WordDifferences{});
        CHECK(small == 3);
    });

    suite.run("undecided competing sections pair with each other in the sources and the result", [] {
        const auto marked = test_support::markedText();
        auto file = test_support::choiceFile("header\nleft first\nleft second\nbridge\nleft later\ntail\n",
            "header\nright first\nright second\nbridge\nright later\ntail\n", marked);
        const auto found = fileWordDifferences(file);
        const auto &left = file.snapshots()->stage2->worktree.utf8;
        const auto &right = file.snapshots()->stage3->worktree.utf8;
        CHECK(words(left, sourceRanges(found, SourceSide::Stage2)) == (std::vector<std::string>{"left", "left", "left"}));
        CHECK(words(right, sourceRanges(found, SourceSide::Stage3)) == (std::vector<std::string>{"right", "right", "right"}));
        CHECK(words(file.correspondence()->text(), found.result)
            == (std::vector<std::string>{"left", "left", "right", "right", "left", "right"}));
        CHECK(!found.sources.contains(SourceSide::Earlier));
    });

    suite.run("a changed passage pairs its source lines with the result lines", [] {
        const std::string left = "header\nvalue = one\ntail\n";
        const std::string right = "header\nvalue = two\ntail\n";
        const std::string result = "header\nvalue = three\ntail\n";
        auto file = test_support::choiceFile(left, right, result);
        // Decide both sides, so only the passages against the result remain.
        auto decisions = file.current().draft.decisions;
        for (auto &[id, decision] : decisions) decision.value = DecisionValue::Ignored;
        CHECK(file.replaceDraft(file.current().id, {result, decisions, 0}) == Transition::Applied);
        const auto found = fileWordDifferences(file);
        CHECK(words(left, sourceRanges(found, SourceSide::Stage2)) == std::vector<std::string>{"one"});
        CHECK(words(right, sourceRanges(found, SourceSide::Stage3)) == std::vector<std::string>{"two"});
        // Both sides differ from the result at the same word; it is one range.
        CHECK(words(file.correspondence()->text(), found.result) == std::vector<std::string>{"three"});
        // A file without captures has nothing to compare.
        FileSession bare("bare", "bare.txt", {"text\n", {}, 0});
        CHECK(fileWordDifferences(bare) == FileWordDifferences{});
    });

    return suite.report();
}
