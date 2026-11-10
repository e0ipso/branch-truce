#include "core/EditingAssertions.h"

namespace {
// The ways of deciding the first section of a marked file completely.
enum class FirstSection { ResolveLeft, ResolveRight, AcceptThenIgnore, IgnoreThenAccept, LeftFirst, RightFirst, Lines };

// Decides the first section of `file` along `path` and returns the text it keeps.
std::string decideFirstSection(FileSession &file, FirstSection path, const std::string &launch)
{
    switch (path) {
    case FirstSection::ResolveLeft:
        CHECK(choose(file, SourceSide::Stage2, ChoiceAction::ResolveUsingSide) == Transition::Applied);
        return "left first\nleft second\n";
    case FirstSection::ResolveRight:
        CHECK(choose(file, SourceSide::Stage3, ChoiceAction::ResolveUsingSide) == Transition::Applied);
        return "right first\nright second\n";
    case FirstSection::AcceptThenIgnore:
        CHECK(choose(file, SourceSide::Stage2, ChoiceAction::Accept) == Transition::Applied);
        CHECK(file.current().draft.text == launch);
        CHECK(choose(file, SourceSide::Stage3, ChoiceAction::Ignore) == Transition::Applied);
        return "left first\nleft second\n";
    case FirstSection::IgnoreThenAccept:
        CHECK(choose(file, SourceSide::Stage2, ChoiceAction::Ignore) == Transition::Applied);
        CHECK(file.current().draft.text == launch);
        CHECK(choose(file, SourceSide::Stage3, ChoiceAction::Accept) == Transition::Applied);
        return "right first\nright second\n";
    case FirstSection::LeftFirst:
        CHECK(choose(file, SourceSide::Stage2, ChoiceAction::CombineLeftFirst) == Transition::Applied);
        return "left first\nleft second\nright first\nright second\n";
    case FirstSection::RightFirst:
        CHECK(choose(file, SourceSide::Stage3, ChoiceAction::CombineRightFirst) == Transition::Applied);
        return "right first\nright second\nleft first\nleft second\n";
    case FirstSection::Lines: break;
    }
    CHECK(choose(file, SourceSide::Stage2, ChoiceAction::Accept, ChoiceScope::Line, 0) == Transition::Applied);
    CHECK(choose(file, SourceSide::Stage2, ChoiceAction::Ignore, ChoiceScope::Line, 1) == Transition::Applied);
    CHECK(choose(file, SourceSide::Stage3, ChoiceAction::Ignore, ChoiceScope::Line, 0) == Transition::Applied);
    CHECK(file.current().draft.text == launch);
    CHECK(choose(file, SourceSide::Stage3, ChoiceAction::Accept, ChoiceScope::Line, 1) == Transition::Applied);
    return "left first\nright second\n";
}

void eachFullDecisionRemovesItsSectionsMarkerLinesInOneStep()
{
    for (const bool base : {false, true}) {
        const auto launch = test_support::markedText(base);
        // The second block is still open, so its marker lines stay.
        const auto later = launch.substr(launch.find("bridge\n"));
        for (const auto path :
             {FirstSection::ResolveLeft, FirstSection::ResolveRight, FirstSection::AcceptThenIgnore,
              FirstSection::IgnoreThenAccept, FirstSection::LeftFirst, FirstSection::RightFirst, FirstSection::Lines}) {
            auto file = test_support::markedFile(base);
            CHECK(file.markerSections().size() == 2);
            std::string expected = "header\n";
            expected += decideFirstSection(file, path, launch);
            expected += later;
            const auto chosen = file.current().draft;
            CHECK(chosen.text == expected);
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
    CHECK(choose(file, SourceSide::Stage3, ChoiceAction::ResolveUsingSide, ChoiceScope::Section, 1)
          == Transition::Applied);
    CHECK(file.current().draft.text == "header\nleft first\nleft second\nbridge\nright later\ntail\n");
    CHECK(file.current().draft.unresolvedMarkers == 0 && file.markerSections().empty());
    CHECK(file.beginMarkResolved(file.current().id) == Transition::Applied);
}

void acceptAloneOrIgnoringBothSidesKeepsTheMarkerLines()
{
    auto file = test_support::markedFile();
    const auto &left = test_support::choiceTarget(file, SourceSide::Stage2, ChoiceScope::Section);
    const auto accept = previewChoice(file, 1, left.source.contributionId, ChoiceAction::Accept);
    CHECK(accept.note == ChoiceNote::Accept && accept.markers == MarkerEffect::None);
    CHECK(applyChoice(file, accept) == Transition::Applied);
    CHECK(file.current().draft.text == test_support::markedText());
    CHECK(file.current().draft.unresolvedMarkers == 2 && file.markerSections().size() == 2);
    const auto &right = test_support::choiceTarget(file, SourceSide::Stage3, ChoiceScope::Section);
    const auto ignore = previewChoice(file, file.current().id, right.source.contributionId, ChoiceAction::Ignore);
    CHECK(ignore.note == ChoiceNote::Ignore && ignore.markers == MarkerEffect::Removed);
    auto ignored = test_support::markedFile();
    CHECK(choose(ignored, SourceSide::Stage2, ChoiceAction::Ignore) == Transition::Applied);
    CHECK(choose(ignored, SourceSide::Stage3, ChoiceAction::Ignore) == Transition::Applied);
    CHECK(ignored.current().draft.text == test_support::markedText());
    CHECK(ignored.current().draft.unresolvedMarkers == 2 && has(ignored, Blocker::UnresolvedMarkers));
}

void aHandEditedMarkerLineOrphansItsBlock()
{
    for (const auto *line : {"<<<<<<< ours\n", "=======\n", ">>>>>>> theirs\n"}) {
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
    CHECK(typed.current().draft.text.find("<<<<<<< ours\nleft first\nleft second\n=======\nright typed first\n")
          != std::string::npos);
    CHECK(typed.current().draft.unresolvedMarkers == 2);
}

// The ways of deciding both sections of a split block.
enum class SplitPath { AcceptThenIgnore, ResolveRight, LeftThenRight };

const char *name(SplitPath path)
{
    switch (path) {
    case SplitPath::AcceptThenIgnore: return "accept then ignore";
    case SplitPath::ResolveRight: return "resolve right";
    case SplitPath::LeftThenRight: break;
    }
    return "left then right";
}

void aSplitBlockResolvesWhenAllSectionsAreDecided(SplitPath path)
{
    const auto launch = test_support::splitText();
    auto file = test_support::splitFile();
    const auto sections = file.markerSections();
    const auto &first = test_support::choiceTarget(file, SourceSide::Stage2, ChoiceScope::Section, 0);
    const auto &second = test_support::choiceTarget(file, SourceSide::Stage2, ChoiceScope::Section, 1);
    CHECK(first.section != second.section && file.markers().blocks.size() == 1);
    CHECK(sections.size() == 1 && sections.front().block == file.markers().blocks.front());
    CHECK((sections.front().sections == std::vector<std::string>{first.section, second.section}));
    const auto partly = [&] {
        CHECK(file.current().draft.text == launch && file.current().draft.unresolvedMarkers == 1);
        CHECK(file.markerSections() == sections && has(file, Blocker::UnresolvedMarkers));
    };
    const auto lastPreview = [&](SourceSide side, std::size_t at, ChoiceAction action) {
        return previewChoice(file, file.current().id,
                             test_support::choiceTarget(file, side, ChoiceScope::Section, at).source.contributionId,
                             action);
    };
    std::string expected;
    std::optional<ChoicePreview> last;
    switch (path) {
    case SplitPath::AcceptThenIgnore:
        CHECK(choose(file, SourceSide::Stage2, ChoiceAction::Accept, ChoiceScope::Section, 0) == Transition::Applied);
        CHECK(choose(file, SourceSide::Stage2, ChoiceAction::Accept, ChoiceScope::Section, 1) == Transition::Applied);
        CHECK(choose(file, SourceSide::Stage3, ChoiceAction::Ignore, ChoiceScope::Section, 0) == Transition::Applied);
        partly();
        last = lastPreview(SourceSide::Stage3, 1, ChoiceAction::Ignore);
        CHECK(last->note == ChoiceNote::Ignore && last->markers == MarkerEffect::Removed);
        expected = "header\nleft one\nshared\nleft two\ntail\n";
        break;
    case SplitPath::ResolveRight:
        CHECK(choose(file, SourceSide::Stage3, ChoiceAction::ResolveUsingSide, ChoiceScope::Section, 1)
              == Transition::Applied);
        partly();
        last = lastPreview(SourceSide::Stage3, 0, ChoiceAction::ResolveUsingSide);
        expected = "header\nright one\nshared\nright two\ntail\n";
        break;
    case SplitPath::LeftThenRight:
        CHECK(choose(file, SourceSide::Stage2, ChoiceAction::ResolveUsingSide, ChoiceScope::Section, 0)
              == Transition::Applied);
        partly();
        last = lastPreview(SourceSide::Stage3, 1, ChoiceAction::ResolveUsingSide);
        expected = "header\nleft one\nshared\nright two\ntail\n";
        break;
    }
    CHECK(applyChoice(file, engaged(last)) == Transition::Applied);
    const auto chosen = file.current().draft;
    CHECK(chosen.text == expected && chosen.unresolvedMarkers == 0 && file.markerSections().empty());
    CHECK(!has(file, Blocker::UnresolvedMarkers) && !has(file, Blocker::Undecided));
    // One undo brings back the block, its text and the earlier decisions.
    CHECK(file.undo(file.current().id) == Transition::Applied);
    partly();
    CHECK(file.current().draft.decisions != chosen.decisions);
    CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == chosen);
    CHECK(file.beginMarkResolved(file.current().id) == Transition::Applied);
}

void aSplitBlockWithCrossedSidesResolvesInSectionOrder()
{
    const auto launch = test_support::splitText();
    // The first section keeps the right line and the second the left one, so
    // deleting cannot put them in order and the block is rebuilt.
    auto crossed = test_support::splitFile();
    CHECK(choose(crossed, SourceSide::Stage3, ChoiceAction::ResolveUsingSide, ChoiceScope::Section, 0)
          == Transition::Applied);
    const auto partly = crossed.current();
    CHECK(partly.draft.text == launch && partly.draft.unresolvedMarkers == 1);
    const auto last = previewChoice(
        crossed, crossed.current().id,
        test_support::choiceTarget(crossed, SourceSide::Stage2, ChoiceScope::Section, 1).source.contributionId,
        ChoiceAction::ResolveUsingSide);
    CHECK(last.note == ChoiceNote::ResolveUsingSide && last.markers == MarkerEffect::Removed);
    CHECK(applyChoice(crossed, last) == Transition::Applied);
    const auto chosen = crossed.current().draft;
    CHECK(chosen.text == "header\nright one\nshared\nleft two\ntail\n");
    CHECK(chosen.unresolvedMarkers == 0 && crossed.markerSections().empty());
    CHECK(!has(crossed, Blocker::UnresolvedMarkers) && !has(crossed, Blocker::Undecided));
    // Each section's scopes are bound in the rebuilt text: the kept one to its
    // line, the ignored one empty beside it, left before right.
    const auto range = [&](SourceSide side, std::size_t at) {
        return anchor(crossed,
                      test_support::choiceTarget(crossed, side, ChoiceScope::Section, at).source.contributionId)
            .result;
    };
    CHECK((range(SourceSide::Stage3, 0) == ByteRange{7, 17} && range(SourceSide::Stage2, 0) == ByteRange{7, 7}));
    CHECK((range(SourceSide::Stage2, 1) == ByteRange{24, 33} && range(SourceSide::Stage3, 1) == ByteRange{33, 33}));
    CHECK(crossed.correspondence()->handEditedAnchors().empty());
    // One undo brings back the block and the first section's decisions.
    CHECK(crossed.undo(crossed.current().id) == Transition::Applied);
    CHECK(crossed.current().draft == partly.draft && crossed.markerSections().size() == 1);
    CHECK(crossed.redo(crossed.current().id) == Transition::Applied && crossed.current().draft == chosen);
    CHECK(range(SourceSide::Stage2, 1) == ByteRange{24, 33});
    CHECK(crossed.beginMarkResolved(crossed.current().id) == Transition::Applied);
    // Recovery replays each draft with its hand-edit flags and derives the same text and count.
    auto replayed = test_support::splitFile();
    CHECK(test_support::replay(replayed, partly.draft) == Transition::Applied);
    CHECK(test_support::replay(replayed, chosen) == Transition::Applied);
    CHECK(replayed.current().draft == chosen && replayed.markers().count() == 0);
    CHECK(replayed.beginMarkResolved(replayed.current().id) == Transition::Applied);
}

void aSplitBlockKeepsItsMarkersWhileASectionKeepsNoSideOrSharedTextIsEdited()
{
    const auto launch = test_support::splitText();
    // Ignoring both sides of one section keeps the markers, as for a single section.
    auto ignored = test_support::splitFile();
    CHECK(choose(ignored, SourceSide::Stage2, ChoiceAction::ResolveUsingSide, ChoiceScope::Section, 0)
          == Transition::Applied);
    CHECK(choose(ignored, SourceSide::Stage2, ChoiceAction::Ignore, ChoiceScope::Section, 1) == Transition::Applied);
    CHECK(choose(ignored, SourceSide::Stage3, ChoiceAction::Ignore, ChoiceScope::Section, 1) == Transition::Applied);
    CHECK(ignored.current().draft.text == launch && ignored.current().draft.unresolvedMarkers == 1);
    // Typing in one copy of the shared line unties the block, and that work stays.
    auto typed = test_support::splitFile();
    const auto at = launch.rfind("shared") + 6;
    CHECK(typed.editResult(1, {{at, at}, " typed"}) == Transition::Applied);
    const auto edited = typed.current().draft.text;
    CHECK(typed.markers().blocks.size() == 1 && typed.markerSections().empty());
    CHECK(choose(typed, SourceSide::Stage2, ChoiceAction::ResolveUsingSide, ChoiceScope::Section, 0)
          == Transition::Applied);
    CHECK(choose(typed, SourceSide::Stage2, ChoiceAction::ResolveUsingSide, ChoiceScope::Section, 1)
          == Transition::Applied);
    CHECK(typed.current().draft.text == edited && typed.current().draft.unresolvedMarkers == 1);
    // Take both on a section works, but a single line of one half still cannot
    // be joined to a line of the other half, so that stays refused and the
    // text and decisions stay as they were.
    auto both = test_support::splitFile();
    const auto before = both.current();
    for (const auto side : {SourceSide::Stage2, SourceSide::Stage3})
        for (const std::size_t line : {0U, 1U})
            for (const auto action : {ChoiceAction::CombineLeftFirst, ChoiceAction::CombineRightFirst}) {
                const auto preview = previewChoice(
                    both, both.current().id,
                    test_support::choiceTarget(both, side, ChoiceScope::Line, line).source.contributionId, action);
                CHECK(preview.state == Transition::AmbiguousTarget);
                CHECK(applyChoice(both, preview, true) != Transition::Applied);
            }
    CHECK(both.current().draft == before.draft && both.current().id == before.id);
}

// How the section that was not combined is decided.
enum class Other { Left, Right, LeftFirst, RightFirst };

constexpr std::array<const char *, 2> kSplitLeft{"left one\n", "left two\n"};
constexpr std::array<const char *, 2> kSplitRight{"right one\n", "right two\n"};

std::string joined(const char *first, const char *second)
{
    std::string text = first;
    text += second;
    return text;
}

// Decides split section `at` as `other` and returns the text it keeps.
std::string decideOther(FileSession &file, Other other, std::size_t at)
{
    switch (other) {
    case Other::Left:
        CHECK(choose(file, SourceSide::Stage2, ChoiceAction::ResolveUsingSide, ChoiceScope::Section, at)
              == Transition::Applied);
        return kSplitLeft.at(at);
    case Other::Right:
        CHECK(choose(file, SourceSide::Stage3, ChoiceAction::ResolveUsingSide, ChoiceScope::Section, at)
              == Transition::Applied);
        return kSplitRight.at(at);
    case Other::LeftFirst:
        CHECK(choose(file, SourceSide::Stage3, ChoiceAction::CombineLeftFirst, ChoiceScope::Section, at)
              == Transition::Applied);
        return joined(kSplitLeft.at(at), kSplitRight.at(at));
    case Other::RightFirst:
        CHECK(choose(file, SourceSide::Stage2, ChoiceAction::CombineRightFirst, ChoiceScope::Section, at)
              == Transition::Applied);
        return joined(kSplitRight.at(at), kSplitLeft.at(at));
    }
    return std::string{};
}

// Take both on the `side` scope of split section `at`, before or after the
// other section is decided as `other`.
struct SplitTakeBoth {
    bool combineFirst = true;
    std::size_t at = 0;
    SourceSide side = SourceSide::Stage2;
    ChoiceAction action = ChoiceAction::CombineLeftFirst;
    Other other = Other::Left;
    [[nodiscard]] std::size_t rest() const { return std::size_t{1} - at; }
};

Transition takeBoth(FileSession &file, const SplitTakeBoth &step)
{
    const auto &target = test_support::choiceTarget(file, step.side, ChoiceScope::Section, step.at);
    return applyChoice(file, previewChoice(file, file.current().id, target.source.contributionId, step.action), true);
}

// Recovery replays each draft and derives the same text and count, and a
// draft recovered between the two decisions finishes the same way.
void expectSplitRecovery(const SplitTakeBoth &step, const std::vector<MarkerSection> &sections, const Draft &partly,
                         const Draft &chosen, const std::string &rest)
{
    auto replayed = test_support::splitFile();
    CHECK(test_support::replay(replayed, partly) == Transition::Applied);
    CHECK(replayed.markerSections() == sections);
    CHECK(test_support::replay(replayed, chosen) == Transition::Applied);
    CHECK(replayed.current().draft == chosen && replayed.markers().count() == 0);
    CHECK(replayed.beginMarkResolved(replayed.current().id) == Transition::Applied);
    auto resumed = test_support::splitFile();
    CHECK(test_support::replay(resumed, partly) == Transition::Applied);
    if (step.combineFirst)
        CHECK(decideOther(resumed, step.other, step.rest()) == rest);
    else
        CHECK(takeBoth(resumed, step) == Transition::Applied);
    CHECK(resumed.current().draft == chosen);
}

void takeBothOnASplitSection(const SplitTakeBoth &step)
{
    const auto launch = test_support::splitText();
    const bool leftFirst = step.action == ChoiceAction::CombineLeftFirst;
    const auto at = step.at;
    const auto rest = step.rest();
    auto file = test_support::splitFile();
    const auto sections = file.markerSections();
    std::array<std::string, 2> kept;
    if (!step.combineFirst) kept.at(rest) = decideOther(file, step.other, rest);
    const auto earlier = file.current();
    CHECK(earlier.draft.text == launch && file.markerSections() == sections);
    const auto preview = previewChoice(
        file, earlier.id, test_support::choiceTarget(file, step.side, ChoiceScope::Section, at).source.contributionId,
        step.action);
    kept.at(at) =
        leftFirst ? joined(kSplitLeft.at(at), kSplitRight.at(at)) : joined(kSplitRight.at(at), kSplitLeft.at(at));
    CHECK(preview.state == Transition::ConfirmationRequired && preview.replacement == kept.at(at));
    CHECK(preview.markers == (step.combineFirst ? MarkerEffect::KeptUntilDecided : MarkerEffect::Removed));
    // Only this section's two scopes are edited, each with its own side's text,
    // so the shared line and the other section's lines are outside every edit.
    CHECK(applyChoice(file, preview) == Transition::ConfirmationRequired && file.current().id == earlier.id);
    CHECK(applyChoice(file, preview, true) == Transition::Applied);
    for (const auto &[combined, order] :
         {std::pair{SourceSide::Stage2, leftFirst ? 1U : 2U}, std::pair{SourceSide::Stage3, leftFirst ? 2U : 1U}})
        for (const auto &id : test_support::choiceTarget(file, combined, ChoiceScope::Section, at).decisions)
            CHECK((file.current().draft.decisions.at(id) == Decision{DecisionValue::Accepted, order}));
    if (step.combineFirst) {
        // The block keeps its markers and text until the other section is decided.
        CHECK(file.current().draft.text == launch && file.current().draft.unresolvedMarkers == 1);
        CHECK(file.markerSections() == sections && has(file, Blocker::UnresolvedMarkers));
    }
    const auto partly = step.combineFirst ? file.current().draft : earlier.draft;
    if (step.combineFirst) kept.at(rest) = decideOther(file, step.other, rest);
    const auto chosen = file.current().draft;
    std::string expected = "header\n";
    expected += kept[0];
    expected += "shared\n";
    expected += kept[1];
    expected += "tail\n";
    CHECK(chosen.text == expected && chosen.unresolvedMarkers == 0 && file.markerSections().empty());
    CHECK(!has(file, Blocker::UnresolvedMarkers) && !has(file, Blocker::Undecided));
    // One undo brings back the block and the first section's decisions.
    CHECK(file.undo(file.current().id) == Transition::Applied);
    CHECK(file.current().draft == partly && file.markerSections() == sections);
    CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == chosen);
    CHECK(file.beginMarkResolved(file.current().id) == Transition::Applied);
    expectSplitRecovery(step, sections, partly, chosen, kept.at(rest));
}

void takeBothOnASectionOfASplitBlockResolvesTheBlockInSectionOrder()
{
    for (const bool combineFirst : {true, false})
        for (const std::size_t at : {0U, 1U})
            for (const auto side : {SourceSide::Stage2, SourceSide::Stage3})
                for (const auto action : {ChoiceAction::CombineLeftFirst, ChoiceAction::CombineRightFirst})
                    for (const auto other : {Other::Left, Other::Right, Other::LeftFirst, Other::RightFirst})
                        takeBothOnASplitSection({combineFirst, at, side, action, other});
    // A combination that would overwrite an unselected contribution is still refused:
    // the right side's second line maps onto the left line its first line also maps to.
    auto overlapping = test_support::choiceFile("header\nleft\ntail\n", "header\nright one\nright two\ntail\n");
    const auto before = overlapping.current();
    const auto refused = previewChoice(
        overlapping, before.id,
        test_support::choiceTarget(overlapping, SourceSide::Stage3, ChoiceScope::Line, 1).source.contributionId,
        ChoiceAction::CombineLeftFirst);
    CHECK(refused.state == Transition::AmbiguousTarget);
    CHECK(refused.note == ChoiceNote::ReplacesUnselected);
    CHECK(applyChoice(overlapping, refused, true) != Transition::Applied
          && overlapping.current().draft == before.draft);
}

void theDeletingSideOfAMarkerSectionCanBeAcceptedAndResolved()
{
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
            CHECK(preview.state == Transition::Applied && preview.before == "right first\n"
                  && preview.replacement.empty());
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
        for (const auto &id : file.catalogue().opposite(left)->decisions)
            CHECK(chosen.decisions.at(id).value == DecisionValue::Ignored);
        CHECK(file.undo(file.current().id) == Transition::Applied && file.current().draft.text == before);
        CHECK(file.markerSections().size() == 1);
        CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == chosen);
    }
}

