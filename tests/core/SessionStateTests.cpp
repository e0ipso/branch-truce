#include "core/SessionState.h"
#include "TestHarness.h"
#include <algorithm>
#include <array>

using namespace branch_truce;

namespace {
Draft draft(std::string text = "result\n")
{
    return {std::move(text), {{"left", {DecisionValue::Accepted, 1}},
                             {"right", {DecisionValue::Ignored, {}}}}, 0};
}
FileSnapshots capturedSources()
{
    FileSnapshots captured;
    captured.path = ":literal\tfile.txt";
    captured.provenance = {"session", "/worktree", "/git/worktrees/session",
        SnapshotProvenance::Operation::Rebase, std::string(64, '2'), std::string(64, '3'),
        std::string(64, '1'), "refs/heads/topic", "original-head", "onto"};
    captured.conversion.attributes = {{"text", "set"}, {"eol", "crlf"}};
    captured.conversion.configuration = {{"core.autocrlf", "false"}, {"core.eol", "crlf"}};
    captured.resultMode = "100755";
    captured.earlier = SourceSnapshot{"base-blob", "100644", "base\n",
        {"base\n", "base\r\n", {SnapshotFormat::Encoding::Utf8, false, SnapshotFormat::Newline::CrLf, true}},
        "base-commit", "Common earlier version"};
    captured.stage2 = SourceSnapshot{"left-blob", "100755", "main\n",
        {"main\n", "\xef\xbb\xbfmain\r\n", {SnapshotFormat::Encoding::Utf8, true, SnapshotFormat::Newline::CrLf, true}},
        "left-commit", "Current rebased state"};
    captured.stage3 = SourceSnapshot{"right-blob", "100755", "side\n",
        {"side\n", std::string("\0s\0i\0d\0e\0\r\0\n", 12),
            {SnapshotFormat::Encoding::Utf16BigEndian, false, SnapshotFormat::Newline::CrLf, true}},
        "right-commit", "Commit being replayed"};
    captured.launchResult = {{"manual\nkept", "\xef\xbb\xbfmanual\r\nkept",
        {SnapshotFormat::Encoding::Utf8, true, SnapshotFormat::Newline::CrLf, false}},
        {11, 22, 0100755, 15, 33, 44, 55, 66}};
    return captured;
}
bool has(const FileSession &file, Blocker blocker)
{
    const auto blockers = file.facts().blockers;
    return std::find(blockers.begin(), blockers.end(), blocker) != blockers.end();
}
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
}

