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