void aDeletionOutsideAMarkerSectionKeepsTheScopeCheck()
{
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
        CHECK(refused.note == ChoiceNote::UncertainDeletion);
        CHECK(applyChoice(file, refused, true) == Transition::AmbiguousTarget);
        CHECK(file.current().id == edited.id && file.current().draft == edited.draft);
    }
}

}

int main()
{
    test_support::Suite suite("MarkerEditingTests");
    suite.run("each full decision removes its section's marker lines in one undoable step",
              eachFullDecisionRemovesItsSectionsMarkerLinesInOneStep);
    suite.run("accept alone or ignoring both sides keeps the marker lines",
              acceptAloneOrIgnoringBothSidesKeepsTheMarkerLines);
    suite.run("a hand-edited marker line orphans its block, which is never removed",
              aHandEditedMarkerLineOrphansItsBlock);
    for (const auto path : {SplitPath::AcceptThenIgnore, SplitPath::ResolveRight, SplitPath::LeftThenRight})
        suite.run(
            titled("a block around two sections split by a shared line resolves when all are decided", {name(path)}),
            [=] { aSplitBlockResolvesWhenAllSectionsAreDecided(path); });
    suite.run("a split block whose sections keep opposite sides out of order resolves in section order",
              aSplitBlockWithCrossedSidesResolvesInSectionOrder);
    suite.run("a split block keeps its markers while a section keeps no side or shared text is edited",
              aSplitBlockKeepsItsMarkersWhileASectionKeepsNoSideOrSharedTextIsEdited);
    suite.run("Take both on a section of a split block resolves the block in section order",
              takeBothOnASectionOfASplitBlockResolvesTheBlockInSectionOrder);
    suite.run("the deleting side of a marker section can be accepted and resolved",
              theDeletingSideOfAMarkerSectionCanBeAcceptedAndResolved);
    suite.run("a deletion outside a marker section keeps the scope check",
              aDeletionOutsideAMarkerSectionKeepsTheScopeCheck);
    return suite.report();
}
