// FileSession transactions: launch, history, discard and the large-file gate.
#include "TestHarness.h"
#include "core/ResourceLimits.h"
#include "core/SessionState.h"
#include "fixtures/ChoiceFile.h"
#include <memory>
#include <new>
#include <string>

using namespace branch_truce;

namespace {
// The engine's alignment, counting its calls and failing the one numbered
// failAt with the only failure the algorithm contract allows.
class CountingAlgorithm final : public CorrespondenceAlgorithm {
public:
    explicit CountingAlgorithm(int failAt = 0) : failAt_(failAt) {}
    [[nodiscard]] std::vector<Passage> compare(std::string_view source, std::string_view result) const override
    {
        if (++calls == failAt_) throw std::bad_alloc();
        return ExactLineCorrespondence{}.compare(source, result);
    }
    mutable int calls = 0;

private:
    int failAt_;
};

std::string lines(std::size_t count)
{
    std::string text;
    text.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) text += "x\n";
    return text;
}

FileSession captured(const std::string &text)
{
    FileSnapshots sources;
    sources.path = "limit.txt";
    sources.provenance.sessionId = "limits";
    sources.stage2 = SourceSnapshot{"source-object", "100644", text, {text, text, {}}, "source-commit", "Source"};
    sources.launchResult.text = {text, text, {}};
    return FileSession("limit", sources.path, {text, {}, 0}, Support::Supported, {}, sources);
}

FileSession withMarkerSize(const std::string &attribute, std::size_t size)
{
    const auto marker = [&](char c) { return std::string(size, c); };
    const std::string left = "head\nleft\ntail\n";
    const std::string right = "head\nright\ntail\n";
    const auto result =
        "head\n" + marker('<') + " ours\nleft\n" + marker('=') + "\nright\n" + marker('>') + " theirs\ntail\n";
    FileSnapshots sources;
    sources.path = "marked.txt";
    sources.provenance.sessionId = "markers";
    sources.conversion.attributes = {{"conflict-marker-size", attribute}};
    sources.stage2 = SourceSnapshot{"left-object", "100644", left, {left, left, {}}, "left-commit", "Left"};
    sources.stage3 = SourceSnapshot{"right-object", "100644", right, {right, right, {}}, "right-commit", "Right"};
    sources.launchResult.text = {result, result, {}};
    return FileSession("marked", sources.path, {result, {}, 0}, Support::Supported, {}, sources, true);
}

void discardAfterLoadingALargeFileRestoresItsLaunchText()
{
    const auto text = lines(kLargeFileLines + 1);
    auto file = captured(text);
    CHECK(file.support() == Support::LargeFile);
    CHECK(file.loadLargeFile() == Transition::Applied);
    const auto loaded = file.current().id;
    CHECK(file.editResult(loaded, {{0, 1}, "y"}) == Transition::Applied);
    CHECK(file.discardToCheckpoint(file.current().id) == Transition::Applied);
    CHECK(file.current().draft.text == text && file.current().result == loaded);
    CHECK(file.correspondence() && file.correspondence()->text() == text);
    CHECK(file.undo(file.current().id) == Transition::Applied && file.current().draft.text.starts_with('y'));
}

void wholeDraftsKeepTheTextInvariant()
{
    const std::string text = "kept\n";
    for (auto file : {captured(text), FileSession("plain", "plain.txt", {text, {}, 0})}) {
        const auto before = file.current();
        for (const auto *rejected : {"invalid \x80\n", "carriage\r\nreturn\n"})
            CHECK(file.replaceDraft(before.id, {rejected, before.draft.decisions, 0}) == Transition::InvalidAnchor);
        CHECK(file.editResult(before.id, {{0, 0}, "\xc3"}) == Transition::InvalidAnchor);
        CHECK(file.editResult(before.id, {{0, 0}, "\r"}) == Transition::InvalidAnchor);
        CHECK(file.current().id == before.id && file.current().draft == before.draft && !file.canUndo());
        CHECK(file.editResult(before.id, {{0, 0}, "still editable\n"}) == Transition::Applied);
    }
}

