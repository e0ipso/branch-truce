#include "app/NavigationTargets.h"
#include "TestHarness.h"
#include "fixtures/ChoiceFile.h"
#include <algorithm>

using namespace branch_truce;

namespace {
bool at(const ScopeTarget &target, std::size_t begin, std::size_t end, SourceSide side)
{
    return target.byteBegin == begin && target.byteEnd == end && target.resultRange && target.side == side;
}

const std::string &lineChoice(const FileSession &file, SourceSide side, std::size_t occurrence = 0)
{
    return test_support::choiceTarget(file, side, ChoiceScope::Line, occurrence).source.contributionId;
}

// Both sides change the first and the last line; the result is the left side.
FileSession edgeFile()
{
    return test_support::choiceFile("left start\nmiddle\nleft end\n", "right start\nmiddle\nright end\n");
}
}

int main()
{
    test_support::Suite suite("navigation-targets");

    // TRUCE-0204: the target builders are pure functions of the file.
    suite.run("a file without a correspondence or without differences has no targets", [] {
        const FileSession plain("plain", "plain.txt", {"text\n", {}, 0});
        CHECK(!plain.correspondence());
        CHECK(differenceTargets(plain).empty());
        CHECK(undecidedTargets(plain).empty());
        const auto same = test_support::choiceFile("same\n", "same\n");
        CHECK(same.correspondence() && same.choices().empty());
        CHECK(differenceTargets(same).empty());
        CHECK(undecidedTargets(same).empty());
    });

    suite.run("differences at the start and end of the result are one target each, in result order", [] {
        const auto file = edgeFile();
        const auto &result = file.current().draft.text;
        const auto targets = differenceTargets(file);
        CHECK(targets.size() == 2);
        CHECK(at(targets.front(), 0, 11, SourceSide::Stage3));
        CHECK(at(targets.back(), 18, result.size(), SourceSide::Stage3));
        CHECK(targets.front().contributionId.empty() && targets.back().contributionId.empty());
    });

    suite.run("undecided lines at the start and end of the result stand in for their sections", [] {
        const auto file = edgeFile();
        const auto &result = file.current().draft.text;
        const auto targets = undecidedTargets(file);
        // Each side has one line choice per region, so no section is a target.
        CHECK(targets.size() == 4);
        CHECK(at(targets[0], 0, 11, SourceSide::Stage2) && targets[0].contributionId == lineChoice(file, SourceSide::Stage2));
        CHECK(at(targets[1], 0, 11, SourceSide::Stage3) && targets[1].contributionId == lineChoice(file, SourceSide::Stage3));
        CHECK(at(targets[2], 18, result.size(), SourceSide::Stage2)
              && targets[2].contributionId == lineChoice(file, SourceSide::Stage2, 1));
        CHECK(at(targets[3], 18, result.size(), SourceSide::Stage3)
              && targets[3].contributionId == lineChoice(file, SourceSide::Stage3, 1));
        for (const auto &target : file.choices()) {
            CHECK(undecidedChoice(file, target));
            CHECK(sectionHasLine(file, target));
        }
    });

    suite.run("an undecided choice next to a decided one stays a target and the decided one does not", [] {
        auto file = test_support::markedFile();
        const auto before = undecidedTargets(file);
        CHECK(before.size() == 6);
        const auto &left = test_support::choiceTarget(file, SourceSide::Stage2, ChoiceScope::Section);
        CHECK(applyChoice(file, previewChoice(file, file.current().id, left.source.contributionId,
            ChoiceAction::Accept), true) == Transition::Applied);
        const auto after = undecidedTargets(file);
        CHECK(after.size() == 4);
        for (std::size_t occurrence = 0; occurrence < 2; ++occurrence) {
            const auto &decided = test_support::choiceTarget(file, SourceSide::Stage2, ChoiceScope::Line, occurrence);
            CHECK(!undecidedChoice(file, decided));
            CHECK(std::none_of(after.begin(), after.end(), [&](const auto &target) {
                return target.contributionId == decided.source.contributionId;
            }));
        }
        // The right side of the same region is still open, ahead of the later region.
        CHECK(after[0].contributionId == lineChoice(file, SourceSide::Stage3));
        CHECK(after[1].contributionId == lineChoice(file, SourceSide::Stage3, 1));
        CHECK(after[0].side == SourceSide::Stage3 && after[1].side == SourceSide::Stage3);
        CHECK(undecidedChoice(file, test_support::choiceTarget(file, SourceSide::Stage3, ChoiceScope::Line)));
        CHECK(after[0].byteBegin == before[2].byteBegin && after[0].byteEnd == before[2].byteEnd);
        CHECK(std::is_sorted(after.begin(), after.end(), [](const auto &l, const auto &r) { return l.byteBegin < r.byteBegin; }));
    });

    suite.run("utf16Offset counts UTF-16 units up to a byte offset and refuses one past the end", [] {
        const std::string text = "a\xc3\xa9\xf0\x9f\x98\x80z";
        CHECK(utf16Offset(text, 0) == 0);
        CHECK(utf16Offset(text, 1) == 1);
        CHECK(utf16Offset(text, 3) == 2);
        CHECK(utf16Offset(text, 7) == 4);
        CHECK(utf16Offset(text, text.size()) == 5);
        CHECK(utf16Offset(text, text.size() + 1) == -1);
    });

    return suite.report();
}
