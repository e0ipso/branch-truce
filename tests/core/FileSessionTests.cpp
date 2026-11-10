// FileSession transactions: launch, captures, history, discard, outcomes, facts and the large-file gate.
#include "TestHarness.h"
#include "core/ExactLineCorrespondence.h"
#include "core/ResourceLimits.h"
#include "core/SessionFixtures.h"
#include "core/SessionState.h"
#include "fixtures/ChoiceFile.h"
#include <algorithm>
#include <array>
#include <memory>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace branch_truce;
using test_support::choose;
using test_support::engaged;
using test_support::has;
using test_support::lines;
using test_support::longestLines;
using test_support::titled;

namespace {
// The engine's alignment, counting its calls and failing the one numbered
// failAt: with the only failure the algorithm contract allows, or with a
// defect that breaks the contract.
class CountingAlgorithm final : public CorrespondenceAlgorithm {
public:
    enum class Failure { OutOfMemory, Defect };
    explicit CountingAlgorithm(int failAt = 0, Failure failure = Failure::OutOfMemory)
        : failAt(failAt), failure_(failure)
    {}
    [[nodiscard]] std::vector<Passage> compare(std::string_view source, std::string_view result) const override
    {
        if (++calls == failAt) {
            if (failure_ == Failure::OutOfMemory) throw std::bad_alloc();
            throw std::logic_error("alignment defect");
        }
        return ExactLineCorrespondence{}.compare(source, result);
    }
    [[nodiscard]] std::string_view identity() const noexcept override { return ExactLineCorrespondence{}.identity(); }
    mutable int calls = 0;
    int failAt;

private:
    Failure failure_;
};

Draft draft(std::string text = "result\n")
{
    return {std::move(text), {{"left", {DecisionValue::Accepted, 1}}, {"right", {DecisionValue::Ignored, {}}}}, 0};
}

// A rebase capture whose sources differ in encoding, newlines and modes.
FileSnapshots capturedSources()
{
    FileSnapshots captured;
    captured.path = ":literal\tfile.txt";
    captured.provenance = {"session",
                           "/worktree",
                           "/git/worktrees/session",
                           SnapshotProvenance::Operation::Rebase,
                           std::string(64, '2'),
                           std::string(64, '3'),
                           std::string(64, '1'),
                           "refs/heads/topic",
                           "original-head",
                           "onto"};
    captured.conversion.attributes = {{"text", "set"}, {"eol", "crlf"}};
    captured.conversion.configuration = {{"core.autocrlf", "false"}, {"core.eol", "crlf"}};
    captured.resultMode = "100755";
    captured.earlier = SourceSnapshot{
        "base-blob",
        "100644",
        "base\n",
        {"base\n", "base\r\n", {SnapshotFormat::Encoding::Utf8, false, SnapshotFormat::Newline::CrLf, true}},
        "base-commit",
        "Common earlier version"};
    captured.stage2 = SourceSnapshot{
        "left-blob",
        "100755",
        "main\n",
        {"main\n", "\xef\xbb\xbfmain\r\n", {SnapshotFormat::Encoding::Utf8, true, SnapshotFormat::Newline::CrLf, true}},
        "left-commit",
        "Current rebased state"};
    captured.stage3 =
        SourceSnapshot{"right-blob",
                       "100755",
                       "side\n",
                       {"side\n",
                        std::string("\0s\0i\0d\0e\0\r\0\n", 12),
                        {SnapshotFormat::Encoding::Utf16BigEndian, false, SnapshotFormat::Newline::CrLf, true}},
                       "right-commit",
                       "Commit being replayed"};
    captured.launchResult = {{"manual\nkept",
                              "\xef\xbb\xbfmanual\r\nkept",
                              {SnapshotFormat::Encoding::Utf8, true, SnapshotFormat::Newline::CrLf, false}},
                             {11, 22, 0100755, 15, 33, 44, 55, 66}};
    return captured;
}

// Mark resolved, then a successful Save, write and stage of the current revision.
void resolve(FileSession &file)
{
    const auto revision = file.current().id;
    CHECK(file.beginMarkResolved(revision) == Transition::Applied);
    CHECK(file.recordCheckpoint(revision, Outcome::Succeeded) == Transition::Applied);
    CHECK(file.recordWrite(revision, Outcome::Succeeded) == Transition::Applied);
    CHECK(file.recordStage(revision, Outcome::Succeeded, "verified-object", "100644") == Transition::Applied);
    CHECK(file.facts().resolved);
    CHECK(file.facts().blockers.empty());
}

const char *name(Outcome outcome)
{
    switch (outcome) {
    case Outcome::Succeeded: return "succeeded";
    case Outcome::Failed: return "failed";
    case Outcome::Uncertain: break;
    }
    return "uncertain";
}

// The publication step an adapter reports an outcome for.
enum class Step { Checkpoint, Write, Stage };

const char *name(Step step)
{
    switch (step) {
    case Step::Checkpoint: return "checkpoint";
    case Step::Write: return "write";
    case Step::Stage: break;
    }
    return "stage";
}

FileSession captured(const std::string &text)
{
    FileSnapshots sources;
    sources.path = "limit.txt";
    sources.provenance.sessionId = "limits";
    sources.stage2 = SourceSnapshot{"source-object", "100644", text, {text, text, {}}, "source-commit", "Source"};
    sources.launchResult.text = {text, text, {}};
    return FileSession("limit", sources.path, {text, {}, 0}, {.snapshots = sources});
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
    return FileSession("marked", sources.path, {result, {}, 0}, {.snapshots = sources, .initializeChoices = true});
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
            CHECK(file.replaceDraft(before.id, {rejected, before.draft.decisions, 0}) == Transition::InvalidText);
        CHECK(file.editResult(before.id, {{0, 0}, "\xc3"}) == Transition::InvalidText);
        CHECK(file.editResult(before.id, {{0, 0}, "\r"}) == Transition::InvalidText);
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
    CHECK(oversized.loadFailure() == LoadFailure{LoadFailureReason::MarkerSizeTooLarge}
          && oversized.supportDetail().empty());
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
    CHECK(!file.publication().checkpoint() && !file.publication().checkpointOutcome());
    // History entries, the current revision and the launch revision remain.
    CHECK(file.recordCheckpoint(2, Outcome::Succeeded) == Transition::Applied);
    CHECK(file.recordCheckpoint(5, Outcome::Succeeded) == Transition::Applied);
    CHECK(file.undo(5) == Transition::Applied && file.current().draft.text == "b\n");
    CHECK(file.undo(6) == Transition::Applied && file.current().draft.text == "a\n");
    // The copy kept its own history.
    CHECK(copy.current().draft.text == "c\n" && copy.canUndo() && !copy.canRedo());
}

// One payload-sized text of 1024-byte lines.
std::string payloadText()
{
    std::string text;
    text.reserve(kMaxRecoveryPayloadBytes);
    while (text.size() < kMaxRecoveryPayloadBytes) {
        text.append(1023, 'x');
        text += '\n';
    }
    return text;
}

// Edits the file `count` times, then undoes as far as the history allows.
std::size_t undoableAfterEdits(FileSession &file, std::size_t count)
{
    for (std::size_t edit = 0; edit < count; ++edit)
        CHECK(file.editResult(file.current().id, {{0, 1}, edit % 2 ? "x" : "y"}) == Transition::Applied);
    std::size_t undone = 0;
    while (file.canUndo()) {
        CHECK(file.undo(file.current().id) == Transition::Applied);
        ++undone;
    }
    return undone;
}

void undoReachesBackAsFarAsTheHistorySizeAllows()
{
    // Each revision holds one payload-sized text, so the history fits this many.
    const auto fits = kMaxHistoryBytes / kMaxRecoveryPayloadBytes;
    const auto text = payloadText();
    FileSession file("plain", "plain.txt", {text, {}, 0});
    const auto undone = undoableAfterEdits(file, fits + 8);
    CHECK(undone > 0 && undone < fits);
    // Discarding without a checkpoint still restores the launch text.
    CHECK(file.discardToCheckpoint(file.current().id) == Transition::Applied && file.current().draft.text == text);
}

void revisionsOutcomesNameStayStoredWithinTheHistorySize()
{
    const auto fits = kMaxHistoryBytes / kMaxRecoveryPayloadBytes;
    const auto text = payloadText();
    FileSession plain("plain", "plain.txt", {text, {}, 0});
    const auto plainUndone = undoableAfterEdits(plain, fits + 8);
    FileSession file("plain", "plain.txt", {text, {}, 0});
    CHECK(file.editResult(1, {{0, 1}, "w"}) == Transition::Applied);
    CHECK(file.beginMarkResolved(2) == Transition::Applied);
    CHECK(file.recordCheckpoint(2, Outcome::Succeeded) == Transition::Applied);
    CHECK(file.recordWrite(2, Outcome::Succeeded) == Transition::Applied);
    CHECK(file.recordStage(2, Outcome::Succeeded, "object", "100644") == Transition::Applied);
    const auto undone = undoableAfterEdits(file, fits + 7);
    // The saved, reviewed, written and staged revision is one stored copy,
    // kept although undo no longer reaches it, and it takes one undo step's room.
    CHECK(file.revision(2) && file.revision(2)->draft.text.starts_with('w'));
    CHECK(undone + 1 == plainUndone);
    CHECK(file.discardToCheckpoint(file.current().id) == Transition::Applied);
    CHECK(file.current().draft.text.starts_with('w') && file.current().result == 2);
}

FileSession diff3Session(std::shared_ptr<const CorrespondenceAlgorithm> algorithm)
{
    auto sources = *test_support::underlinedBaseFile().snapshots();
    const auto text = sources.launchResult.text.utf8;
    return FileSession("diff3", sources.path, {text, {}, 0},
                       {.snapshots = sources, .initializeChoices = true, .algorithm = std::move(algorithm)});
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
    CHECK(file.loadFailure() == LoadFailure{LoadFailureReason::OutOfMemory});
    CHECK(file.choices().empty() && file.nonConflictingTargets().empty());
    CHECK(file.current().draft.decisions.empty() && file.markerSections().empty());
    CHECK(file.snapshots() && file.current().draft.text == file.snapshots()->launchResult.text.utf8);
    CHECK(file.editResult(1, {{0, 0}, "x"}) == Transition::Blocked);
}

void anEditThatRunsOutOfMemoryChangesNothing()
{
    const auto algorithm = std::make_shared<CountingAlgorithm>();
    auto file = diff3Session(algorithm);
    const auto before = file.current();
    const auto *mapping = file.correspondence();
    const auto unchanged = [&] {
        return file.current().id == before.id && file.current().draft == before.draft
            && file.correspondence() == mapping && !file.canUndo();
    };
    algorithm->failAt = algorithm->calls + 1;
    CHECK(file.editResult(before.id, {{0, 0}, "typed\n"}) == Transition::OutOfMemory);
    CHECK(unchanged());
    algorithm->failAt = algorithm->calls + 1;
    CHECK(file.replaceDraft(before.id, {"replaced\n", before.draft.decisions, 0}) == Transition::OutOfMemory);
    CHECK(unchanged());
    algorithm->failAt = 0;
    CHECK(file.editResult(before.id, {{0, 0}, "typed\n"}) == Transition::Applied);
    CHECK(file.current().id == before.id + 1 && file.canUndo());
}

void aDefectiveAlignmentHoldsTheFileBack()
{
    auto file = diff3Session(std::make_shared<CountingAlgorithm>(2, CountingAlgorithm::Failure::Defect));
    CHECK(file.support() == Support::Unreadable && !file.correspondence());
    CHECK(file.loadFailure() == LoadFailure{LoadFailureReason::AlignmentFailed});
    CHECK(file.current().draft.text == file.snapshots()->launchResult.text.utf8);
}

void commandsCheckTheirAnchorsAgainstTheCurrentRevision()
{
    auto file = test_support::choiceFile("start\n\xc3\xa9\xf0\x9f\x98\x80\nend\n", "start\nother\nend\n");
    const auto id = test_support::choiceTarget(file, SourceSide::Stage2, ChoiceScope::Line).source.contributionId;
    const auto issued = *file.correspondence()->anchor(id);
    CHECK(issued.state == AnchorState::Mapped && issued.result == ByteRange{6, 13});
    const auto command = [&](const ResultAnchor &anchor, std::string replacement, RevisionId expected) {
        return EditingCommand{
            expected, {anchor}, {{*anchor.result, std::move(replacement)}}, {{id, {DecisionValue::Accepted, {}}}}};
    };
    CHECK(file.applyCommand(command(issued, "stale\n", 0)) == Transition::StaleRevision);
    CHECK(file.applyCommand(command(issued, "\x80", 1)) == Transition::InvalidText);
    for (int field = 0; field < 5; ++field) {
        auto forged = issued;
        if (field == 0) forged.origin.sessionId = "other session";
        if (field == 1) forged.origin.path = "another.txt";
        if (field == 2) forged.origin.sourceObjectId = "other object";
        if (field == 3) forged.source.range.begin = 0;
        if (field == 4) forged.result = ByteRange{0, 5};
        CHECK(file.applyCommand(command(forged, "wrong\n", 1)) == Transition::InvalidAnchor);
    }
    CHECK(file.current().id == 1 && !file.canUndo());
    // A manual edit moves the anchor, so the copy issued before it is stale.
    CHECK(file.editResult(1, {{0, 0}, "typed\n"}) == Transition::Applied);
    const auto moved = *file.correspondence()->anchor(id);
    CHECK(moved.result == ByteRange{12, 19} && moved.revision == file.current().id);
    CHECK(file.applyCommand(command(issued, "stale\n", file.current().id)) == Transition::StaleRevision);
    CHECK(file.applyCommand(command(moved, "valid\n", file.current().id)) == Transition::Applied);
    const auto chosen = file.current();
    CHECK(chosen.draft.text == "typed\nstart\nvalid\nend\n");
    CHECK(chosen.draft.decisions.at(id) == Decision{DecisionValue::Accepted, {}});
    CHECK(file.undo(chosen.id) == Transition::Applied);
    CHECK(file.correspondence()->anchor(id)->result == ByteRange{12, 19});
    CHECK(file.correspondence()->anchor(id)->revision == file.current().id);
    CHECK(file.redo(file.current().id) == Transition::Applied && file.current().draft == chosen.draft);
}

void launchTextOutsideTheInvariantIsUnreadable()
{
    for (const auto *text : {"bad \xff\n", "crlf\r\n"}) {
        const FileSession plain("plain", "plain.txt", {text, {}, 0});
        CHECK(plain.support() == Support::Unreadable
              && plain.loadFailure() == LoadFailure{LoadFailureReason::InvalidText});
        CHECK(captured(text).support() == Support::Unreadable && !captured(text).correspondence());
    }
    // A conflict block written with carriage returns is unreadable too.
    const std::string block = "<<<<<<< ours\r\nleft\r\n=======\r\nright\r\n>>>>>>> theirs\r\n";
    CHECK(FileSession("id", "file", {block, {}, 0}).support() == Support::Unreadable);
    // An adapter's own diagnosis stays in place.
    CHECK(FileSession("outside", "image.png", {"\xff", {}, 0},
                      {.support = Support::OutsideApp, .supportDetail = "binary"})
              .supportDetail()
          == "binary");
}

void sourceCapturesSurviveEditsHistoryCheckpointsAndStagedDiscards()
{
    auto input = capturedSources();
    const auto expected = input;
    FileSession file("id", input.path, draft(input.launchResult.text.utf8), {.snapshots = input});
    const auto *capture = file.snapshots();
    const auto unchanged = [&] {
        CHECK(capture != nullptr && file.snapshots() == capture);
        CHECK(*file.snapshots() == expected);
    };
    unchanged();
    engaged(input.stage2).rawBytes = "changed caller-owned source";
    input.provenance.sessionId = "another session";
    input.launchResult.text.bytes.clear();
    unchanged();
    CHECK(file.current().draft.text == "manual\nkept");
    CHECK(file.replaceDraft(file.current().id, draft("edited\n")) == Transition::Applied);
    CHECK(file.recordCheckpoint(file.current().id, Outcome::Succeeded) == Transition::Applied);
    unchanged();
    auto later = draft("later\n");
    later.decisions["right"] = {DecisionValue::Accepted, 2};
    CHECK(file.replaceDraft(file.current().id, later) == Transition::Applied);
    unchanged();
    CHECK(file.undo(file.current().id) == Transition::Applied);
    CHECK(file.current().draft == draft("edited\n"));
    unchanged();
    CHECK(file.redo(file.current().id) == Transition::Applied);
    CHECK(file.current().draft == later);
    unchanged();
    CHECK(file.undo(file.current().id) == Transition::Applied);
    CHECK(file.replaceDraft(file.current().id, draft("new history branch\n")) == Transition::Applied);
    CHECK(!file.canRedo());
    unchanged();
    CHECK(file.discardToCheckpoint(file.current().id) == Transition::Applied);
    CHECK(file.current().draft == draft("edited\n"));
    unchanged();
    resolve(file);
    CHECK(file.replaceDraft(file.current().id, draft("after staging\n")) == Transition::Applied);
    CHECK(file.discardToStaged(file.current().id) == Transition::Applied);
    CHECK(file.current().draft == draft("edited\n"));
    unchanged();
    const auto copy = file;
    auto moved = std::move(file);
    CHECK(copy.snapshots() == capture && moved.snapshots() == capture);
    CHECK(*copy.snapshots() == expected && *moved.snapshots() == expected);
}

void anUnavailableEarlierSourceRemainsDistinctFromACapturedEmptySource()
{
    for (const bool missing : {false, true}) {
        auto captured = capturedSources();
        if (missing)
            captured.earlier.reset();
        else {
            auto &earlier = engaged(captured.earlier);
            earlier.rawBytes.clear();
            earlier.worktree = {};
        }
        const auto expected = captured;
        FileSession file("id", captured.path, draft(captured.launchResult.text.utf8), {.snapshots = captured});
        CHECK(file.snapshots() && file.snapshots()->earlier.has_value() == !missing);
        if (!missing) {
            const auto &earlier = engaged(file.snapshots()->earlier);
            CHECK(earlier.rawBytes.empty());
            CHECK(earlier.worktree.utf8.empty());
            CHECK(earlier.worktree.bytes.empty());
        }
        CHECK(file.replaceDraft(file.current().id, draft("edited")) == Transition::Applied);
        CHECK(file.undo(file.current().id) == Transition::Applied);
        CHECK(file.redo(file.current().id) == Transition::Applied);
        CHECK(*file.snapshots() == expected);
    }
    FileSession uncaptured("id", "outside", {}, {.support = Support::OutsideApp, .supportDetail = "Missing source"});
    CHECK(uncaptured.snapshots() == nullptr);
}

void sourceCapturesRejectADifferentPathOrLaunchResult()
{
    const auto captured = capturedSources();
    for (const bool wrongPath : {false, true}) {
        bool rejected = false;
        try {
            FileSession invalid("id", wrongPath ? "another path" : captured.path,
                                draft(wrongPath ? captured.launchResult.text.utf8 : "regenerated source"),
                                {.snapshots = captured});
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        CHECK(rejected);
    }
}

void identityAndInitialBlockers()
{
    for (const bool emptyId : {false, true}) {
        bool rejected = false;
        try {
            FileSession invalid(emptyId ? "" : "id", emptyId ? "path" : "", draft());
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        CHECK(rejected);
    }
    FileSession file("id", ":literal", draft());
    CHECK(file.id() == "id" && file.path() == ":literal");
    CHECK(file.current().id == 1 && file.current().result == 1 && file.current().decisions == 1);
    CHECK(file.facts().unsaved && file.facts().needsReview && !file.facts().resolved);
    CHECK(has(file, Blocker::UnsavedDraft) && has(file, Blocker::ReviewNeeded));
    CHECK(has(file, Blocker::WriteRequired) && has(file, Blocker::StageRequired));
    CHECK(file.undo(1) == Transition::NoChange && file.redo(1) == Transition::NoChange);
}

void transactionDimensionsAndStaleEditingCommands()
{
    FileSession file("id", "file", draft());
    CHECK(file.replaceDraft(1, draft()) == Transition::NoChange);
    CHECK(file.replaceDraft(1, draft("changed")) == Transition::Applied);
    const auto textRevision = file.current();
    CHECK(textRevision.result == textRevision.id && textRevision.decisions == 1);
    auto decisions = textRevision.draft;
    decisions.decisions["right"] = {DecisionValue::Accepted, 2};
    CHECK(file.replaceDraft(textRevision.id, decisions) == Transition::Applied);
    CHECK(file.current().result == textRevision.result);
    CHECK(file.current().decisions == file.current().id);
    const auto current = file.current().id;
    CHECK(file.replaceDraft(1, draft("stale")) == Transition::StaleRevision);
    CHECK(file.undo(1) == Transition::StaleRevision && file.redo(1) == Transition::StaleRevision);
    CHECK(file.discardToCheckpoint(1) == Transition::StaleRevision);
    CHECK(file.discardToStaged(1) == Transition::StaleRevision);
    CHECK(file.beginMarkResolved(1) == Transition::StaleRevision);
    CHECK(file.current().id == current && file.current().draft == decisions);
    // The marker count comes from the text, so a supplied count changes nothing.
    decisions.unresolvedMarkers = 1;
    CHECK(file.replaceDraft(current, decisions) == Transition::NoChange);
    CHECK(file.current().id == current && file.current().draft.unresolvedMarkers == 0);
}

void saveCheckpointsOnlyAndKeepsCommandHistory()
{
    FileSession file("id", "file", draft());
    CHECK(file.replaceDraft(1, draft("saved")) == Transition::Applied);
    const auto saved = file.current();
    CHECK(file.recordCheckpoint(saved.id, Outcome::Succeeded) == Transition::Applied);
    CHECK(!file.facts().unsaved && file.facts().needsReview && !file.facts().resolved);
    CHECK(!file.publication().reviewed() && !file.publication().written() && !file.publication().staged());
    CHECK(file.facts().status == FileStatus::SavedDraft && file.canUndo());
    CHECK(file.replaceDraft(saved.id, draft("newer")) == Transition::Applied);
    CHECK(file.undo(file.current().id) == Transition::Applied);
    CHECK(file.current().id > saved.id && file.current().draft == saved.draft);
    CHECK(file.current().result == saved.result && file.current().decisions == saved.decisions);
    CHECK(!file.facts().unsaved && file.facts().needsReview && file.canRedo());
    CHECK(file.recordCheckpoint(saved.id - 1, Outcome::Succeeded) == Transition::StaleRevision);
    CHECK(engaged(file.publication().checkpoint()) == saved.id);
}

void historyRestoresDecisionsAndCutsTheRedoBranch()
{
    FileSession file("id", "file", draft());
    auto combined = draft("first then second");
    combined.decisions["right"] = {DecisionValue::Accepted, 2};
    CHECK(file.replaceDraft(1, combined) == Transition::Applied);
    const auto combinedRevision = file.current();
    resolve(file);
    CHECK(file.undo(file.current().id) == Transition::Applied);
    CHECK(file.current().draft == draft() && file.facts().needsReview && !file.facts().resolved);
    CHECK(file.redo(file.current().id) == Transition::Applied);
    CHECK(file.current().draft == combined && file.current().id != combinedRevision.id);
    CHECK(file.current().result == combinedRevision.result && file.current().decisions == combinedRevision.decisions);
    CHECK(file.facts().earlierStaged && !file.facts().resolved);
    CHECK(file.undo(file.current().id) == Transition::Applied);
    CHECK(file.replaceDraft(file.current().id, draft("branch")) == Transition::Applied);
    CHECK(!file.canRedo() && file.redo(file.current().id) == Transition::NoChange);
    CHECK(file.revision(engaged(file.publication().staged()).revision)->draft == combined);
}

void confirmationMarkersAndSupportBlockPublication()
{
    auto undecided = draft();
    undecided.decisions["right"] = {};
    FileSession file("id", "file", undecided);
    CHECK(file.beginMarkResolved(1) == Transition::ConfirmationRequired);
    CHECK(file.current().draft == undecided && !file.publication().reviewed());
    CHECK(file.beginMarkResolved(1, true) == Transition::Applied);
    CHECK(engaged(file.publication().reviewed()).confirmedUndecided && !has(file, Blocker::Undecided));
    undecided.text = "<<<<<<< ours\nresult\n=======\nother\n>>>>>>> theirs\n";
    CHECK(file.replaceDraft(1, undecided) == Transition::Applied);
    CHECK(file.current().draft.unresolvedMarkers == 1);
    CHECK(file.beginMarkResolved(file.current().id, true) == Transition::Blocked);
    CHECK(has(file, Blocker::UnresolvedMarkers) && has(file, Blocker::Undecided));
    for (const auto support : {Support::OutsideApp, Support::Unreadable}) {
        CHECK(file.setSupport(support, "unavailable") == Transition::Applied);
        const auto id = file.current().id;
        CHECK(file.replaceDraft(id, draft()) == Transition::Blocked);
        CHECK(file.undo(id) == Transition::Blocked && file.redo(id) == Transition::Blocked);
        CHECK(file.discardToCheckpoint(id) == Transition::Blocked);
        CHECK(file.discardToStaged(id) == Transition::Blocked);
        CHECK(file.beginMarkResolved(id, true) == Transition::Blocked && !file.facts().resolved);
        CHECK(file.supportDetail() == "unavailable");
        // No app draft blockers for a file the app cannot edit.
        const auto expected = support == Support::OutsideApp ? Blocker::OutsideApp : Blocker::Unreadable;
        CHECK(file.facts().blockers == std::vector<Blocker>{expected});
        CHECK(file.facts().status
              == (support == Support::OutsideApp ? FileStatus::OutsideApp : FileStatus::Unreadable));
    }
}

void aLoneMarkerLineBlocksMarkResolved()
{
    for (const auto *line : {"<<<<<<< ours", "||||||| base", "=======", ">>>>>>> theirs"}) {
        std::string text = "before\n";
        text += line;
        text += "\nafter\n";
        FileSession file("id", "file", {text, {}, 0});
        CHECK(file.current().draft.unresolvedMarkers == 1);
        CHECK(file.beginMarkResolved(1) == Transition::Blocked && has(file, Blocker::UnresolvedMarkers));
    }
}

void markerLikeTextDoesNotBlockMarkResolved()
{
    const std::string text = "x <<<<<<< ours\n  =======\n<<<<<<<< eight\n======== heading\n"
                             ">>>>>>>theirs\n|||||| six\nend >>>>>>> theirs\n";
    FileSession file("id", "file", {text, {}, 7});
    CHECK(file.current().draft.unresolvedMarkers == 0);
    CHECK(file.beginMarkResolved(1) == Transition::Applied);
}

void deletingOneMarkerLineKeepsThePublishBlocker()
{
    const std::string text = "a\n<<<<<<< ours\nleft\n||||||| base\nbase\n=======\nright\n>>>>>>> theirs\nz\n";
    const auto block = scanMarkers(text).blocks.at(0);
    for (const auto &line : {block.begin, engaged(block.base), block.separator, block.end}) {
        FileSession file("id", "file", {text, {}, 0});
        CHECK(file.beginMarkResolved(1) == Transition::Blocked);
        CHECK(file.editResult(1, {line.bytes, ""}) == Transition::Applied);
        const auto remaining = file.markers();
        CHECK(remaining.blocks.size() == (line.kind == MarkerKind::Base ? 1u : 0u));
        CHECK(file.current().draft.unresolvedMarkers == remaining.count());
        CHECK(file.current().draft.unresolvedMarkers != 0);
        CHECK(file.beginMarkResolved(file.current().id, true) == Transition::Blocked);
        CHECK(has(file, Blocker::UnresolvedMarkers) && !file.publication().reviewed());
    }
    FileSession cleared("id", "file", {text, {}, 0});
    CHECK(cleared.replaceDraft(1, {"a\nleft\nz\n", {}, 3}) == Transition::Applied);
    CHECK(cleared.current().draft.unresolvedMarkers == 0 && !has(cleared, Blocker::UnresolvedMarkers));
    CHECK(cleared.beginMarkResolved(cleared.current().id) == Transition::Applied);
}

void aFileIsSettledOnceEveryChoiceIsDecidedAndNoMarkerRemains()
{
    auto file = test_support::markedFile();
    CHECK(!file.facts().settled);
    CHECK(choose(file, SourceSide::Stage2, ChoiceAction::ResolveUsingSide) == Transition::Applied);
    // The second block is still open.
    CHECK(!file.facts().settled && file.facts().status == FileStatus::Undecided);
    CHECK(choose(file, SourceSide::Stage3, ChoiceAction::ResolveUsingSide, ChoiceScope::Section, 1)
          == Transition::Applied);
    CHECK(file.facts().undecided == 0 && file.current().draft.unresolvedMarkers == 0);
    CHECK(file.facts().settled && file.facts().status == FileStatus::ReviewNeeded && !file.facts().resolved);
    // Settling is not review: Mark resolved is still required.
    CHECK(has(file, Blocker::ReviewNeeded) && has(file, Blocker::WriteRequired) && has(file, Blocker::StageRequired));
    CHECK(file.undo(file.current().id) == Transition::Applied);
    CHECK(!file.facts().settled && file.facts().undecided != 0);
    CHECK(file.redo(file.current().id) == Transition::Applied);
    CHECK(file.facts().settled);
    const auto revision = file.current().id;
    CHECK(file.recordCheckpoint(revision, Outcome::Succeeded) == Transition::Applied);
    CHECK(file.facts().status == FileStatus::SavedDraft && file.facts().settled);
    resolve(file);
    CHECK(file.facts().resolved && !file.facts().settled);

    // Every contribution decided while marker lines stay is not settled.
    auto ignored = test_support::markedFile();
    for (const std::size_t section : {0U, 1U}) {
        CHECK(choose(ignored, SourceSide::Stage2, ChoiceAction::Ignore, ChoiceScope::Section, section)
              == Transition::Applied);
        CHECK(choose(ignored, SourceSide::Stage3, ChoiceAction::Ignore, ChoiceScope::Section, section)
              == Transition::Applied);
    }
    CHECK(ignored.facts().undecided == 0 && ignored.current().draft.unresolvedMarkers != 0);
    CHECK(!ignored.facts().settled);
}

void aHeadingUnderlineBothSidesKeepDoesNotBlockMarkResolved()
{
    auto chosen = test_support::headingFile();
    // Only the conflict block counts at launch.
    CHECK(chosen.current().draft.unresolvedMarkers == 1 && chosen.markers().orphans.empty());
    CHECK(chosen.markers().blocks.size() == 1 && engaged(chosen.markers().first()).line == 3);
    CHECK(chosen.markerSections().size() == 1);
    CHECK(choose(chosen, SourceSide::Stage2, ChoiceAction::ResolveUsingSide) == Transition::Applied);
    CHECK(chosen.current().draft.text == "Changes\n=======\nintro\nleft line\ntail\n");
    CHECK(chosen.current().draft.unresolvedMarkers == 0 && !has(chosen, Blocker::UnresolvedMarkers));
    CHECK(chosen.beginMarkResolved(chosen.current().id) == Transition::Applied);
    // Resolving the block by hand clears the blocker too, and the heading stays.
    auto edited = test_support::headingFile();
    auto draft = edited.current().draft;
    draft.text = "Changes\n=======\nintro\nright line\ntail\n";
    CHECK(edited.replaceDraft(1, draft) == Transition::Applied);
    CHECK(edited.current().draft.unresolvedMarkers == 0 && edited.markers().count() == 0);
    CHECK(edited.beginMarkResolved(edited.current().id, true) == Transition::Applied);
    // Without captured sources the plain scan still counts the underline.
    FileSession plain("id", "file", {test_support::headingFile().current().draft.text, {}, 0});
    CHECK(plain.current().draft.unresolvedMarkers == 2 && plain.markers().orphans.front().line == 1);
}

void markerLinesGitInsertedStillBlockWholeOrOrphaned()
{
    for (const auto kind : {MarkerKind::Begin, MarkerKind::Separator, MarkerKind::End}) {
        auto file = test_support::headingFile();
        const auto block = file.markers().blocks.front();
        const auto line = kind == MarkerKind::Begin ? block.begin
            : kind == MarkerKind::Separator         ? block.separator
                                                    : block.end;
        CHECK(file.editResult(1, {line.bytes, ""}) == Transition::Applied);
        const auto remaining = file.markers();
        CHECK(remaining.blocks.empty() && remaining.orphans.size() == 2);
        CHECK(std::none_of(remaining.orphans.begin(), remaining.orphans.end(),
                           [](const auto &orphan) { return orphan.line == 1; }));
        CHECK(file.current().draft.unresolvedMarkers == 2);
        CHECK(file.beginMarkResolved(file.current().id, true) == Transition::Blocked);
        CHECK(has(file, Blocker::UnresolvedMarkers) && !file.publication().reviewed());
    }
}

void anOutsideAppFileKeepsItsSupportBlockerAndRecordedFailuresOnly()
{
    FileSession file("id", "file", draft());
    CHECK(file.beginMarkResolved(1) == Transition::Applied);
    CHECK(file.recordCheckpoint(1, Outcome::Succeeded) == Transition::Applied);
    CHECK(file.recordWrite(1, Outcome::Failed, "disk full") == Transition::Applied);
    CHECK(file.setSupport(Support::OutsideApp, "binary now") == Transition::Applied);
    const auto facts = file.facts();
    CHECK((facts.blockers == std::vector<Blocker>{Blocker::OutsideApp, Blocker::WriteFailed}));
    CHECK(!facts.resolved && facts.status == FileStatus::OutsideApp);
    FileSession uncertain("id", "file", draft());
    CHECK(uncertain.beginMarkResolved(1) == Transition::Applied);
    CHECK(uncertain.recordCheckpoint(1, Outcome::Succeeded) == Transition::Applied);
    CHECK(uncertain.recordWrite(1, Outcome::Uncertain, "lost reply") == Transition::Applied);
    CHECK(uncertain.setSupport(Support::Unreadable, "gone") == Transition::Applied);
    CHECK((uncertain.facts().blockers == std::vector<Blocker>{Blocker::Unreadable, Blocker::UncertainOutcome}));
    CHECK(!uncertain.facts().resolved && uncertain.facts().status == FileStatus::Unreadable);
}

void outcomesRequireTheReviewedSavedAndWrittenTransaction()
{
    FileSession file("id", "file", draft());
    CHECK(file.recordWrite(1, Outcome::Succeeded) == Transition::StaleRevision);
    CHECK(file.recordStage(1, Outcome::Succeeded, "object", "100644") == Transition::StaleRevision);
    CHECK(file.recordCheckpoint(999, Outcome::Succeeded) == Transition::StaleRevision);
    CHECK(file.beginMarkResolved(1) == Transition::Applied);
    CHECK(file.recordWrite(1, Outcome::Succeeded) == Transition::InvalidOutcome);
    for (const auto outcome : {Outcome::Failed, Outcome::Uncertain}) {
        CHECK(file.recordCheckpoint(1, outcome, "save diagnostic") == Transition::Applied);
        CHECK(file.recordWrite(1, Outcome::Succeeded) == Transition::InvalidOutcome);
        CHECK(engaged(file.publication().reviewed()).revision == 1 && !file.facts().resolved);
        CHECK(engaged(file.publication().checkpointOutcome()).detail == "save diagnostic");
    }
    CHECK(file.recordCheckpoint(1, Outcome::Succeeded) == Transition::Applied);
    CHECK(file.recordStage(1, Outcome::Succeeded, "object", "100644") == Transition::InvalidOutcome);
    CHECK(file.recordWrite(1, Outcome::Failed, "write failed") == Transition::Applied);
    CHECK(file.recordStage(1, Outcome::Succeeded, "object", "100644") == Transition::InvalidOutcome);
    CHECK(file.recordWrite(1, Outcome::Succeeded) == Transition::Applied);
    CHECK(file.recordStage(1, Outcome::Succeeded, "", "100644") == Transition::InvalidOutcome);
    CHECK(file.recordStage(1, Outcome::Succeeded, "object", "") == Transition::InvalidOutcome);
    CHECK(!file.publication().staged() && !file.facts().resolved);
    CHECK(file.recordStage(1, Outcome::Succeeded, "object", "100644") == Transition::Applied);
    CHECK(file.facts().resolved && file.facts().status == FileStatus::Resolved);
}

// A failed or uncertain outcome for `step` of a resolved file keeps every
// recorded revision and blocks with that step's failure.
void anUnsuccessfulOutcomeRetainsSnapshots(Outcome outcome, Step step)
{
    FileSession file("id", "file", draft());
    resolve(file);
    if (step == Step::Checkpoint) CHECK(file.recordCheckpoint(1, outcome, "diagnostic") == Transition::Applied);
    if (step == Step::Write) CHECK(file.recordWrite(1, outcome, "diagnostic") == Transition::Applied);
    if (step == Step::Stage) CHECK(file.recordStage(1, outcome, {}, {}, "diagnostic") == Transition::Applied);
    CHECK(engaged(file.publication().checkpoint()) == 1 && engaged(file.publication().written()) == 1
          && engaged(file.publication().staged()).revision == 1);
    CHECK(!file.facts().resolved);
    const std::array failures{Blocker::CheckpointFailed, Blocker::WriteFailed, Blocker::StageFailed};
    const auto index = static_cast<std::size_t>(step);
    CHECK(has(file, outcome == Outcome::Uncertain ? Blocker::UncertainOutcome : failures.at(index)));
    const std::array statuses{FileStatus::CheckpointFailed, FileStatus::WriteFailed, FileStatus::StageFailed};
    CHECK(file.facts().status == (outcome == Outcome::Uncertain ? FileStatus::OutcomeUncertain : statuses.at(index)));
}

void checkpointDiscardFencesDelayedCallbacksEvenAfterUndo()
{
    FileSession file("id", "file", draft());
    CHECK(file.recordCheckpoint(1, Outcome::Succeeded) == Transition::Applied);
    CHECK(file.replaceDraft(1, draft("discarded")) == Transition::Applied);
    const auto discarded = file.current().id;
    CHECK(file.beginMarkResolved(discarded) == Transition::Applied);
    CHECK(file.undo(discarded) == Transition::Applied);
    CHECK(file.current().draft == draft());
    CHECK(file.discardToCheckpoint(file.current().id) == Transition::Applied);
    CHECK(file.recordCheckpoint(discarded, Outcome::Succeeded) == Transition::StaleRevision);
    CHECK(file.recordWrite(discarded, Outcome::Succeeded) == Transition::StaleRevision);
    CHECK(file.recordStage(discarded, Outcome::Succeeded, "object", "100644") == Transition::StaleRevision);
    CHECK(engaged(file.publication().checkpoint()) == 1 && !file.facts().unsaved && file.facts().needsReview);
}

void discardWithoutCheckpointRestoresInitialDraftAndRejectsDelayedSave()
{
    FileSession file("id", "file", draft());
    CHECK(file.discardToCheckpoint(1) == Transition::NoChange);
    CHECK(file.replaceDraft(1, draft("discarded")) == Transition::Applied);
    const auto discarded = file.current().id;
    CHECK(file.discardToCheckpoint(discarded) == Transition::Applied);
    CHECK(file.current().draft == draft() && !file.publication().checkpoint());
    CHECK(file.recordCheckpoint(discarded, Outcome::Succeeded) == Transition::StaleRevision);
    CHECK(file.facts().unsaved && !file.facts().resolved);
}

void stagedDiscardRestoresVerifiedIdentityAndFencesNewerCallbacks()
{
    FileSession file("id", "file", draft());
    CHECK(file.discardToStaged(1) == Transition::Blocked);
    resolve(file);
    CHECK(file.discardToStaged(1) == Transition::NoChange);
    CHECK(file.replaceDraft(1, draft("discarded")) == Transition::Applied);
    const auto discarded = file.current().id;
    CHECK(file.recordCheckpoint(discarded, Outcome::Succeeded) == Transition::Applied);
    CHECK(file.discardToStaged(discarded) == Transition::Applied);
    CHECK(file.current().id == 1 && engaged(file.publication().checkpoint()) == 1 && file.facts().resolved);
    CHECK(file.recordCheckpoint(discarded, Outcome::Failed) == Transition::StaleRevision);
    CHECK(file.recordWrite(discarded, Outcome::Succeeded) == Transition::StaleRevision);
    CHECK(file.recordStage(discarded, Outcome::Succeeded, "other", "100644") == Transition::StaleRevision);
    CHECK(file.facts().resolved);
}

void stagedDiscardCannotPretendANewerWriteDidNotHappen(Outcome outcome)
{
    FileSession file("id", "file", draft());
    resolve(file);
    CHECK(file.replaceDraft(1, draft("new")) == Transition::Applied);
    const auto newer = file.current().id;
    CHECK(file.beginMarkResolved(newer) == Transition::Applied);
    CHECK(file.recordCheckpoint(newer, Outcome::Succeeded) == Transition::Applied);
    CHECK(file.recordWrite(newer, outcome) == Transition::Applied);
    CHECK(file.discardToStaged(newer) == Transition::Blocked);
    CHECK(file.current().id == newer && engaged(file.publication().staged()).revision == 1);
}

void aDelayedReviewWriteAfterANewerSave(Outcome outcome, bool saveBeforeReview)
{
    FileSession file("id", "file", draft());
    if (saveBeforeReview) CHECK(file.recordCheckpoint(1, Outcome::Succeeded) == Transition::Applied);
    CHECK(file.beginMarkResolved(1) == Transition::Applied);
    if (!saveBeforeReview) CHECK(file.recordCheckpoint(1, Outcome::Succeeded) == Transition::Applied);
    CHECK(file.replaceDraft(1, draft("new saved draft")) == Transition::Applied);
    const auto newer = file.current().id;
    CHECK(file.recordCheckpoint(newer, Outcome::Succeeded) == Transition::Applied);
    CHECK(file.recordWrite(1, outcome, "delayed write diagnostic") == Transition::Applied);
    CHECK(engaged(file.publication().writeOutcome()).revision == 1
          && engaged(file.publication().writeOutcome()).outcome == outcome);
    CHECK(engaged(file.publication().writeOutcome()).detail == "delayed write diagnostic");
    CHECK(engaged(file.publication().checkpoint()) == newer && file.current().id == newer);
    CHECK(!file.facts().unsaved && file.facts().needsReview && !file.facts().resolved);
    CHECK(engaged(file.publication().reviewed()).revision == 1);
    if (outcome == Outcome::Succeeded) {
        CHECK(engaged(file.publication().written()) == 1
              && file.revision(engaged(file.publication().written()))->draft == draft());
        CHECK(file.facts().status == FileStatus::WrittenUnresolved);
        CHECK(file.recordStage(1, Outcome::Succeeded, "earlier", "100644") == Transition::Applied);
        CHECK(file.facts().earlierStaged && !file.facts().resolved);
    } else {
        CHECK(!file.publication().written());
        CHECK(file.facts().status
              == (outcome == Outcome::Failed ? FileStatus::WriteFailed : FileStatus::OutcomeUncertain));
    }
}

void handEditsKeepTheirRangesThroughUndoRedoChoicesAndReplay()
{
    using Ranges = std::vector<ByteRange>;
    const auto ranges = [](const FileSession &file) { return file.correspondence()->handEditedRanges(); };
    // "header" and "bridge" are lines both sides share, outside every region.
    auto file = test_support::choiceFile();
    CHECK(ranges(file).empty());
    CHECK(file.editResult(1, {{0, 6}, "HEADER"}) == Transition::Applied);
    const auto typed = file.current();
    CHECK((ranges(file) == Ranges{{0, 6}}) && file.correspondence()->handEditedAnchors().empty());
    // Deleting a line by hand leaves an empty range where it was.
    const auto bridge = typed.draft.text.find("bridge\n");
    CHECK(file.editResult(file.current().id, {{bridge, bridge + 7}, ""}) == Transition::Applied);
    const Ranges deleted{{0, 6}, {bridge, bridge}};
    CHECK(ranges(file) == deleted);
    CHECK(file.undo(file.current().id) == Transition::Applied && (ranges(file) == Ranges{{0, 6}}));
    CHECK(file.undo(file.current().id) == Transition::Applied && ranges(file).empty());
    CHECK(file.redo(file.current().id) == Transition::Applied && (ranges(file) == Ranges{{0, 6}}));
    CHECK(file.redo(file.current().id) == Transition::Applied && ranges(file) == deleted);
    // Typing inside a region adds a range there too. A choice that replaces
    // that text drops it and keeps the hand edits outside the region.
    const auto left = file.current().draft.text.find("left first");
    CHECK(file.editResult(file.current().id, {{left, left + 4}, "LEFT"}) == Transition::Applied);
    CHECK((ranges(file) == Ranges{{0, 6}, {left, left + 4}, {bridge, bridge}}));
    CHECK(choose(file, SourceSide::Stage3, ChoiceAction::ResolveUsingSide) == Transition::Applied);
    const auto chosen = file.current().draft.text;
    const auto right = chosen.find("right first\nright second\n");
    CHECK(right != std::string::npos);
    const auto after = ranges(file);
    CHECK(after.size() == 2 && (after.front() == ByteRange{0, 6}));
    CHECK(std::none_of(after.begin(), after.end(),
                       [&](const auto &range) { return range.begin < right + 25 && right < range.end; }));
    // A whole draft from the editor counts the lines it changed.
    auto whole = test_support::choiceFile();
    auto draft = whole.current().draft;
    const auto shared = draft.text.find("bridge");
    draft.text.replace(shared, 6, "BRIDGE");
    CHECK(whole.replaceDraft(1, draft) == Transition::Applied);
    CHECK(ranges(whole).size() == 1 && ranges(whole).front().begin <= shared
          && shared + 6 <= ranges(whole).front().end);
    // Recovery replay restores the recorded ranges. A record without them,
    // as written before they were recorded, replays with none.
    auto replay = test_support::choiceFile();
    CHECK(test_support::replay(replay, typed.draft, {}, {{0, 6}}) == Transition::Applied);
    CHECK((ranges(replay) == Ranges{{0, 6}}));
    CHECK(test_support::replay(replay, typed.draft, {}, {{0, 6}}) == Transition::NoChange);
    CHECK(test_support::replay(replay, typed.draft, {}, {{0, 3}}) == Transition::Applied);
    CHECK(replay.undo(replay.current().id) == Transition::Applied && (ranges(replay) == Ranges{{0, 6}}));
    auto older = test_support::choiceFile();
    CHECK(test_support::replay(older, typed.draft) == Transition::Applied);
    CHECK(ranges(older).empty() && older.current().draft == typed.draft);
    // Ranges must be in order, apart and inside the text.
    auto refused = test_support::choiceFile();
    for (const auto &bad :
         std::vector<Ranges>{{{4, 2}}, {{0, 3}, {3, 6}}, {{3, 6}, {0, 2}}, {{0, typed.draft.text.size() + 1}}})
        CHECK(test_support::replay(refused, typed.draft, {}, bad) == Transition::InvalidAnchor);
    CHECK(refused.current().id == 1 && ranges(refused).empty());
}

void aNewerSuccessfulSaveDoesNotAuthorizeAnUnsuccessfulReviewCheckpoint()
{
    for (const auto outcome : {Outcome::Failed, Outcome::Uncertain}) {
        FileSession file("id", "file", draft());
        CHECK(file.beginMarkResolved(1) == Transition::Applied);
        CHECK(file.recordCheckpoint(1, outcome) == Transition::Applied);
        CHECK(file.replaceDraft(1, draft("newer")) == Transition::Applied);
        CHECK(file.recordCheckpoint(file.current().id, Outcome::Succeeded) == Transition::Applied);
        CHECK(file.recordWrite(1, Outcome::Succeeded) == Transition::InvalidOutcome);
        CHECK(!file.publication().written() && !file.facts().resolved);
    }
}

void aNewerReviewRejectsSupersededPublicationCallbacks()
{
    FileSession file("id", "file", draft());
    CHECK(file.beginMarkResolved(1) == Transition::Applied);
    CHECK(file.recordCheckpoint(1, Outcome::Succeeded) == Transition::Applied);
    CHECK(file.replaceDraft(1, draft("newer")) == Transition::Applied);
    const auto newer = file.current().id;
    CHECK(file.beginMarkResolved(newer) == Transition::Applied);
    CHECK(file.recordWrite(1, Outcome::Succeeded) == Transition::StaleRevision);
    CHECK(file.recordStage(1, Outcome::Succeeded, "earlier", "100644") == Transition::StaleRevision);
    CHECK(!file.publication().written() && !file.publication().staged()
          && engaged(file.publication().reviewed()).revision == newer);
}

void supportIsAdmittedOnlyThroughTheLaunchChecks()
{
    // A hold the session made itself stays until its own command lifts it.
    FileSession invalid("plain", "plain.txt", {"carriage\r\nreturn\n", {}, 0});
    CHECK(invalid.support() == Support::Unreadable);
    CHECK(invalid.setSupport(Support::Supported) == Transition::Blocked);
    CHECK(invalid.support() == Support::Unreadable
          && engaged(invalid.loadFailure()).reason == LoadFailureReason::InvalidText);
    auto large = captured(lines(kLargeFileLines + 1));
    CHECK(large.setSupport(Support::Supported) == Transition::Blocked);
    CHECK(large.support() == Support::LargeFile && !large.correspondence());
    CHECK(large.setSupport(Support::LargeFile) == Transition::InvalidOutcome);
    CHECK(large.loadLargeFile() == Transition::Applied && large.correspondence());
    // Text an adapter held back was never checked, so admitting it checks it.
    FileSession binary("binary", "binary.txt", {"carriage\r\nreturn\n", {}, 0},
                       {.support = Support::OutsideApp, .supportDetail = "binary"});
    CHECK(binary.setSupport(Support::Supported) == Transition::Blocked && binary.support() == Support::OutsideApp);
    // A captured file the adapter held back is aligned when it is admitted.
    auto sources = *test_support::choiceFile().snapshots();
    const auto text = sources.launchResult.text.utf8;
    FileSession held("choices", sources.path, {text, {}, 0},
                     {.support = Support::OutsideApp, .supportDetail = "busy", .snapshots = sources});
    CHECK(!held.correspondence() && held.choices().empty());
    CHECK(held.setSupport(Support::Supported) == Transition::Applied);
    CHECK(held.support() == Support::Supported && held.supportDetail().empty());
    CHECK(held.correspondence() && held.correspondence()->revision() == held.current().id);
    CHECK(!held.choices().empty() && !held.current().draft.decisions.empty());
    CHECK(held.editResult(held.current().id, {{0, 0}, "typed\n"}) == Transition::Applied);
    // Once it has history, holding it back and admitting it again keeps that history.
    CHECK(held.setSupport(Support::Unreadable, "gone") == Transition::Applied);
    CHECK(held.setSupport(Support::Supported) == Transition::Applied && held.canUndo());
    // An untouched capture held back is not admitted once it has outcomes.
    FileSession saved("choices", sources.path, {text, {}, 0},
                      {.support = Support::OutsideApp, .supportDetail = "busy", .snapshots = sources});
    CHECK(saved.recordCheckpoint(1, Outcome::Failed, "disk full") == Transition::Applied);
    CHECK(saved.setSupport(Support::Supported) == Transition::Blocked && saved.support() == Support::OutsideApp);
}

void aRecordedRevisionReplaysInOneStep()
{
    // A burst of typing joined into one undo entry, which a journal records once.
    auto live = test_support::choiceFile();
    bool join = false;
    for (const auto *typed : {"one\n", "two\n", "three\n"}) {
        CHECK(live.editResult(live.current().id, {{0, 0}, typed}, {.join = join}) == Transition::Applied);
        join = true;
    }
    const auto &last = live.current();
    CHECK(last.id == 4 && !last.correspondence->handEditedRanges().empty());
    const RecordedRevision recorded{last.id, last.result, last.draft, last.correspondence->handEditedAnchors(),
                                    last.correspondence->handEditedRanges()};
    auto replayed = test_support::choiceFile();
    auto stale = recorded;
    stale.id = 1;
    CHECK(replayed.replayDraft(1, stale) == Transition::InvalidOutcome);
    auto foreign = recorded;
    foreign.result = 2;
    CHECK(replayed.replayDraft(1, foreign) == Transition::InvalidOutcome);
    CHECK(replayed.current().id == 1);
    CHECK(replayed.replayDraft(1, recorded) == Transition::Applied);
    CHECK(replayed.current().id == 4 && replayed.current().result == 4 && replayed.current().draft == last.draft);
    CHECK(replayed.correspondence()->handEditedRanges() == last.correspondence->handEditedRanges());
    CHECK(replayed.correspondence()->handEditedAnchors() == last.correspondence->handEditedAnchors());
    // Undo takes back the whole burst in both, and later ids agree.
    CHECK(live.undo(4) == Transition::Applied && replayed.undo(4) == Transition::Applied);
    CHECK(!live.canUndo() && !replayed.canUndo() && live.current().id == 5 && replayed.current().id == 5);
    CHECK(replayed.current().draft == live.current().draft
          && live.current().draft.text == test_support::choiceFile().current().draft.text);
    // The first entry, where the history starts, is never joined.
    auto fresh = test_support::choiceFile();
    CHECK(fresh.editResult(1, {{0, 0}, "x"}, {.join = true}) == Transition::Applied && fresh.canUndo());
}

void aDeferredEditIsAlignedLaterOnlyForItsRevision()
{
    auto file = test_support::choiceFile();
    auto now = file;
    CHECK(now.editResult(1, {{0, 0}, "typed\n"}) == Transition::Applied);
    CHECK(!now.pendingAlignment());
    CHECK(file.editResult(1, {{0, 0}, "typed\n"}, {.realignment = Realignment::Deferred}) == Transition::Applied);
    const auto pending = file.pendingAlignment();
    CHECK(pending && pending == file.current().correspondence && !pending->aligned());
    // Anchors, hand edits and text follow the edit at once.
    CHECK(pending->sameAnchors(*now.correspondence()));
    CHECK(file.installAlignment(1, std::make_shared<const CorrespondenceMap>(pending->realigned()))
          == Transition::StaleRevision);
    CHECK(file.installAlignment(
              2, std::make_shared<const CorrespondenceMap>(file.revision(1)->correspondence->realigned()))
          == Transition::InvalidAnchor);
    const auto aligned = std::make_shared<const CorrespondenceMap>(pending->realigned());
    CHECK(file.installAlignment(2, aligned) == Transition::Applied);
    CHECK(!file.pendingAlignment() && file.correspondence() == aligned.get() && file.current().id == 2);
    for (const auto side : {SourceSide::Stage2, SourceSide::Stage3})
        CHECK(*file.correspondence()->passages(side) == *now.correspondence()->passages(side));
    CHECK(file.markers() == now.markers() && file.current().draft == now.current().draft);
    CHECK(file.installAlignment(2, aligned) == Transition::StaleRevision);
    // A map made for a revision a later edit replaced is never installed.
    CHECK(file.editResult(2, {{0, 0}, "more\n"}, {.realignment = Realignment::Deferred}) == Transition::Applied);
    const auto older = file.pendingAlignment();
    CHECK(file.editResult(3, {{0, 0}, "again\n"}, {.realignment = Realignment::Deferred}) == Transition::Applied);
    CHECK(file.installAlignment(3, std::make_shared<const CorrespondenceMap>(older->realigned()))
          == Transition::StaleRevision);
    // Mark resolved counts markers against aligned passages.
    CHECK(file.beginMarkResolved(4, true) == Transition::Applied && !file.pendingAlignment());
}

void aFileOverTheLineThresholdWaitsForConfirmation()
{
    CHECK(captured(lines(kLargeFileLines - 1)).support() == Support::Supported);
    CHECK(captured(lines(kLargeFileLines)).support() == Support::Supported);
    CHECK(captured(lines(kLargeFileLines)).correspondence());
    const auto text = lines(kLargeFileLines + 1);
    auto large = captured(text);
    CHECK(large.support() == Support::LargeFile && !large.correspondence());
    CHECK(large.current().draft.text == text);
    CHECK(large.loadFailure() == LoadFailure{LoadFailureReason::LargeFile, {}, kLargeFileLines + 1});
    CHECK(large.facts().status == FileStatus::LargeFile && !large.facts().resolved);
    CHECK(large.facts().blockers == std::vector<Blocker>{Blocker::LargeFile});
    CHECK(large.editResult(1, {{0, 1}, "y"}) == Transition::Blocked);
    CHECK(large.beginMarkResolved(1) == Transition::Blocked);
    CHECK(large.loadLargeFile() == Transition::Applied);
    CHECK(large.support() == Support::Supported && !large.loadFailure());
    CHECK(large.correspondence() && large.correspondence()->revision() == 2);
    CHECK(large.current().id == 2 && large.current().draft.text == text);
    CHECK(!large.canUndo() && !large.canRedo());
    CHECK(large.loadLargeFile() == Transition::NoChange);
    CHECK(large.editResult(2, {{0, 1}, "y"}) == Transition::Applied);
    CHECK(large.current().draft.text.starts_with('y') && large.canUndo());
    CHECK(large.undo(large.current().id) == Transition::Applied && large.current().draft.text == text);
    CHECK(!large.canUndo());
    // A source over the threshold holds the file back even when the result is not.
    FileSnapshots largeSource;
    largeSource.path = "limit.txt";
    largeSource.provenance.sessionId = "limits";
    largeSource.stage2 = SourceSnapshot{"source-object", "100644", text, {text, text, {}}, "source-commit", "Source"};
    largeSource.launchResult.text = {lines(10), lines(10), {}};
    CHECK(FileSession("limit", "limit.txt", {lines(10), {}, 0}, {.snapshots = largeSource}).support()
          == Support::LargeFile);
    // Without captures there is no correspondence to build, so nothing waits.
    CHECK(FileSession("plain", "plain.txt", {text, {}, 0}).support() == Support::Supported);
}

void textOutsideTheEnvelopeIsUnreadable()
{
    const auto atLine = std::string(kMaxLineBytes, 'a');
    CHECK(captured(std::string(kMaxLineBytes - 1, 'a')).support() == Support::Supported);
    CHECK(captured(atLine).support() == Support::Supported);
    CHECK(captured(atLine).current().draft.text.size() == kMaxLineBytes);
    const auto tooLong = captured(std::string(kMaxLineBytes + 1, 'a'));
    CHECK(tooLong.support() == Support::Unreadable && !tooLong.correspondence());
    CHECK(tooLong.current().draft.text.size() == kMaxLineBytes + 1);
    CHECK(engaged(tooLong.loadFailure()).reason == LoadFailureReason::TextTooLarge
          && engaged(tooLong.loadFailure()).text.limit == TextLimit::LineBytes);
    const auto fits = kMaxRecoveryPayloadBytes / kMaxLineBytes;
    CHECK(captured(longestLines(fits)).support() == Support::Supported);
    const auto abovePayload = longestLines(fits + 1);
    const auto tooHeavy = captured(abovePayload);
    CHECK(tooHeavy.support() == Support::Unreadable);
    CHECK(tooHeavy.current().draft.text.size() == abovePayload.size());
    CHECK(engaged(tooHeavy.loadFailure()).reason == LoadFailureReason::TextTooLarge
          && engaged(tooHeavy.loadFailure()).text.limit == TextLimit::RecoveryPayload);
}

void editsStayInsideTheEnvelope()
{
    const auto text = lines(kLargeFileLines);
    FileSession file("id", "file", {text, {}, 0});
    CHECK(file.support() == Support::Supported);
    CHECK(file.editResult(1, {{file.current().draft.text.size(), file.current().draft.text.size()}, "z\n"})
          == Transition::Applied);
    CHECK(file.current().draft.text == text + "z\n");
    CHECK(file.editResult(file.current().id, {{0, 1}, "y"}) == Transition::Applied);
    CHECK(file.current().draft.text.starts_with('y'));
    CHECK(file.beginMarkResolved(file.current().id) == Transition::Applied);
    const auto atLine = std::string(kMaxLineBytes, 'a');
    FileSession narrow("id", "file", {atLine, {}, 0});
    CHECK(narrow.editResult(1, {{atLine.size(), atLine.size()}, "z"}) == Transition::TextTooLarge);
    CHECK(narrow.current().draft.text == atLine);
    CHECK(narrow.beginMarkResolved(1) == Transition::Applied);
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
    suite.run("revisions outcomes name stay stored within the history size",
              revisionsOutcomesNameStayStoredWithinTheHistorySize);
    suite.run("the session aligns with the algorithm it is given", theSessionAlignsWithTheAlgorithmItIsGiven);
    suite.run("a failed opening leaves nothing half built", aFailedOpeningLeavesNothingHalfBuilt);
    suite.run("an edit that runs out of memory changes nothing", anEditThatRunsOutOfMemoryChangesNothing);
    suite.run("a defective alignment holds the file back", aDefectiveAlignmentHoldsTheFileBack);
    suite.run("commands check their anchors against the current revision",
              commandsCheckTheirAnchorsAgainstTheCurrentRevision);
    suite.run("source captures survive edits history checkpoints and staged discards",
              sourceCapturesSurviveEditsHistoryCheckpointsAndStagedDiscards);
    suite.run("an unavailable earlier source remains distinct from a captured empty source",
              anUnavailableEarlierSourceRemainsDistinctFromACapturedEmptySource);
    suite.run("source captures reject a different path or launch result",
              sourceCapturesRejectADifferentPathOrLaunchResult);
    suite.run("identity and initial blockers", identityAndInitialBlockers);
    suite.run("transaction dimensions and stale editing commands", transactionDimensionsAndStaleEditingCommands);
    suite.run("Save checkpoints only and keeps command history", saveCheckpointsOnlyAndKeepsCommandHistory);
    suite.run("history restores decisions and cuts the redo branch", historyRestoresDecisionsAndCutsTheRedoBranch);
    suite.run("confirmation, markers and support block publication", confirmationMarkersAndSupportBlockPublication);
    suite.run("a lone marker line blocks Mark resolved", aLoneMarkerLineBlocksMarkResolved);
    suite.run("marker-like text does not block Mark resolved", markerLikeTextDoesNotBlockMarkResolved);
    suite.run("deleting one marker line keeps the publish blocker", deletingOneMarkerLineKeepsThePublishBlocker);
    suite.run("a file is settled once every choice is decided and no marker remains, and undo clears it",
              aFileIsSettledOnceEveryChoiceIsDecidedAndNoMarkerRemains);
    suite.run("a heading underline both sides keep does not block Mark resolved",
              aHeadingUnderlineBothSidesKeepDoesNotBlockMarkResolved);
    suite.run("marker lines Git inserted still block, whole or orphaned",
              markerLinesGitInsertedStillBlockWholeOrOrphaned);
    suite.run("an outside-app file keeps its support blocker and recorded failures only",
              anOutsideAppFileKeepsItsSupportBlockerAndRecordedFailuresOnly);
    suite.run("outcomes require the reviewed saved and written transaction",
              outcomesRequireTheReviewedSavedAndWrittenTransaction);
    for (const auto outcome : {Outcome::Failed, Outcome::Uncertain})
        for (const auto step : {Step::Checkpoint, Step::Write, Step::Stage})
            suite.run(titled("a failed or uncertain outcome retains snapshots", {name(outcome), name(step)}),
                      [=] { anUnsuccessfulOutcomeRetainsSnapshots(outcome, step); });
    suite.run("checkpoint discard fences delayed callbacks even after undo",
              checkpointDiscardFencesDelayedCallbacksEvenAfterUndo);
    suite.run("discard without checkpoint restores initial draft and rejects delayed Save",
              discardWithoutCheckpointRestoresInitialDraftAndRejectsDelayedSave);
    suite.run("staged discard deliberately restores verified identity and fences newer callbacks",
              stagedDiscardRestoresVerifiedIdentityAndFencesNewerCallbacks);
    for (const auto outcome : {Outcome::Succeeded, Outcome::Uncertain})
        suite.run(titled("staged discard cannot pretend a newer write did not happen", {name(outcome)}),
                  [=] { stagedDiscardCannotPretendANewerWriteDidNotHappen(outcome); });
    for (const auto outcome : {Outcome::Succeeded, Outcome::Failed, Outcome::Uncertain})
        for (const bool saveBeforeReview : {false, true})
            suite.run(titled("delayed latest-review write after newer Save",
                             {name(outcome), saveBeforeReview ? "saved before review" : "reviewed before Save"}),
                      [=] { aDelayedReviewWriteAfterANewerSave(outcome, saveBeforeReview); });
    suite.run("hand edits outside every region keep their ranges through undo, redo, choices and replay",
              handEditsKeepTheirRangesThroughUndoRedoChoicesAndReplay);
    suite.run("a newer successful Save does not authorize an unsuccessful review checkpoint",
              aNewerSuccessfulSaveDoesNotAuthorizeAnUnsuccessfulReviewCheckpoint);
    suite.run("a newer review rejects superseded publication callbacks",
              aNewerReviewRejectsSupersededPublicationCallbacks);
    suite.run("support is admitted only through the launch checks", supportIsAdmittedOnlyThroughTheLaunchChecks);
    suite.run("a recorded revision replays in one step", aRecordedRevisionReplaysInOneStep);
    suite.run("a deferred edit is aligned later only for its revision", aDeferredEditIsAlignedLaterOnlyForItsRevision);
    suite.run("a file over the line threshold waits for confirmation", aFileOverTheLineThresholdWaitsForConfirmation);
    suite.run("text over the line or payload limit is unreadable", textOutsideTheEnvelopeIsUnreadable);
    suite.run("edits stay inside the text envelope", editsStayInsideTheEnvelope);
    return suite.report();
}
