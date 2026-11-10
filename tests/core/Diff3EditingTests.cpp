#include "core/EditingAssertions.h"

namespace {
// Opens the one diff3 block Git wrote for these versions, checks that Git's
// separator is zero-based line `separator`, then keeps each side in turn by
// accepting it and ignoring the other. Mark resolved applies once both are
// decided, and undo, redo and a replay of the drafts give the same text.
void resolvesDiff3Block(const std::string &left, const std::string &earlier, const std::string &right,
                        const std::string &merged, std::size_t separator)
{
    for (const auto kept : {SourceSide::Stage2, SourceSide::Stage3}) {
        const auto other = kept == SourceSide::Stage2 ? SourceSide::Stage3 : SourceSide::Stage2;
        auto file = test_support::diff3File(left, right, earlier, merged);
        const auto launch = file.current().draft;
        const auto scan = file.markers();
        CHECK(scan.orphans.empty() && scan.blocks.size() == 1);
        CHECK(scan.blocks.front().separator.line == separator);
        CHECK(launch.unresolvedMarkers == 1 && file.markerSections().size() == 1);
        CHECK(choose(file, kept, ChoiceAction::Accept) == Transition::Applied);
        const auto partly = file.current().draft;
        // Accepting a deletion removes the other side's text, but the block stays.
        const auto held = file.markers();
        CHECK(partly.unresolvedMarkers == 1 && held.orphans.empty() && file.markerSections().size() == 1);
        CHECK(has(file, Blocker::Undecided) && has(file, Blocker::UnresolvedMarkers));
        CHECK(choose(file, other, ChoiceAction::Ignore) == Transition::Applied);
        const auto chosen = file.current().draft;
        CHECK(chosen.text == (kept == SourceSide::Stage2 ? left : right));
        CHECK(chosen.unresolvedMarkers == 0 && file.markerSections().empty());
        CHECK(!has(file, Blocker::UnresolvedMarkers) && !has(file, Blocker::Undecided));
        CHECK(file.undo(file.current().id) == Transition::Applied);
        CHECK(file.current().draft == partly && file.markers() == held);
        CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == chosen);
        CHECK(file.beginMarkResolved(file.current().id) == Transition::Applied);
        auto replayed = test_support::diff3File(left, right, earlier, merged);
        CHECK(test_support::replay(replayed, partly) == Transition::Applied);
        CHECK(replayed.current().draft == partly && replayed.markers() == held);
        CHECK(test_support::replay(replayed, chosen) == Transition::Applied);
        CHECK(replayed.current().draft == chosen && replayed.markers().count() == 0);
        CHECK(replayed.beginMarkResolved(replayed.current().id) == Transition::Applied);
    }
}

void aHeadingUnderlineInTheDiff3BaseIsContent()
{
    auto file = test_support::underlinedBaseFile();
    const auto launch = file.current().draft;
    const auto scan = file.markers();
    CHECK(scan.orphans.empty() && scan.blocks.size() == 1);
    CHECK(engaged(scan.blocks.front().base).line == 5 && scan.blocks.front().separator.line == 9);
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
}

void aDiff3BlockWithoutMarkerLikeBaseTextIsAssembledAsBefore()
{
    const std::string left = "intro\nleft line\ntail\n";
    const std::string launch =
        "intro\n<<<<<<< ours\nleft line\n||||||| base\nbase line\n=======\nright line\n>>>>>>> theirs\ntail\n";
    auto file = test_support::diff3File(left, "intro\nright line\ntail\n", "intro\nbase line\ntail\n", launch);
    auto plain = test_support::diff3File(left, "intro\nright line\ntail\n", "intro\nbase line\ntail\n", launch, false);
    CHECK(file.markers() == plain.markers() && file.markers() == scanMarkers(launch));
    CHECK(file.markerSections() == plain.markerSections() && file.markerSections().size() == 1);
    CHECK(choose(file, SourceSide::Stage2, ChoiceAction::ResolveUsingSide) == Transition::Applied);
    CHECK(file.current().draft.text == left && file.current().draft.unresolvedMarkers == 0);
    CHECK(file.beginMarkResolved(file.current().id) == Transition::Applied);
}

