#include "app/RecoveryRecord.h"
#include "app/RecoveryRestore.h"
#include "core/EditingCommands.h"
#include "TestHarness.h"
#include "fixtures/ChoiceFile.h"
#include "fixtures/RecoveryJournal.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
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

// tests/fixtures/recovery/underlined-base-7447467 holds a journal that the
// encoder and core of commit 7447467 wrote, read back and replayed. A writer
// built against that commit opened a diff3 conflict whose base underlines a
// heading with exactly seven '=' (the session of underlinedBaseFile), accepted
// the left section, deleted the base copy of the underline by hand and undid
// that edit, committing each step. That build took the underline for Git's
// separator and recorded marker counts 2, 2, 1 and 2, where this one counts 1.
QString copiedJournal(const QString &directory)
{
    const QString fixture = QStringLiteral(BRANCH_TRUCE_RECOVERY_FIXTURES "/underlined-base-7447467");
    const auto store = directory + QStringLiteral("/store");
    CHECK(QDir().mkpath(store + QStringLiteral("/payloads")));
    const auto owner = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    for (const auto *folder : {"", "/payloads"}) {
        CHECK(QFile::setPermissions(store + QLatin1String(folder), owner | QFileDevice::ExeOwner));
        for (const auto &name : QDir(fixture + QLatin1String(folder)).entryList(QDir::Files)) {
            const auto target = store + QLatin1String(folder) + QLatin1Char('/') + name;
            CHECK(QFile::copy(fixture + QLatin1String(folder) + QLatin1Char('/') + name, target));
            CHECK(QFile::setPermissions(target, owner));
        }
    }
    return store;
}