class SessionStateTests {
public:
    int run()
    {
        test_support::Suite suite("SessionStateTests");
        suite.run("source captures survive edits history checkpoints and staged discards", [] {
            auto input = capturedSources();
            const auto expected = input;
            FileSession file("id", input.path, draft(input.launchResult.text.utf8),
                             Support::Supported, {}, input);
            const auto *capture = file.snapshots();
            const auto unchanged = [&] {
                CHECK(capture != nullptr && file.snapshots() == capture);
                CHECK(*file.snapshots() == expected);
            };
            unchanged();
            input.stage2->rawBytes = "changed caller-owned source";
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
        });
        suite.run("an unavailable earlier source remains distinct from a captured empty source", [] {
            for (const bool missing : {false, true}) {
                auto captured = capturedSources();
                if (missing) captured.earlier.reset();
                else {
                    captured.earlier->rawBytes.clear();
                    captured.earlier->worktree = {};
                }
                const auto expected = captured;
                FileSession file("id", captured.path, draft(captured.launchResult.text.utf8),
                                 Support::Supported, {}, captured);
                CHECK(file.snapshots() && file.snapshots()->earlier.has_value() == !missing);
                if (!missing) {
                    CHECK(file.snapshots()->earlier->rawBytes.empty());
                    CHECK(file.snapshots()->earlier->worktree.utf8.empty());
                    CHECK(file.snapshots()->earlier->worktree.bytes.empty());
                }
                CHECK(file.replaceDraft(file.current().id, draft("edited")) == Transition::Applied);
                CHECK(file.undo(file.current().id) == Transition::Applied);
                CHECK(file.redo(file.current().id) == Transition::Applied);
                CHECK(*file.snapshots() == expected);
            }
            FileSession uncaptured("id", "outside", {}, Support::OutsideApp, "Missing source");
            CHECK(uncaptured.snapshots() == nullptr);
        });
        suite.run("source captures reject a different path or launch result", [] {
            const auto captured = capturedSources();
            for (const bool wrongPath : {false, true}) {
                bool rejected = false;
                try {
                    FileSession invalid("id", wrongPath ? "another path" : captured.path,
                        draft(wrongPath ? captured.launchResult.text.utf8 : "regenerated source"),
                        Support::Supported, {}, captured);
                } catch (const std::invalid_argument &) { rejected = true; }
                CHECK(rejected);
            }
        });
        suite.run("identity and initial blockers", [] {
            for (const bool emptyId : {false, true}) {
                bool rejected = false;
                try { FileSession invalid(emptyId ? "" : "id", emptyId ? "path" : "", draft()); }
                catch (const std::invalid_argument &) { rejected = true; }
                CHECK(rejected);
            }
            FileSession file("id", ":literal", draft());
            CHECK(file.id() == "id" && file.path() == ":literal");
            CHECK(file.current().id == 1 && file.current().result == 1 && file.current().decisions == 1);
            CHECK(file.facts().unsaved && file.facts().needsReview && !file.facts().resolved);
            CHECK(has(file, Blocker::UnsavedDraft) && has(file, Blocker::ReviewNeeded));
            CHECK(has(file, Blocker::WriteRequired) && has(file, Blocker::StageRequired));
            CHECK(file.undo(1) == Transition::NoChange && file.redo(1) == Transition::NoChange);
        });
        suite.run("transaction dimensions and stale editing commands", [] {
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
            decisions.unresolvedMarkers = 1;
            CHECK(file.replaceDraft(current, decisions) == Transition::Applied);
            CHECK(file.current().result == textRevision.result);
            CHECK(file.current().decisions == file.current().id);
        });
        suite.run("Save checkpoints only and keeps command history", [] {
            FileSession file("id", "file", draft());
            CHECK(file.replaceDraft(1, draft("saved")) == Transition::Applied);
            const auto saved = file.current();
            CHECK(file.recordCheckpoint(saved.id, Outcome::Succeeded) == Transition::Applied);
            CHECK(!file.facts().unsaved && file.facts().needsReview && !file.facts().resolved);
            CHECK(!file.reviewed() && !file.written() && !file.staged());
            CHECK(file.facts().status == FileStatus::SavedDraft && file.canUndo());
            CHECK(file.replaceDraft(saved.id, draft("newer")) == Transition::Applied);
            CHECK(file.undo(file.current().id) == Transition::Applied);
            CHECK(file.current().id > saved.id && file.current().draft == saved.draft);
            CHECK(file.current().result == saved.result && file.current().decisions == saved.decisions);
            CHECK(!file.facts().unsaved && file.facts().needsReview && file.canRedo());
            CHECK(file.recordCheckpoint(saved.id - 1, Outcome::Succeeded) == Transition::StaleRevision);
            CHECK(file.checkpoint()->id == saved.id);
        });
        suite.run("history restores decisions and cuts the redo branch", [] {
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
            CHECK(file.staged()->revision.draft == combined);
        });
        suite.run("confirmation, markers and support block publication", [] {
            auto undecided = draft();
            undecided.decisions["right"] = {};
            FileSession file("id", "file", undecided);
            CHECK(file.beginMarkResolved(1) == Transition::ConfirmationRequired);
            CHECK(file.current().draft == undecided && !file.reviewed());
            CHECK(file.beginMarkResolved(1, true) == Transition::Applied);
            CHECK(file.reviewed()->confirmedUndecided && !has(file, Blocker::Undecided));
            undecided.unresolvedMarkers = 1;
            CHECK(file.replaceDraft(1, undecided) == Transition::Applied);
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
            }
        });
        suite.run("outcomes require the reviewed saved and written transaction", [] {
            FileSession file("id", "file", draft());
            CHECK(file.recordWrite(1, Outcome::Succeeded) == Transition::StaleRevision);
            CHECK(file.recordStage(1, Outcome::Succeeded, "object", "100644") == Transition::StaleRevision);
            CHECK(file.recordCheckpoint(999, Outcome::Succeeded) == Transition::StaleRevision);
            CHECK(file.beginMarkResolved(1) == Transition::Applied);
            CHECK(file.recordWrite(1, Outcome::Succeeded) == Transition::InvalidOutcome);
            for (const auto outcome : {Outcome::Failed, Outcome::Uncertain}) {
                CHECK(file.recordCheckpoint(1, outcome, "save diagnostic") == Transition::Applied);
                CHECK(file.recordWrite(1, Outcome::Succeeded) == Transition::InvalidOutcome);
                CHECK(file.reviewed()->revision.id == 1 && !file.facts().resolved);
                CHECK(file.checkpointOutcome()->detail == "save diagnostic");
            }
            CHECK(file.recordCheckpoint(1, Outcome::Succeeded) == Transition::Applied);
            CHECK(file.recordStage(1, Outcome::Succeeded, "object", "100644") == Transition::InvalidOutcome);
            CHECK(file.recordWrite(1, Outcome::Failed, "write failed") == Transition::Applied);
            CHECK(file.recordStage(1, Outcome::Succeeded, "object", "100644") == Transition::InvalidOutcome);
            CHECK(file.recordWrite(1, Outcome::Succeeded) == Transition::Applied);
            CHECK(file.recordStage(1, Outcome::Succeeded, "", "100644") == Transition::InvalidOutcome);
            CHECK(file.recordStage(1, Outcome::Succeeded, "object", "") == Transition::InvalidOutcome);
            CHECK(!file.staged() && !file.facts().resolved);
            CHECK(file.recordStage(1, Outcome::Succeeded, "object", "100644") == Transition::Applied);
            CHECK(file.facts().resolved && file.facts().status == FileStatus::Resolved);
        });
        for (const auto outcome : {Outcome::Failed, Outcome::Uncertain}) {
            for (int step = 0; step != 3; ++step) {
                suite.run("failed or uncertain outcome retains snapshots " + std::to_string(step)
                          + '/' + std::to_string(static_cast<int>(outcome)), [=] {
                    FileSession file("id", "file", draft());
                    resolve(file);
                    const std::array failures{Blocker::CheckpointFailed, Blocker::WriteFailed, Blocker::StageFailed};
                    if (step == 0) CHECK(file.recordCheckpoint(1, outcome, "diagnostic") == Transition::Applied);
                    if (step == 1) CHECK(file.recordWrite(1, outcome, "diagnostic") == Transition::Applied);
                    if (step == 2) CHECK(file.recordStage(1, outcome, {}, {}, "diagnostic") == Transition::Applied);
                    CHECK(file.checkpoint()->id == 1 && file.written()->id == 1 && file.staged()->revision.id == 1);
                    CHECK(!file.facts().resolved);
                    CHECK(has(file, outcome == Outcome::Uncertain ? Blocker::UncertainOutcome
                                                                : failures[static_cast<std::size_t>(step)]));
                    const std::array statuses{FileStatus::CheckpointFailed, FileStatus::WriteFailed, FileStatus::StageFailed};
                    CHECK(file.facts().status == (outcome == Outcome::Uncertain ? FileStatus::OutcomeUncertain
                                                                : statuses[static_cast<std::size_t>(step)]));
                });
            }
        }
        suite.run("checkpoint discard fences delayed callbacks even after undo", [] {
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
            CHECK(file.checkpoint()->id == 1 && !file.facts().unsaved && file.facts().needsReview);
        });
        suite.run("discard without checkpoint restores initial draft and rejects delayed Save", [] {
            FileSession file("id", "file", draft());
            CHECK(file.discardToCheckpoint(1) == Transition::NoChange);
            CHECK(file.replaceDraft(1, draft("discarded")) == Transition::Applied);
            const auto discarded = file.current().id;
            CHECK(file.discardToCheckpoint(discarded) == Transition::Applied);
            CHECK(file.current().draft == draft() && !file.checkpoint());
            CHECK(file.recordCheckpoint(discarded, Outcome::Succeeded) == Transition::StaleRevision);
            CHECK(file.facts().unsaved && !file.facts().resolved);
        });
        suite.run("staged discard deliberately restores verified identity and fences newer callbacks", [] {
            FileSession file("id", "file", draft());
            CHECK(file.discardToStaged(1) == Transition::Blocked);
            resolve(file);
            CHECK(file.discardToStaged(1) == Transition::NoChange);
            CHECK(file.replaceDraft(1, draft("discarded")) == Transition::Applied);
            const auto discarded = file.current().id;
            CHECK(file.recordCheckpoint(discarded, Outcome::Succeeded) == Transition::Applied);
            CHECK(file.discardToStaged(discarded) == Transition::Applied);
            CHECK(file.current().id == 1 && file.checkpoint()->id == 1 && file.facts().resolved);
            CHECK(file.recordCheckpoint(discarded, Outcome::Failed) == Transition::StaleRevision);
            CHECK(file.recordWrite(discarded, Outcome::Succeeded) == Transition::StaleRevision);
            CHECK(file.recordStage(discarded, Outcome::Succeeded, "other", "100644") == Transition::StaleRevision);
            CHECK(file.facts().resolved);
        });
        for (const auto outcome : {Outcome::Succeeded, Outcome::Uncertain}) {
            suite.run("staged discard cannot pretend a newer write did not happen "
                      + std::to_string(static_cast<int>(outcome)), [=] {
                FileSession file("id", "file", draft());
                resolve(file);
                CHECK(file.replaceDraft(1, draft("new")) == Transition::Applied);
                const auto newer = file.current().id;
                CHECK(file.beginMarkResolved(newer) == Transition::Applied);
                CHECK(file.recordCheckpoint(newer, Outcome::Succeeded) == Transition::Applied);
                CHECK(file.recordWrite(newer, outcome) == Transition::Applied);
                CHECK(file.discardToStaged(newer) == Transition::Blocked);
                CHECK(file.current().id == newer && file.staged()->revision.id == 1);
            });
        }
        for (const auto outcome : {Outcome::Succeeded, Outcome::Failed, Outcome::Uncertain}) {
            for (const bool saveBeforeReview : {false, true}) {
                suite.run("TRUCE-0042 delayed latest-review write after newer Save "
                          + std::to_string(static_cast<int>(outcome)) + '/' + std::to_string(saveBeforeReview), [=] {
                    FileSession file("id", "file", draft());
                    if (saveBeforeReview) CHECK(file.recordCheckpoint(1, Outcome::Succeeded) == Transition::Applied);
                    CHECK(file.beginMarkResolved(1) == Transition::Applied);
                    if (!saveBeforeReview) CHECK(file.recordCheckpoint(1, Outcome::Succeeded) == Transition::Applied);
                    CHECK(file.replaceDraft(1, draft("new saved draft")) == Transition::Applied);
                    const auto newer = file.current().id;
                    CHECK(file.recordCheckpoint(newer, Outcome::Succeeded) == Transition::Applied);
                    CHECK(file.recordWrite(1, outcome, "delayed write diagnostic") == Transition::Applied);
                    CHECK(file.writeOutcome()->revision == 1 && file.writeOutcome()->outcome == outcome);
                    CHECK(file.writeOutcome()->detail == "delayed write diagnostic");
                    CHECK(file.checkpoint()->id == newer && file.current().id == newer);
                    CHECK(!file.facts().unsaved && file.facts().needsReview && !file.facts().resolved);
                    CHECK(file.reviewed()->revision.id == 1);
                    if (outcome == Outcome::Succeeded) {
                        CHECK(file.written()->id == 1 && file.written()->draft == draft());
                        CHECK(file.facts().status == FileStatus::WrittenUnresolved);
                        CHECK(file.recordStage(1, Outcome::Succeeded, "earlier", "100644") == Transition::Applied);
                        CHECK(file.facts().earlierStaged && !file.facts().resolved);
                    } else {
                        CHECK(!file.written());
                        CHECK(file.facts().status == (outcome == Outcome::Failed ? FileStatus::WriteFailed : FileStatus::OutcomeUncertain));
                    }
                });
            }
        }
        suite.run("a newer successful Save does not authorize an unsuccessful review checkpoint", [] {
            for (const auto outcome : {Outcome::Failed, Outcome::Uncertain}) {
                FileSession file("id", "file", draft());
                CHECK(file.beginMarkResolved(1) == Transition::Applied);
                CHECK(file.recordCheckpoint(1, outcome) == Transition::Applied);
                CHECK(file.replaceDraft(1, draft("newer")) == Transition::Applied);
                CHECK(file.recordCheckpoint(file.current().id, Outcome::Succeeded) == Transition::Applied);
                CHECK(file.recordWrite(1, Outcome::Succeeded) == Transition::InvalidOutcome);
                CHECK(!file.written() && !file.facts().resolved);
            }
        });
        suite.run("a newer review rejects superseded publication callbacks", [] {
            FileSession file("id", "file", draft());
            CHECK(file.beginMarkResolved(1) == Transition::Applied);
            CHECK(file.recordCheckpoint(1, Outcome::Succeeded) == Transition::Applied);
            CHECK(file.replaceDraft(1, draft("newer")) == Transition::Applied);
            const auto newer = file.current().id;
            CHECK(file.beginMarkResolved(newer) == Transition::Applied);
            CHECK(file.recordWrite(1, Outcome::Succeeded) == Transition::StaleRevision);
            CHECK(file.recordStage(1, Outcome::Succeeded, "earlier", "100644") == Transition::StaleRevision);
            CHECK(!file.written() && !file.staged() && file.reviewed()->revision.id == newer);
        });
        return suite.report();
    }
};

int main() { return SessionStateTests{}.run(); }
