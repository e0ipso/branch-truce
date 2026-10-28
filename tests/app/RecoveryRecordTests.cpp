#include "app/RecoveryRecord.h"
#include "core/EditingCommands.h"
#include "TestHarness.h"
#include "fixtures/ChoiceFile.h"
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>

using namespace branch_truce;

namespace {
// Every source slot filled, a rebase provenance, a conversion policy, a
// launch identity and a path that is not valid UTF-8.
FileSession capturedFile()
{
    auto captured = *test_support::choiceFile().snapshots();
    captured.path = std::string("choices\t") + static_cast<char>(0xff) + ".txt";
    const std::string base = "header\nbase first\nbase second\nbridge\nbase later\ntail\n";
    captured.earlier = SourceSnapshot{"base-object", "100644", base, test_support::choiceText(base), {}, "Common ancestor"};
    captured.conversion.attributes = {{"text", "set"}, {"eol", "lf"}};
    captured.conversion.configuration = {{"core.autocrlf", "false"}};
    captured.provenance.operation = SnapshotProvenance::Operation::Rebase;
    captured.provenance.baseCommitId = "base-commit";
    captured.provenance.headName = "refs/heads/topic";
    captured.provenance.originalHead = "original-head";
    captured.provenance.onto = "onto-commit";
    captured.launchResult.identity = {11, 22, 0100644, 64, 33, 44, 55, 66};
    const auto text = captured.launchResult.text.utf8;
    const auto path = captured.path;
    return FileSession("choices", path, {text, {}, 0}, Support::Supported, "captured at launch",
        std::move(captured), true);
}

void choose(FileSession &file, SourceSide side, ChoiceScope scope, std::size_t occurrence, ChoiceAction action)
{
    const auto target = test_support::choiceTarget(file, side, scope, occurrence).source.contributionId;
    CHECK(applyChoice(file, previewChoice(file, file.current().id, target, action), true) == Transition::Applied);
}

// Decisions with and without an order, typed text, a manual Save checkpoint
// and a later draft, so the current revision differs from the checkpoint.
FileSession editedFile()
{
    auto file = capturedFile();
    choose(file, SourceSide::Stage2, ChoiceScope::Line, 0, ChoiceAction::Accept);
    choose(file, SourceSide::Stage3, ChoiceScope::Line, 0, ChoiceAction::Ignore);
    choose(file, SourceSide::Stage2, ChoiceScope::Section, 1, ChoiceAction::CombineLeftFirst);
    const auto at = file.current().draft.text.find("left first") + 5;
    CHECK(file.editResult(file.current().id, {{at, at}, "hand "}) == Transition::Applied);
    CHECK(file.recordCheckpoint(file.current().id, Outcome::Succeeded, "saved") == Transition::Applied);
    const auto end = file.current().draft.text.size();
    CHECK(file.editResult(file.current().id, {{end, end}, "after save\n"}) == Transition::Applied);
    return file;
}

QJsonObject committedState(storage::RecoveryStore &store, const storage::RecoveryTransaction &transaction)
{
    CHECK(store.commit(transaction).succeeded);
    const auto read = store.read();
    CHECK(read.succeeded && !read.incompleteTail && read.transactions.size() == 1);
    return read.transactions.front();
}
}

int main()
{
    test_support::Suite suite("RecoveryRecordTests");
    suite.run("an edited file session decodes to the state it was encoded from", [] {
        const auto file = editedFile();
        const auto &current = file.current();
        CHECK(file.checkpoint() && file.checkpoint()->id != current.id && file.checkpointOutcome());
        CHECK(current.correspondence && !current.correspondence->handEditedAnchors().empty());
        CHECK(std::any_of(current.draft.decisions.begin(), current.draft.decisions.end(),
            [](const auto &entry) { return entry.second.order.has_value(); }));
        CHECK(std::any_of(current.draft.decisions.begin(), current.draft.decisions.end(),
            [](const auto &entry) { return entry.second.value == DecisionValue::Ignored; }));
        // Draft::unresolvedMarkers is not decoded; this file has no markers.
        CHECK(current.draft.unresolvedMarkers == 0 && file.checkpoint()->draft.unresolvedMarkers == 0);

        QTemporaryDir directory;
        CHECK(directory.isValid());
        storage::RecoveryStore store(directory.path() + "/store");
        const auto transactionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto transaction = recoveryTransaction(file, "manual-save", transactionId);
        const auto state = committedState(store, transaction);
        CHECK(state == transaction.state);
        CHECK(state.value("schema").toInt() == 1 && state.value("transactionId").toString() == transactionId
            && state.value("action").toString() == "manual-save");

        const RecoveryRecordDecoder decoder(store);
        CHECK(decoder.bytes(state.value("fileId")) == file.id());
        CHECK(decoder.bytes(state.value("path")) == file.path());
        CHECK(decoder.enumeration<Support>(state.value("support"), 2) == file.support());
        CHECK(decoder.bytes(state.value("supportDetail")) == file.supportDetail());
        const auto snapshots = decoder.snapshots(state.value("snapshots"));
        CHECK(snapshots && *snapshots == *file.snapshots());
        CHECK(snapshots->earlier && !snapshots->earlier->commitId && snapshots->stage2->commitId);

        const auto encodedCurrent = state.value("current").toObject();
        CHECK(decoder.draft(encodedCurrent.value("draft")) == current.draft);
        CHECK(decoder.handEdited(encodedCurrent.value("handEdited")) == current.correspondence->handEditedAnchors());
        CHECK(encodedCurrent.value("id").toString() == QString::number(current.id));

        const auto encodedCheckpoint = state.value("checkpoint").toObject();
        CHECK(decoder.draft(encodedCheckpoint.value("draft")) == file.checkpoint()->draft);
        CHECK(decoder.handEdited(encodedCheckpoint.value("handEdited"))
            == file.checkpoint()->correspondence->handEditedAnchors());
        CHECK(encodedCheckpoint.value("id").toString() == QString::number(file.checkpoint()->id));
        const auto checkpointOutcome = decoder.outcome(state.value("checkpointOutcome"));
        CHECK(checkpointOutcome.revision == file.checkpointOutcome()->revision
            && checkpointOutcome.outcome == file.checkpointOutcome()->outcome
            && checkpointOutcome.detail == file.checkpointOutcome()->detail);
        for (const auto *absent : {"written", "reviewed", "staged", "writeOutcome", "stageOutcome"})
            CHECK(state.value(absent).isNull());

        // Encoding the same file again gives the same state and payloads.
        const auto again = recoveryTransaction(file, "manual-save", transactionId);
        CHECK(again.state == transaction.state && again.payloads == transaction.payloads);
    });
    return suite.report();
}
