// Import validates supported drafts before rebuilding and preserves publication eligibility.
#include "TestHarness.h"
#include "core/SessionCheckpoint.h"
#include "fixtures/ChoiceFile.h"
#include "recovery/RecoveryRecord.h"
#include <QCoreApplication>
#include <QTemporaryDir>

using namespace branch_truce;
namespace {
std::vector<RecordedRevision> recorded(SessionCheckpoint &checkpoint)
{
    std::vector<RecordedRevision> result;
    result.reserve(checkpoint.history.revisions.size());
    for (const auto &revision : checkpoint.history.revisions)
        result.push_back(
            {revision->id, revision->result, revision->draft,
             revision->correspondence ? revision->correspondence->handEditedAnchors() : std::set<std::string>{},
             revision->correspondence ? revision->correspondence->handEditedRanges() : std::vector<ByteRange>{},
             revision->decisions});
    checkpoint.history.revisions.clear();
    return result;
}
void refused(FileSession &file, SessionCheckpoint checkpoint, std::vector<RecordedRevision> revisions)
{
    const auto before = recoveryFileCheckpoint(file).state;
    bool rejected = false;
    try {
        std::move(checkpoint).restore(file, std::move(revisions));
    } catch (const std::exception &) {
        rejected = true;
    }
    CHECK(rejected && recoveryFileCheckpoint(file).state == before);
}
void roundtrip(const FileSession &file)
{
    QTemporaryDir directory;
    storage::RecoveryStore store(directory.path());
    const auto encoded = recoveryFileCheckpoint(file);
    auto restored = RecoveryRecordReader(store, encoded.state, encoded.retainedBytes, &encoded.payloads)
                        .checkpointFile(file.algorithm());
    CHECK(recoveryFileCheckpoint(restored).state == encoded.state);
    CHECK(restored.facts().blockers == file.facts().blockers && restored.facts().status == file.facts().status);
    CHECK(restored.facts().unsaved == file.facts().unsaved && restored.facts().resolved == file.facts().resolved);
    for (const auto &revision : SessionCheckpoint::capture(file).history.revisions) {
        auto before = file, after = restored;
        CHECK(before.recordCheckpoint(revision->id, Outcome::Failed)
              == after.recordCheckpoint(revision->id, Outcome::Failed));
        before = file;
        after = restored;
        CHECK(before.recordWrite(revision->id, Outcome::Failed) == after.recordWrite(revision->id, Outcome::Failed));
        before = file;
        after = restored;
        CHECK(before.recordStage(revision->id, Outcome::Failed) == after.recordStage(revision->id, Outcome::Failed));
    }
}
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    test_support::configure(argc, argv);
    test_support::Suite suite("SessionCheckpointTests");
    suite.run("every retained supported no-map draft obeys the ordinary text envelope", [] {
        FileSession file("plain", "plain.txt", {"launch\n", {}, 0});
        CHECK(file.editResult(file.current().id, {{0, 0}, "edit\n"}) == Transition::Applied);
        for (const auto &text : {std::string("bad\0text", 8), std::string("bad\r\n"), std::string(1, char(0xff)),
                                 std::string(kMaxLineBytes + 1, 'x'), std::string(kMaxIndexedLines + 1, '\n'),
                                 (std::string(131071, 'x') + '\n')}) {
            auto checkpoint = SessionCheckpoint::capture(file);
            auto revisions = recorded(checkpoint);
            revisions.front().draft.text = text;
            if (text.size() == 131072) {
                revisions.front().draft.text.clear();
                for (int i = 0; i < 65; ++i) revisions.front().draft.text += text;
            }
            refused(file, std::move(checkpoint), std::move(revisions));
        }
        std::string payload;
        for (int i = 0; i < 64; ++i) payload += std::string(131071, 'x') + '\n';
        for (const auto &text :
             {std::string(kMaxLineBytes - 1, 'x') + '\n', std::string(kMaxIndexedLines, '\n'), payload}) {
            auto checkpoint = SessionCheckpoint::capture(file);
            auto revisions = recorded(checkpoint);
            revisions.front().draft.text = text;
            auto restored = file;
            std::move(checkpoint).restore(restored, std::move(revisions));
            CHECK(restored.revision(1)->draft.text == text && restored.current().draft == file.current().draft);
        }
        FileSession outside("outside", "outside.txt", {std::string(kMaxLineBytes + 1, 'x'), {}, 0},
                            {.support = Support::OutsideApp});
        roundtrip(outside);
    });
    suite.run("an older Save cannot authorize a later review during import", [] {
        FileSession file("plain", "plain.txt", {"launch\n", {}, 0});
        CHECK(file.recordCheckpoint(1, Outcome::Succeeded) == Transition::Applied);
        CHECK(file.editResult(1, {{0, 0}, "later\n"}) == Transition::Applied);
        CHECK(file.beginMarkResolved(2) == Transition::Applied);
        roundtrip(file);
        CHECK(file.recordWrite(2, Outcome::Succeeded) == Transition::InvalidOutcome);
        auto checkpoint = SessionCheckpoint::capture(file);
        auto revisions = recorded(checkpoint);
        checkpoint.publication.reviewSaved = true;
        refused(file, std::move(checkpoint), std::move(revisions));
    });
    suite.run("Save and review order, failed outcomes, discard fences and later Saves survive import", [] {
        for (const bool saveFirst : {false, true}) {
            FileSession file("plain", "plain.txt", {"launch\n", {}, 0});
            CHECK(file.editResult(1, {{0, 0}, "edit\n"}) == Transition::Applied);
            if (saveFirst) CHECK(file.recordCheckpoint(2, Outcome::Succeeded) == Transition::Applied);
            CHECK(file.beginMarkResolved(2) == Transition::Applied);
            roundtrip(file);
            CHECK(file.recordCheckpoint(2, Outcome::Succeeded) == Transition::Applied);
            CHECK(file.recordWrite(2, Outcome::Succeeded) == Transition::Applied);
            CHECK(file.recordStage(2, Outcome::Succeeded, "object", "100644") == Transition::Applied);
            CHECK(file.recordCheckpoint(2, Outcome::Failed, "save failed") == Transition::Applied);
            CHECK(file.discardToCheckpoint(2) == Transition::Applied);
            roundtrip(file); // The review's failed Save evidence stays failed after discard resets the latest outcome.
            CHECK(file.editResult(file.current().id, {{0, 0}, "later\n"}) == Transition::Applied);
            CHECK(file.recordCheckpoint(file.current().id, Outcome::Succeeded) == Transition::Applied);
            roundtrip(file);
        }
        {
            FileSession file("plain", "plain.txt", {"launch\n", {}, 0});
            CHECK(file.beginMarkResolved(1) == Transition::Applied);
            CHECK(file.recordCheckpoint(1, Outcome::Succeeded) == Transition::Applied);
            CHECK(file.editResult(1, {{0, 0}, "later\n"}) == Transition::Applied);
            CHECK(file.recordCheckpoint(2, Outcome::Succeeded) == Transition::Applied);
            roundtrip(file); // Saving a later draft retains the earlier review's successful Save gate.
            CHECK(file.recordWrite(1, Outcome::Uncertain, "old reviewed write") == Transition::Applied);
            roundtrip(file);
        }
        for (const auto outcome : {Outcome::Failed, Outcome::Uncertain}) {
            auto file = test_support::choiceFile();
            CHECK(file.beginMarkResolved(1, true) == Transition::Applied);
            CHECK(file.recordCheckpoint(1, Outcome::Succeeded) == Transition::Applied);
            CHECK(file.recordWrite(1, outcome, "write detail") == Transition::Applied);
            roundtrip(file);
            CHECK(file.recordWrite(1, Outcome::Succeeded) == Transition::Applied);
            CHECK(file.recordStage(1, outcome, {}, {}, "stage detail") == Transition::Applied);
            CHECK(file.editResult(1, {{0, 0}, "hand edit\n"}) == Transition::Applied);
            CHECK(file.setSupport(Support::OutsideApp, "held externally") == Transition::Applied);
            roundtrip(file);
            auto checkpoint = SessionCheckpoint::capture(file);
            auto revisions = recorded(checkpoint);
            checkpoint.publication.reviewed->revision = checkpoint.history.next;
            refused(file, std::move(checkpoint), std::move(revisions));
        }
    });
    return suite.report();
}