void onlyAcceptedDecisionsCarryAnOrder()
{
    FileSession file("plain", "plain.txt", {"text\n", {}, 0});
    CHECK(file.replaceDraft(1, {"text\n", {{"left", {DecisionValue::Ignored, 1}}}, 0}) == Transition::InvalidAnchor);
    CHECK(file.replaceDraft(1, {"text\n", {{"left", {DecisionValue::Undecided, 2}}}, 0}) == Transition::InvalidAnchor);
    CHECK(file.current().id == 1 && file.current().draft.decisions.empty());
    CHECK(file.replaceDraft(1, {"text\n", {{"left", {DecisionValue::Accepted, 1}}}, 0}) == Transition::Applied);
}

void markersFollowTheCapturedMarkerSize()
{
    auto file = withMarkerSize("32", 32);
    CHECK(file.support() == Support::Supported && file.current().draft.unresolvedMarkers == 1);
    CHECK(file.markers().blocks.size() == 1 && file.markerSections().size() == 1);
    CHECK(file.beginMarkResolved(1) == Transition::Blocked);
    // Git falls back to seven for a value that is not a positive number.
    CHECK(withMarkerSize("unset", 7).current().draft.unresolvedMarkers == 1);
    CHECK(withMarkerSize("unset", 32).current().draft.unresolvedMarkers == 0);
    const auto oversized = withMarkerSize(std::to_string(kMaxConflictMarkerSize + 1), 7);
    CHECK(oversized.support() == Support::OutsideApp && !oversized.correspondence());
}

void copiesShareRevisionsAndDiscardedBranchesAreDropped()
{
    FileSession file("plain", "plain.txt", {"a\n", {}, 0});
    CHECK(file.editResult(1, {{0, 1}, "b"}) == Transition::Applied);
    CHECK(file.editResult(2, {{0, 1}, "c"}) == Transition::Applied);
    const auto copy = file;
    CHECK(&copy.current() == &file.current());
    // Undo publishes revision 4 as a copy of 2; the next edit discards 3 and 4.
    CHECK(file.undo(3) == Transition::Applied && file.current().id == 4);
    CHECK(file.editResult(4, {{0, 1}, "d"}) == Transition::Applied && file.current().id == 5);
    CHECK(file.recordCheckpoint(3, Outcome::Succeeded) == Transition::StaleRevision);
    CHECK(file.recordCheckpoint(4, Outcome::Succeeded) == Transition::StaleRevision);
    CHECK(!file.checkpoint() && !file.checkpointOutcome());
    // History entries, the current revision and the launch revision remain.
    CHECK(file.recordCheckpoint(2, Outcome::Succeeded) == Transition::Applied);
    CHECK(file.recordCheckpoint(5, Outcome::Succeeded) == Transition::Applied);
    CHECK(file.undo(5) == Transition::Applied && file.current().draft.text == "b\n");
    CHECK(file.undo(6) == Transition::Applied && file.current().draft.text == "a\n");
    // The copy kept its own history.
    CHECK(copy.current().draft.text == "c\n" && copy.canUndo() && !copy.canRedo());
}

void undoReachesBackAsFarAsTheHistorySizeAllows()
{
    // Each revision holds one payload-sized text, so the history fits this many.
    const auto fits = kMaxHistoryBytes / kMaxRecoveryPayloadBytes;
    std::string text;
    text.reserve(kMaxRecoveryPayloadBytes);
    while (text.size() < kMaxRecoveryPayloadBytes) text += std::string(1023, 'x') + '\n';
    FileSession file("plain", "plain.txt", {text, {}, 0});
    for (std::size_t edit = 0; edit < fits + 8; ++edit)
        CHECK(file.editResult(file.current().id, {{0, 1}, edit % 2 ? "x" : "y"}) == Transition::Applied);
    std::size_t undone = 0;
    while (file.canUndo()) {
        CHECK(file.undo(file.current().id) == Transition::Applied);
        ++undone;
    }
    CHECK(undone > 0 && undone < fits);
    // Discarding without a checkpoint still restores the launch text.
    CHECK(file.discardToCheckpoint(file.current().id) == Transition::Applied && file.current().draft.text == text);
}