QJsonObject recordedDraft(const QJsonObject &state)
{
    return state.value("current").toObject().value("draft").toObject();
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
        CHECK(!current.correspondence->handEditedRanges().empty());
        CHECK(decoder.handEditedRanges(encodedCurrent.value("handEditedRanges"))
            == current.correspondence->handEditedRanges());
        CHECK(encodedCurrent.value("id").toString() == QString::number(current.id));

        const auto encodedCheckpoint = state.value("checkpoint").toObject();
        CHECK(decoder.draft(encodedCheckpoint.value("draft")) == file.checkpoint()->draft);
        CHECK(decoder.handEdited(encodedCheckpoint.value("handEdited"))
            == file.checkpoint()->correspondence->handEditedAnchors());
        CHECK(decoder.handEditedRanges(encodedCheckpoint.value("handEditedRanges"))
            == file.checkpoint()->correspondence->handEditedRanges());
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
    suite.run("TRUCE-0143 hand-edited ranges outside every region round-trip through a recovery record", [] {
        // "header" is a line both sides share, so no choice region covers it.
        auto file = test_support::choiceFile();
        CHECK(file.editResult(1, {{0, 6}, "HEADER"}) == Transition::Applied);
        const auto &current = file.current();
        CHECK(current.correspondence->handEditedAnchors().empty());
        CHECK((current.correspondence->handEditedRanges() == std::vector<ByteRange>{{0, 6}}));

        QTemporaryDir directory;
        CHECK(directory.isValid());
        storage::RecoveryStore store(directory.path() + "/store");
        const auto transactionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto state = committedState(store, recoveryTransaction(file, "replace-draft", transactionId));
        const auto encoded = state.value("current").toObject();
        CHECK(!encoded.contains("handEdited"));
        CHECK((encoded.value("handEditedRanges").toArray()
            == QJsonArray{QJsonObject{{"begin", "0"}, {"end", "6"}}}));
        const RecoveryRecordDecoder decoder(store);
        const auto ranges = decoder.handEditedRanges(encoded.value("handEditedRanges"));
        CHECK(ranges == current.correspondence->handEditedRanges());

        // Replay restores them, so the replayed revision encodes the same record.
        auto replayed = test_support::choiceFile();
        CHECK(replayed.replaceDraft(1, decoder.draft(encoded.value("draft")),
            decoder.handEdited(encoded.value("handEdited")), ranges) == Transition::Applied);
        CHECK(replayed.correspondence()->handEditedRanges() == ranges);
        CHECK(recoveryTransaction(replayed, "replace-draft", transactionId).state == state);

        // A revision without the field, as written before it existed, has none,
        // and a revision with none writes no field.
        const auto absent = encoded.value("missing");
        CHECK(absent.isUndefined() && decoder.handEditedRanges(absent).empty());
        auto older = test_support::choiceFile();
        CHECK(older.replaceDraft(1, decoder.draft(encoded.value("draft")), std::set<std::string>{},
            decoder.handEditedRanges(absent)) == Transition::Applied);
        CHECK(older.correspondence()->handEditedRanges().empty());
        CHECK(!recoveryTransaction(older, "replace-draft", transactionId).state.value("current").toObject()
            .contains("handEditedRanges"));

        // Malformed ranges are refused before replay.
        const auto refused = [&](const QJsonValue &value) {
            try {
                (void)decoder.handEditedRanges(value);
            } catch (const std::runtime_error &) {
                return true;
            }
            return false;
        };
        CHECK(refused(QJsonValue(QStringLiteral("0-6"))));
        CHECK(refused(QJsonArray{QStringLiteral("0")}));
        CHECK(refused(QJsonArray{QJsonObject{{"begin", "6"}, {"end", "0"}}}));
        CHECK(refused(QJsonArray{QJsonObject{{"begin", "00"}, {"end", "6"}}}));
        CHECK(refused(QJsonArray{QJsonObject{{"begin", 0}, {"end", 6}}}));
        CHECK(refused(QJsonArray{QJsonObject{{"begin", "0"}}}));
    });
    suite.run("TRUCE-0253 a journal written before the marker count fixes replays its text and decisions", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        const storage::RecoveryStore store(copiedJournal(directory.path()));
        const auto read = store.read();
        CHECK(read.succeeded && !read.incompleteTail && read.transactions.size() == 4);
        QStringList actions;
        QStringList counts;
        for (const auto &state : read.transactions) {
            actions.push_back(state.value("action").toString());
            counts.push_back(recordedDraft(state).value("unresolvedMarkers").toString());
        }
        CHECK((actions == QStringList{"open", "replace-draft", "replace-draft", "undo"}));
        CHECK((counts == QStringList{"2", "2", "1", "2"}));
        const RecoveryRecordDecoder decoder(store);
        const auto chosen = decoder.draft(recordedDraft(read.transactions[1]));
        const auto edited = decoder.draft(recordedDraft(read.transactions[2]));

        auto replay = replayRecovery(store);
        CHECK(replay.succeeded && replay.error.isEmpty() && replay.files.size() == 1 && replay.userWork);
        auto &file = replay.files.front();
        const auto launch = test_support::underlinedBaseFile();
        CHECK(file.snapshots() && *file.snapshots() == *launch.snapshots());
        // The undo restored the accepted left section over the launch text.
        CHECK(file.current().id == 4 && file.current().draft.text == launch.current().draft.text);
        CHECK(file.current().draft.text == chosen.text && file.current().draft.decisions == chosen.decisions);
        CHECK(std::any_of(chosen.decisions.begin(), chosen.decisions.end(),
            [](const auto &entry) { return entry.second.value == DecisionValue::Accepted; }));
        // This build counts the one block, not the underline as another marker.
        CHECK(file.current().draft.unresolvedMarkers == 1 && file.markers() == launch.markers());
        CHECK(file.redo(file.current().id) == Transition::Applied);
        CHECK(file.current().draft.text == edited.text && file.current().draft.decisions == edited.decisions);
        CHECK(edited.text.find("Title\n=======\nbody\n") == std::string::npos);
        CHECK(file.current().draft.unresolvedMarkers == 1);
        // That build recorded no hand-edited ranges, so the hand edit replays with none.
        for (const auto &state : read.transactions)
            CHECK(!state.value("current").toObject().contains("handEditedRanges"));
        CHECK(file.correspondence() && file.correspondence()->handEditedRanges().empty());
    });
    suite.run("TRUCE-0253 replay still rejects recorded text, decisions or derived values that cannot be right", [] {
        const auto rejected = [](const auto &mutate, const QString &error) {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            const auto store = copiedJournal(directory.path());
            test_support::rewriteLastRecoveryState(store, mutate);
            const auto replay = replayRecovery(storage::RecoveryStore(store));
            CHECK(!replay.succeeded && replay.files.empty() && replay.error.contains(error));
        };
        const auto withDraft = [](QJsonObject &state, const auto &change) {
            auto current = state.value("current").toObject();
            auto draft = current.value("draft").toObject();
            change(draft, current);
            current.insert("draft", draft);
            state.insert("current", current);
        };
        const QString mismatch = "does not match the acknowledged record";
        // Other text than the undo restores, here an existing payload.
        rejected([&](QJsonObject &state) {
            withDraft(state, [&](QJsonObject &draft, QJsonObject &) { draft.insert("text", state.value("path")); });
        }, mismatch);
        // Another value for one recorded decision.
        rejected([&](QJsonObject &state) {
            withDraft(state, [](QJsonObject &draft, QJsonObject &) {
                auto decisions = draft.value("decisions").toArray();
                auto entry = decisions.first().toObject();
                entry.insert("value", (entry.value("value").toInt() + 1) % 3);
                decisions.replace(0, entry);
                draft.insert("decisions", decisions);
            });
        }, mismatch);
        // Another revision identity.
        rejected([&](QJsonObject &state) {
            withDraft(state, [](QJsonObject &, QJsonObject &current) { current.insert("id", "5"); });
        }, mismatch);
        // Derived values keep their recorded format.
        rejected([&](QJsonObject &state) {
            withDraft(state, [](QJsonObject &draft, QJsonObject &) { draft.insert("unresolvedMarkers", "02"); });
        }, "Invalid recovery integer");
        rejected([&](QJsonObject &state) {
            withDraft(state, [](QJsonObject &draft, QJsonObject &) { draft.remove("unresolvedMarkers"); });
        }, "Invalid recovery integer");
        rejected([&](QJsonObject &state) {
            withDraft(state, [](QJsonObject &, QJsonObject &current) { current.insert("decisions", "5"); });
        }, "Invalid recovery decision revision");
    });
    return suite.report();
}