void aDiff3BaseEndingInAHeadingUnderlineIsClosedByGitsSeparator(bool deleted)
{
    auto file = test_support::underlineEndedBaseFile(true, deleted);
    const auto launch = file.current().draft;
    const auto scan = file.markers();
    CHECK(scan.orphans.empty() && scan.blocks.size() == 1);
    CHECK(engaged(scan.blocks.front().base).line == 4 && scan.blocks.front().separator.line == 7);
    CHECK(launch.unresolvedMarkers == 1 && file.markerSections().size() == 1);
    CHECK(choose(file, SourceSide::Stage2, ChoiceAction::Accept) == Transition::Applied);
    CHECK(file.current().draft.text == launch.text && file.markerSections().size() == 1);
    CHECK(choose(file, SourceSide::Stage3, ChoiceAction::Ignore) == Transition::Applied);
    const auto chosen = file.current().draft;
    CHECK(chosen.text == "intro\nLeft\n-----\ntail\n");
    CHECK(chosen.unresolvedMarkers == 0 && file.markerSections().empty());
    CHECK(!has(file, Blocker::UnresolvedMarkers) && !has(file, Blocker::Undecided));
    CHECK(file.undo(file.current().id) == Transition::Applied);
    CHECK(file.current().draft.text == launch.text && file.markers() == scan);
    CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == chosen);
    CHECK(file.beginMarkResolved(file.current().id) == Transition::Applied);
    // Without the earlier version the underline still takes the separator's place.
    auto plain = test_support::underlineEndedBaseFile(false, deleted);
    CHECK(plain.markers() == scanMarkers(launch.text));
    CHECK(plain.markers().blocks.front().separator.line == 6 && plain.markers().orphans.size() == 1);
    CHECK(plain.current().draft.unresolvedMarkers == 2 && plain.markerSections().empty());
}

void gitsSeparatorLiesBetweenABaseEndingAndARightSideStartingWithASeparatorLine()
{
    // The base's underline sits directly above Git's separator and the right
    // side's first line directly below it, three identical lines in a row.
    const std::string left = "intro\nLeft\n-----\ntail\n";
    const std::string earlier = "intro\nTitle\n=======\ntail\n";
    const std::string right = "intro\n=======\nRight\ntail\n";
    const auto merged = test_support::gitDiff3(left, earlier, right);
    CHECK(merged
          == "intro\n<<<<<<< ours\nLeft\n-----\n||||||| base\nTitle\n=======\n=======\n=======\nRight\n"
             ">>>>>>> theirs\ntail\n");
    resolvesDiff3Block(left, earlier, right, merged, 7);
}

void gitsSeparatorLiesBetweenABaseEndingInASeparatorLineAndLeftText()
{
    // The right side starts with a line the left side has too, so both copies
    // of it are equally good matches for the right side's line, and the left
    // side's deletion lies between two lines its region shares with the right.
    const std::string left = "intro\nLeft\n-----\ntail\n";
    const std::string earlier = "intro\nTitle\n=======\ntail\n";
    const std::string right = "intro\nLeft\nRight\n-----\ntail\n";
    const auto merged = test_support::gitDiff3(left, earlier, right);
    CHECK(merged
          == "intro\n<<<<<<< ours\nLeft\n-----\n||||||| base\nTitle\n=======\n=======\nLeft\nRight\n-----\n"
             ">>>>>>> theirs\ntail\n");
    resolvesDiff3Block(left, earlier, right, merged, 7);
}
}

int main()
{
    test_support::Suite suite("Diff3EditingTests");
    suite.run("a heading underline in the diff3 base is content and the block resolves",
              aHeadingUnderlineInTheDiff3BaseIsContent);
    suite.run("a diff3 block without marker-like base text is assembled as before",
              aDiff3BlockWithoutMarkerLikeBaseTextIsAssembledAsBefore);
    for (const bool deleted : {false, true})
        suite.run(titled("a diff3 base ending in a heading underline is closed by Git's separator",
                         {deleted ? "right side deletes the hunk" : "right side changes the hunk"}),
                  [=] { aDiff3BaseEndingInAHeadingUnderlineIsClosedByGitsSeparator(deleted); });
    suite.run("Git's separator lies between a base ending and a right side starting with a separator line",
              gitsSeparatorLiesBetweenABaseEndingAndARightSideStartingWithASeparatorLine);
    suite.run("Git's separator lies between a base ending in a separator line and left text",
              gitsSeparatorLiesBetweenABaseEndingInASeparatorLineAndLeftText);
    return suite.report();
}