FileSession diff3Session(std::shared_ptr<const CorrespondenceAlgorithm> algorithm)
{
    auto sources = *test_support::underlinedBaseFile().snapshots();
    const auto text = sources.launchResult.text.utf8;
    return FileSession("diff3", sources.path, {text, {}, 0}, Support::Supported, {}, sources, true,
                       std::move(algorithm));
}

void theSessionAlignsWithTheAlgorithmItIsGiven()
{
    const auto algorithm = std::make_shared<CountingAlgorithm>();
    auto file = diff3Session(algorithm);
    CHECK(file.support() == Support::Supported && file.correspondence() && !file.choices().empty());
    // Three sources against the result, the two sides, and each side against the earlier version.
    CHECK(algorithm->calls >= 6);
    const auto opened = algorithm->calls;
    CHECK(file.editResult(1, {{0, 0}, "typed\n"}) == Transition::Applied);
    CHECK(algorithm->calls > opened);
}

void aFailedOpeningLeavesNothingHalfBuilt()
{
    // Calls 1 to 3 align the sources, 4 builds the choices and 5 fails while
    // finding the non-conflicting changes.
    auto file = diff3Session(std::make_shared<CountingAlgorithm>(5));
    CHECK(file.support() == Support::Unreadable && !file.correspondence());
    CHECK(file.choices().empty() && file.nonConflictingTargets().empty());
    CHECK(file.current().draft.decisions.empty() && file.markerSections().empty());
    CHECK(file.snapshots() && file.current().draft.text == file.snapshots()->launchResult.text.utf8);
    CHECK(file.editResult(1, {{0, 0}, "x"}) == Transition::Blocked);
}

void launchTextOutsideTheInvariantIsUnreadable()
{
    for (const auto *text : {"bad \xff\n", "crlf\r\n"}) {
        CHECK(FileSession("plain", "plain.txt", {text, {}, 0}).support() == Support::Unreadable);
        CHECK(captured(text).support() == Support::Unreadable && !captured(text).correspondence());
    }
    // An adapter's own diagnosis stays in place.
    CHECK(FileSession("outside", "image.png", {"\xff", {}, 0}, Support::OutsideApp, "binary").supportDetail()
          == "binary");
}
}

int main()
{
    test_support::Suite suite("FileSessionTests");
    suite.run("discarding after loading a large file restores its launch text",
              discardAfterLoadingALargeFileRestoresItsLaunchText);
    suite.run("whole drafts and edits keep UTF-8 text with LF line endings", wholeDraftsKeepTheTextInvariant);
    suite.run("only accepted decisions carry a combination order", onlyAcceptedDecisionsCarryAnOrder);
    suite.run("launch text outside that invariant is unreadable", launchTextOutsideTheInvariantIsUnreadable);
    suite.run("markers follow the captured conflict-marker-size", markersFollowTheCapturedMarkerSize);
    suite.run("copies share revisions and discarded branches are dropped",
              copiesShareRevisionsAndDiscardedBranchesAreDropped);
    suite.run("undo reaches back as far as the history size allows", undoReachesBackAsFarAsTheHistorySizeAllows);
    suite.run("the session aligns with the algorithm it is given", theSessionAlignsWithTheAlgorithmItIsGiven);
    suite.run("a failed opening leaves nothing half built", aFailedOpeningLeavesNothingHalfBuilt);
    return suite.report();
}
