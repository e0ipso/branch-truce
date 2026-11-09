// FileSession transactions: launch, history, discard and the large-file gate.
#include "TestHarness.h"
#include "core/ResourceLimits.h"
#include "core/SessionState.h"
#include <string>

using namespace branch_truce;

namespace {
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
    return suite.report();
}
