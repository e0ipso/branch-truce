#include "TestHarness.h"
#include "TestSession.h"
#include "app/SelectedFile.h"
#include "app/Selection.h"
#include "app/SessionModel.h"
#include "app/Translations.h"
#include "fixtures/ApplicationIdentity.h"
#include "fixtures/ChoiceFile.h"
#include "fixtures/RecoveryJournal.h"
#include "fixtures/SyncFaults.h"
#include "recovery/RecoveryRestore.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QMap>
#include <QPersistentModelIndex>
#include <QProcess>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>
#include <cstdlib>
#include <limits>

using namespace branch_truce;
using test_support::TestSession;

namespace {
struct NoSelectionGuard : SelectionGuard {
    [[nodiscard]] bool holdsSelection() const override { return false; }
};
constexpr auto recoverySessionId = "42c4588c-5cc2-4fba-964f-3e98a066e671";

Draft draft(std::string text = "result\n") { return {std::move(text), {{"left", {DecisionValue::Accepted, 1}}}, 0}; }
QVariant role(const TestSession &model, int value, int row = 0) { return model.data(model.index(row), value); }
void resolve(TestSession &model, const std::string &id)
{
    const auto revision = model.file(id)->current().id;
    CHECK(model.beginMarkResolved(id, revision) == Transition::Applied);
    CHECK(model.recordCheckpoint(id, revision, Outcome::Succeeded) == Transition::Applied);
    CHECK(model.recordWrite(id, revision, Outcome::Succeeded) == Transition::Applied);
    CHECK(model.recordStage(id, revision, Outcome::Succeeded, "verified-object", "100644") == Transition::Applied);
}
FileSession recoverableFile()
{
    FileSnapshots captured;
    captured.path = std::string(":literal\t") + static_cast<char>(0xff);
    captured.resultMode = "100644";
    captured.provenance.sessionId = recoverySessionId;
    captured.provenance.worktreeRoot = "/disposable-worktree";
    captured.provenance.gitDirectory = "/disposable-worktree/.git";
    captured.provenance.stage2CommitId = "left-commit";
    captured.provenance.stage3CommitId = "right-commit";
    captured.conversion.attributes = {{"text", "set"}, {"eol", "crlf"}};
    captured.stage2 = SourceSnapshot{
        "left-object",
        "100644",
        "left\n",
        {"left\n", "left\r\n", {SnapshotFormat::Encoding::Utf8, false, SnapshotFormat::Newline::CrLf, true}},
        "left-commit",
        "Current checkout"};
    captured.stage3 = SourceSnapshot{
        "right-object",
        "100644",
        "right\n",
        {"right\n", "right\n", {SnapshotFormat::Encoding::Utf8, false, SnapshotFormat::Newline::Lf, true}},
        "right-commit",
        "Merged commit"};
    captured.launchResult.text = {"manual launch\n",
                                  "\xef\xbb\xbfmanual launch\r\n",
                                  {SnapshotFormat::Encoding::Utf8, true, SnapshotFormat::Newline::CrLf, true}};
    captured.launchResult.identity = {11, 22, 0100644, 18, 33, 44, 55, 66};
    return FileSession("recoverable", captured.path, draft(captured.launchResult.text.utf8), Support::Supported, {},
                       captured);
}
// A supported file of the recoverable session under another identity.
FileSession sessionFile(const std::string &id, Draft value = draft())
{
    auto captured = *recoverableFile().snapshots();
    captured.path = id + ".txt";
    captured.launchResult.text = test_support::choiceText(value.text);
    return FileSession(id, captured.path, std::move(value), Support::Supported, {}, captured);
}
// fileId -> handoff entry; every file must appear in exactly one group.
QMap<QString, QVariantMap> handoffEntries(const QVariantMap &summary)
{
    QMap<QString, QVariantMap> result;
    for (const auto &group : summary.value("groups").toList()) {
        const auto category = group.toMap().value("category").toString();
        for (const auto &value : group.toMap().value("entries").toList()) {
            const auto entry = value.toMap();
            CHECK(entry.value("category").toString() == category);
            CHECK(!result.contains(entry.value("fileId").toString()));
            result.insert(entry.value("fileId").toString(), entry);
        }
    }
    return result;
}
QByteArray payload(const storage::RecoveryStore &store, const QJsonValue &reference)
{
    QString error;
    const auto bytes = store.payload(reference.toString().toLatin1(), &error);
    CHECK(error.isEmpty());
    return bytes;
}
// A draft a record holds: its text and decisions. Records do not hold the marker count.
void checkDraft(const Draft &recorded, const Draft &expected)
{
    CHECK(recorded.text == expected.text);
    CHECK(recorded.decisions == expected.decisions);
}
Draft savedDraft()
{
    auto value = draft("saved combined \xc3\xa9\n");
    value.decisions["right"] = {DecisionValue::Ignored, {}};
    return value;
}
Draft unsavedDraft()
{
    auto value = savedDraft();
    value.text += "<<<<<<< ours\nacknowledged unsaved \xf0\x9f\x98\x80\n=======\nother\n>>>>>>> theirs\n";
    value.decisions["right"] = {DecisionValue::Accepted, 2};
    value.unresolvedMarkers = 1;
    return value;
}
Draft finalDecision()
{
    auto value = unsavedDraft();
    value.decisions["later contribution"] = {DecisionValue::Ignored, {}};
    return value;
}
int acknowledgeAndCrash(const QString &directory)
{
    TestSession model(directory);
    CHECK(model.appendFile(recoverableFile()));
    CHECK(model.replaceDraft("recoverable", 1, savedDraft()) == Transition::Applied);
    CHECK(model.recordCheckpoint("recoverable", 2, Outcome::Succeeded) == Transition::Applied);
    CHECK(model.replaceDraft("recoverable", 2, unsavedDraft()) == Transition::Applied);
    CHECK(model.replaceDraft("recoverable", 3, finalDecision()) == Transition::Applied);
    CHECK(!model.hasPendingRecovery() && model.file("recoverable")->current().id == 4);
    // Exit immediately after the acknowledgment without destructors or Qt cleanup.
    CHECK(::write(STDOUT_FILENO, "ACK 4\n", 6) == 6);
    std::_Exit(0);
}
FileSession replayedFile(const QString &directory)
{
    const auto replay = replayRecovery(storage::RecoveryStore(directory));
    if (!replay.succeeded) throw std::runtime_error("recovery replay: " + replay.error.toStdString());
    CHECK(replay.succeeded && replay.error.isEmpty() && replay.files.size() == 1);
    return replay.files.front();
}
void checkRecoveredHistory(TestSession &model, const Draft &before, const Draft &chosen)
{
    const auto captures = *model.file("choices")->snapshots();
    const auto check = [&](const Draft &expected) {
        const auto replay = replayedFile(model.recoveryDirectory());
        CHECK(replay.current().draft == expected && replay.current().id == model.file("choices")->current().id);
        CHECK(*replay.snapshots() == captures && replay.checkpoint() && replay.checkpoint()->draft == before);
        CHECK(!replay.reviewed() && !replay.written() && !replay.staged() && !replay.facts().resolved);
        CHECK(!model.hasPendingRecovery());
    };
    check(chosen);
    CHECK(model.undo("choices", model.file("choices")->current().id) == Transition::Applied);
    check(before);
    CHECK(model.redo("choices", model.file("choices")->current().id) == Transition::Applied);
    check(chosen);
    auto reopened = replayedFile(model.recoveryDirectory());
    CHECK(reopened.undo(reopened.current().id) == Transition::Applied && reopened.current().draft == before);
    CHECK(reopened.redo(reopened.current().id) == Transition::Applied && reopened.current().draft == chosen);
}

QString recordIntent(TestSession &model, const std::string &id, MutationKind kind)
{
    const auto mutation = QUuid::createUuid().toString(QUuid::WithoutBraces);
    CHECK(model.commitRecord(mutationRecoveryTransaction(*model.file(id), {mutation, kind, {}})).succeeded);
    return mutation;
}

void preparePublication(TestSession &model, const std::string &id, bool staging)
{
    const auto revision = model.file(id)->current().id;
    CHECK(model.beginMarkResolved(id, revision, true) == Transition::Applied);
    CHECK(model.recordCheckpoint(id, revision, Outcome::Succeeded) == Transition::Applied);
    if (staging) CHECK(model.recordWrite(id, revision, Outcome::Succeeded) == Transition::Applied);
}

void completePublication(TestSession &model, const std::string &id, bool staging)
{
    const auto revision = model.file(id)->current().id;
    CHECK((staging ? model.recordStage(id, revision, Outcome::Succeeded, "verified-object", "100644")
                   : model.recordWrite(id, revision, Outcome::Succeeded))
          == Transition::Applied);
}
}

class SessionModelTests {
public:
    int run()
    {
        test_support::Suite suite("SessionModelTests");
        suite.run("comparison bindings receive one complete projection per revision", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            TestSession model(directory.path() + "/recovery");
            CHECK(model.appendFile(test_support::choiceFile()));
            const NoSelectionGuard guard;
            Selection selection(model.rows, guard);
            SelectedFile selected(model.rows, selection);
            CHECK(selection.select(QStringLiteral("choices")));
            QQmlEngine engine;
            engine.rootContext()->setContextProperty(QStringLiteral("selected"), &selected);
            QQmlExpression binding(
                engine.rootContext(), nullptr,
                QStringLiteral("JSON.stringify({passages:selected.passages, regions:selected.regions, "
                               "handEditedLines:selected.handEditedLines, choices:selected.choices, "
                               "markerBlocks:selected.markerBlocks, markerOrphans:selected.markerOrphans, "
                               "choiceNotice:selected.choiceNotice})"));
            binding.setNotifyOnValueChanged(true);
            const auto read = [&] {
                const auto value = binding.evaluate().toString();
                CHECK(!binding.hasError());
                return value;
            };
            const auto initial = read();
            QStringList observed, focused;
            QObject::connect(&binding, &QQmlExpression::valueChanged, [&] { observed.push_back(read()); });
            const auto observeFocused = [&] { focused.push_back(read()); };
            QObject::connect(&selected, &SelectedFile::passagesChanged, observeFocused);
            QObject::connect(&selected, &SelectedFile::regionsChanged, observeFocused);
            QObject::connect(&selected, &SelectedFile::handEditedLinesChanged, observeFocused);
            const auto checkPublication = [&] {
                const auto complete = read();
                CHECK(observed.size() == 1);
                CHECK(observed.front() == complete);
                CHECK(focused.size() >= 2);
                for (const auto &value : focused) CHECK(value == complete);
                observed.clear();
                focused.clear();
                return complete;
            };
            CHECK(model.editResult("choices", model.file("choices")->current().id, {{0, 0}, "manual prefix\n"})
                  == Transition::Applied);
            selected.sync();
            CHECK(checkPublication() != initial);
            // Status-only refreshes must not recompute the same QML projection.
            selected.sync();
            CHECK(observed.isEmpty() && focused.isEmpty());
            CHECK(model.undo("choices", model.file("choices")->current().id) == Transition::Applied);
            selected.sync();
            CHECK(checkPublication() == initial);
            CHECK(model.redo("choices", model.file("choices")->current().id) == Transition::Applied);
            selected.sync();
            CHECK(checkPublication() != initial);
            // Without files nothing is selected and the projection empties.
            model.replaceFiles({});
            checkPublication();
            CHECK(selected.passages().isEmpty() && selected.regions().isEmpty() && selected.handEditedLines().isEmpty()
                  && selected.choices().isEmpty());
        });
        for (const auto scope : {ChoiceScope::Line, ChoiceScope::Section}) {
            for (const auto action : {ChoiceAction::CombineLeftFirst, ChoiceAction::CombineRightFirst}) {
                suite.run(
                    "ordered combination undo and redo recover as complete transactions "
                        + std::to_string(static_cast<int>(scope)) + "/" + std::to_string(static_cast<int>(action)),
                    [=] {
                        QTemporaryDir directory;
                        CHECK(directory.isValid());
                        TestSession model(directory.path() + "/recovery");
                        CHECK(model.appendFile(test_support::combinationFile(scope)));
                        CHECK(model.editResult("choices", 1, {{0, 0}, "manual prefix\n"}) == Transition::Applied);
                        CHECK(model.recordCheckpoint("choices", 2, Outcome::Succeeded) == Transition::Applied);
                        const auto before = model.file("choices")->current().draft;
                        const auto target =
                            test_support::choiceTarget(*model.file("choices"), SourceSide::Stage2, scope);
                        const auto preview =
                            previewChoice(*model.file("choices"), 2, target.source.contributionId, action);
                        const auto count = storage::RecoveryStore(model.recoveryDirectory()).read().transactions.size();
                        CHECK(model.applyChoice("choices", preview) == Transition::ConfirmationRequired);
                        CHECK(storage::RecoveryStore(model.recoveryDirectory()).read().transactions.size() == count);
                        CHECK(model.applyChoice("choices", preview, true) == Transition::Applied);
                        CHECK(storage::RecoveryStore(model.recoveryDirectory()).read().transactions.size()
                              == count + 1);
                        const auto chosen = model.file("choices")->current().draft;
                        CHECK(chosen != before && chosen.text.find(preview.replacement) != std::string::npos);
                        checkRecoveredHistory(model, before, chosen);
                    });
            }
        }
        for (const auto scope : {NonConflictingScope::Left, NonConflictingScope::Right, NonConflictingScope::Both}) {
            suite.run(
                "file batch undo and redo recover together with protected manual work "
                    + std::to_string(static_cast<int>(scope)),
                [=] {
                    QTemporaryDir directory;
                    CHECK(directory.isValid());
                    TestSession model(directory.path() + "/recovery");
                    CHECK(model.appendFile(test_support::batchFile()));
                    CHECK(model.editResult("choices", 1, {{0, 0}, "manual prefix\n"}) == Transition::Applied);
                    CHECK(model.recordCheckpoint("choices", 2, Outcome::Succeeded) == Transition::Applied);
                    const auto before = model.file("choices")->current().draft;
                    const auto preview = previewNonConflicting(*model.file("choices"), 2, scope);
                    const auto count = storage::RecoveryStore(model.recoveryDirectory()).read().transactions.size();
                    CHECK(model.applyNonConflicting("choices", preview) == Transition::ConfirmationRequired);
                    CHECK(storage::RecoveryStore(model.recoveryDirectory()).read().transactions.size() == count);
                    CHECK(model.applyNonConflicting("choices", preview, true) == Transition::Applied);
                    CHECK(storage::RecoveryStore(model.recoveryDirectory()).read().transactions.size() == count + 1);
                    const auto chosen = model.file("choices")->current().draft;
                    CHECK(chosen.text.starts_with("manual prefix\n") && chosen != before);
                    checkRecoveredHistory(model, before, chosen);
                });
        }
        for (const auto side : {SourceSide::Stage2, SourceSide::Stage3}) {
            suite.run(
                "whole-file replacement undo and redo recover text and decisions "
                    + std::to_string(static_cast<int>(side)),
                [=] {
                    QTemporaryDir directory;
                    CHECK(directory.isValid());
                    TestSession model(directory.path() + "/recovery");
                    CHECK(model.appendFile(test_support::choiceFile()));
                    CHECK(model.editResult("choices", 1, {{0, 0}, "manual prefix\n"}) == Transition::Applied);
                    CHECK(model.recordCheckpoint("choices", 2, Outcome::Succeeded) == Transition::Applied);
                    const auto before = model.file("choices")->current().draft;
                    const auto preview = previewWholeFile(*model.file("choices"), 2, side);
                    const auto count = storage::RecoveryStore(model.recoveryDirectory()).read().transactions.size();
                    CHECK(model.applyWholeFile("choices", preview) == Transition::ConfirmationRequired);
                    CHECK(storage::RecoveryStore(model.recoveryDirectory()).read().transactions.size() == count);
                    CHECK(model.applyWholeFile("choices", preview, true) == Transition::Applied);
                    CHECK(storage::RecoveryStore(model.recoveryDirectory()).read().transactions.size() == count + 1);
                    const auto chosen = model.file("choices")->current().draft;
                    CHECK(chosen.text == preview.replacement && chosen != before);
                    checkRecoveredHistory(model, before, chosen);
                });
        }
        for (const int operation : {0, 1, 2}) {
            suite.run(
                "wave 9 failed compound command retains one atomic recovery candidate " + std::to_string(operation),
                [=] {
                    QTemporaryDir directory;
                    CHECK(directory.isValid());
                    const auto faults = std::make_shared<test_support::SyncFaults>();
                    TestSession model(directory.path() + "/recovery", faults);
                    CHECK(model.appendFile(operation == 1 ? test_support::batchFile() : test_support::choiceFile()));
                    const auto before = model.file("choices")->current().draft;
                    const auto target =
                        test_support::choiceTarget(*model.file("choices"), SourceSide::Stage2, ChoiceScope::Section);
                    const auto combination = previewChoice(*model.file("choices"), 1, target.source.contributionId,
                                                           ChoiceAction::CombineRightFirst);
                    const auto batch = previewNonConflicting(*model.file("choices"), 1);
                    const auto whole = previewWholeFile(*model.file("choices"), 1, SourceSide::Stage3);
                    const test_support::ArmedFault fault(*faults, test_support::SyncFaults::Point::JournalSync);
                    const auto result = operation == 0 ? model.applyChoice("choices", combination, true)
                        : operation == 1               ? model.applyNonConflicting("choices", batch, true)
                                                       : model.applyWholeFile("choices", whole, true);
                    CHECK(result == Transition::Blocked && faults->failures == 1);
                    CHECK(model.file("choices")->current().id == 1 && model.file("choices")->current().draft == before);
                    CHECK(model.pendingDraft("choices") && *model.pendingDraft("choices") != before);
                    const auto chosen = *model.pendingDraft("choices");
                    CHECK(model.undo("choices", 1) == Transition::Blocked);
                    CHECK(model.retryRecovery("choices") == Transition::Applied);
                    CHECK(model.file("choices")->current().draft == chosen && !model.hasPendingRecovery());
                    CHECK(replayedFile(model.recoveryDirectory()).current().draft == chosen);
                    const auto read = storage::RecoveryStore(model.recoveryDirectory()).read();
                    CHECK(read.succeeded && read.transactions.size() == 2);
                    CHECK(model.undo("choices", model.file("choices")->current().id) == Transition::Applied);
                    CHECK(replayedFile(model.recoveryDirectory()).current().draft == before);
                });
        }
        for (const bool staging : {false, true}) {
            const auto kind = staging ? MutationKind::Staging : MutationKind::Publication;
            const auto kindName = staging ? std::string("staging") : std::string("publication");
            suite.run("successive successful outcomes retain every earlier completion " + kindName, [=] {
                QTemporaryDir directory;
                CHECK(directory.isValid());
                TestSession model(directory.path() + "/recovery");
                CHECK(model.appendFile(recoverableFile()));
                QStringList identities;
                std::vector<RevisionId> revisions;
                for (const auto &text : {std::string("first published\n"), std::string("second published\n")}) {
                    const auto current = model.file("recoverable")->current();
                    auto next = current.draft;
                    next.text = text;
                    CHECK(model.replaceDraft("recoverable", current.id, next) == Transition::Applied);
                    preparePublication(model, "recoverable", staging);
                    revisions.push_back(model.file("recoverable")->current().id);
                    identities.push_back(recordIntent(model, "recoverable", kind));
                    completePublication(model, "recoverable", staging);
                }
                const auto replay = replayRecovery(storage::RecoveryStore(model.recoveryDirectory()));
                CHECK(replay.succeeded && replay.mutations.size() == 2 && revisions[0] != revisions[1]);
                for (std::size_t i = 0; i < replay.mutations.size(); ++i) {
                    CHECK(replay.mutations[i].completed
                          && replay.mutations[i].id == identities[static_cast<qsizetype>(i)]);
                    CHECK(replay.mutations[i].kind == kind && replay.mutations[i].target.current().id == revisions[i]);
                }
                CHECK(replay.files.front().current().draft.text == "second published\n");
            });
            suite.run("earlier same-revision success cannot complete a later intent " + kindName, [=] {
                QTemporaryDir directory;
                CHECK(directory.isValid());
                TestSession model(directory.path() + "/recovery");
                CHECK(model.appendFile(recoverableFile()));
                preparePublication(model, "recoverable", staging);
                const auto first = recordIntent(model, "recoverable", kind);
                completePublication(model, "recoverable", staging);
                const auto interrupted = recordIntent(model, "recoverable", kind);
                const auto replay = replayRecovery(storage::RecoveryStore(model.recoveryDirectory()));
                CHECK(replay.succeeded && replay.mutations.size() == 2);
                CHECK(replay.mutations[0].id == first && replay.mutations[0].completed);
                CHECK(replay.mutations[1].id == interrupted && !replay.mutations[1].completed);
                CHECK(replay.mutations[0].target.current().id == replay.mutations[1].target.current().id);
            });
            suite.run("a later revision outcome leaves an unmatched earlier interruption uncertain " + kindName, [=] {
                QTemporaryDir directory;
                CHECK(directory.isValid());
                TestSession model(directory.path() + "/recovery");
                CHECK(model.appendFile(recoverableFile()));
                preparePublication(model, "recoverable", staging);
                const auto interrupted = recordIntent(model, "recoverable", kind);
                auto edited = model.file("recoverable")->current().draft;
                edited.text = "newer successful revision\n";
                CHECK(model.replaceDraft("recoverable", 1, edited) == Transition::Applied);
                preparePublication(model, "recoverable", staging);
                const auto completed = recordIntent(model, "recoverable", kind);
                completePublication(model, "recoverable", staging);
                const auto replay = replayRecovery(storage::RecoveryStore(model.recoveryDirectory()));
                CHECK(replay.succeeded && replay.mutations.size() == 2);
                CHECK(replay.mutations[0].id == interrupted && !replay.mutations[0].completed);
                CHECK(replay.mutations[1].id == completed && replay.mutations[1].completed);
            });
        }
        suite.run("outcomes match the file and kind and failed staging does not complete an intent", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            TestSession model(directory.path() + "/recovery");
            CHECK(model.appendFile(recoverableFile()));
            auto captures = *recoverableFile().snapshots();
            captures.path = "other.txt";
            CHECK(model.appendFile(FileSession("other", captures.path, draft(captures.launchResult.text.utf8),
                                               Support::Supported, {}, captures)));
            preparePublication(model, "recoverable", false);
            preparePublication(model, "other", false);
            recordIntent(model, "recoverable", MutationKind::Publication);
            recordIntent(model, "recoverable", MutationKind::Staging);
            recordIntent(model, "other", MutationKind::Publication);
            completePublication(model, "recoverable", false);
            CHECK(model.recordStage("recoverable", 1, Outcome::Failed, {}, {}, "stage failed") == Transition::Applied);
            auto replay = replayRecovery(storage::RecoveryStore(model.recoveryDirectory()));
            CHECK(replay.succeeded && replay.mutations.size() == 3);
            CHECK(replay.mutations[0].completed && !replay.mutations[1].completed && !replay.mutations[2].completed);
            completePublication(model, "recoverable", true);
            replay = replayRecovery(storage::RecoveryStore(model.recoveryDirectory()));
            CHECK(replay.succeeded && replay.mutations[1].completed && !replay.mutations[2].completed);
            completePublication(model, "other", false);
            replay = replayRecovery(storage::RecoveryStore(model.recoveryDirectory()));
            CHECK(replay.succeeded && replay.mutations[2].completed);
        });
        for (const auto action : {ChoiceAction::Accept, ChoiceAction::Ignore, ChoiceAction::ResolveUsingSide}) {
            suite.run(
                "command undo and redo each replay complete text and decisions "
                    + std::to_string(static_cast<int>(action)),
                [=] {
                    QTemporaryDir directory;
                    CHECK(directory.isValid());
                    TestSession model(directory.path() + "/recovery");
                    CHECK(model.appendFile(test_support::choiceFile()));
                    CHECK(model.editResult("choices", 1, {{0, 0}, "manual prefix\n"}) == Transition::Applied);
                    CHECK(model.recordCheckpoint("choices", 2, Outcome::Succeeded) == Transition::Applied);
                    const auto before = model.file("choices")->current().draft;
                    const auto target =
                        test_support::choiceTarget(*model.file("choices"), SourceSide::Stage3, ChoiceScope::Section);
                    const auto preview = previewChoice(*model.file("choices"), 2, target.source.contributionId, action);
                    CHECK(model.applyChoice("choices", preview, true) == Transition::Applied);
                    const auto chosen = model.file("choices")->current().draft;
                    const auto check = [&](const Draft &expected) {
                        const auto replay = replayedFile(model.recoveryDirectory());
                        CHECK(replay.current().draft == expected);
                        CHECK(replay.current().id == model.file("choices")->current().id);
                        CHECK(replay.checkpoint() && replay.checkpoint()->draft == before);
                        CHECK(!replay.reviewed() && !replay.written() && !replay.staged() && !replay.facts().resolved);
                    };
                    CHECK(chosen != before);
                    check(chosen);
                    CHECK(model.undo("choices", model.file("choices")->current().id) == Transition::Applied);
                    check(before);
                    CHECK(model.redo("choices", model.file("choices")->current().id) == Transition::Applied);
                    check(chosen);
                    auto reopened = replayedFile(model.recoveryDirectory());
                    CHECK(reopened.undo(reopened.current().id) == Transition::Applied
                          && reopened.current().draft == before);
                    CHECK(reopened.redo(reopened.current().id) == Transition::Applied
                          && reopened.current().draft == chosen);
                });
        }
        suite.run("a failed choice retains its atomic candidate until durable retry", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            const auto faults = std::make_shared<test_support::SyncFaults>();
            TestSession model(directory.path() + "/recovery", faults);
            CHECK(model.appendFile(test_support::choiceFile()));
            const auto before = model.file("choices")->current().draft;
            const auto target =
                test_support::choiceTarget(*model.file("choices"), SourceSide::Stage3, ChoiceScope::Section);
            const auto preview =
                previewChoice(*model.file("choices"), 1, target.source.contributionId, ChoiceAction::ResolveUsingSide);
            const test_support::ArmedFault fault(*faults, test_support::SyncFaults::Point::JournalSync);
            CHECK(model.applyChoice("choices", preview, true) == Transition::Blocked);
            CHECK(faults->failures == 1 && model.file("choices")->current().draft == before);
            CHECK(model.pendingDraft("choices") && model.pendingDraft("choices")->decisions != before.decisions);
            const auto pending = *model.pendingDraft("choices");
            CHECK(role(model, SessionModel::PendingInputRole).toBool());
            CHECK(role(model, SessionModel::RecoveryStateRole).value<FileStates::RecoveryState>()
                  == FileStates::RecoveryState::Failed);
            CHECK(!model.hasRecoveryFailure("choices") && !role(model, SessionModel::CanUndoRole).toBool());
            CHECK(model.undo("choices", 1) == Transition::Blocked);
            CHECK(model.retryRecovery("choices") == Transition::Applied);
            CHECK(!model.hasPendingRecovery() && model.file("choices")->current().draft == pending);
            CHECK(replayedFile(model.recoveryDirectory()).current().draft == pending);
            const auto records = storage::RecoveryStore(model.recoveryDirectory()).read();
            CHECK(records.succeeded && records.transactions.size() == 2);
        });
        for (const int checkpoint : {0, 1, 2}) {
            suite.run("durable discard replays instead of the abandoned draft " + std::to_string(checkpoint), [=] {
                QTemporaryDir directory;
                CHECK(directory.isValid());
                const auto location = directory.path() + "/recovery";
                Draft retained;
                {
                    TestSession model(location);
                    CHECK(model.appendFile(recoverableFile()));
                    if (checkpoint != 0) {
                        CHECK(model.replaceDraft("recoverable", 1, savedDraft()) == Transition::Applied);
                        if (checkpoint == 2)
                            resolve(model, "recoverable");
                        else
                            CHECK(model.recordCheckpoint("recoverable", 2, Outcome::Succeeded) == Transition::Applied);
                    }
                    retained = model.file("recoverable")->current().draft;
                    CHECK(model.replaceDraft("recoverable", model.file("recoverable")->current().id, unsavedDraft())
                          == Transition::Applied);
                    const auto revision = model.file("recoverable")->current().id;
                    CHECK((checkpoint == 2 ? model.discardToStaged("recoverable", revision)
                                           : model.discardToCheckpoint("recoverable", revision))
                          == Transition::Applied);
                    CHECK(model.file("recoverable")->current().draft == retained && !model.hasPendingRecovery());
                }
                const auto restored = replayedFile(location);
                CHECK(restored.current().draft == retained && restored.current().draft != unsavedDraft());
                CHECK(restored.checkpoint().has_value() == (checkpoint != 0));
                CHECK(restored.facts().resolved == (checkpoint == 2));
                CHECK(replayedFile(location).current().draft == retained);
            });
        }
        for (const int corruption : {0, 1, 2}) {
            suite.run(
                "invalid schema session or file identity rejects the entire replay " + std::to_string(corruption), [=] {
                    QTemporaryDir directory;
                    CHECK(directory.isValid());
                    const auto lease = test_support::leasedStore(directory.path() + "/recovery");
                    const auto &store = lease->store();
                    auto file = recoverableFile();
                    CHECK(lease
                              ->commit(
                                  recoveryTransaction(file, RecoveryAction::Open, QUuid::createUuid().toString(), {}))
                              .succeeded);
                    const auto valid = replayRecovery(store);
                    CHECK(valid.succeeded && valid.files.size() == 1);
                    const auto before = file.current().draft.text;
                    CHECK(file.replaceDraft(1, unsavedDraft()) == Transition::Applied);
                    auto invalid =
                        recoveryTransaction(file, RecoveryAction::ReplaceDraft, QUuid::createUuid().toString(), before);
                    QString expected;
                    if (corruption == 0) {
                        expected = "schema";
                    } else if (corruption == 1) {
                        // Only a launch record may carry captures.
                        const QByteArray wrong = "another-session";
                        const auto hash = storage::RecoveryStore::digest(wrong);
                        invalid.payloads.insert(hash, wrong);
                        invalid.state.insert("snapshots", QJsonObject{{"sessionId", QString::fromLatin1(hash)}});
                        expected = "does not match the acknowledged record";
                    } else {
                        const QByteArray wrong = "another-file";
                        const auto hash = storage::RecoveryStore::digest(wrong);
                        invalid.payloads.insert(hash, wrong);
                        invalid.state.insert("fileId", QString::fromLatin1(hash));
                        expected = "matching open file";
                    }
                    CHECK(lease->commit(invalid).succeeded);
                    if (corruption == 0)
                        test_support::rewriteLastRecoveryState(store.directory(),
                                                               [](QJsonObject &state) { state.insert("schema", 99); });
                    // The store checks frames; the state belongs to replay.
                    CHECK(store.read().succeeded);
                    const auto replay = replayRecovery(store);
                    CHECK(!replay.succeeded && replay.files.empty() && replay.mutations.empty());
                    CHECK(replay.error.contains(expected));
                });
        }
        suite.run(
            "an immediate process exit after acknowledgment preserves edits decisions captures and Save separately",
            [] {
                QTemporaryDir directory;
                CHECK(directory.isValid());
                const auto location = directory.path() + "/recovery";
                QProcess process;
                process.start(QCoreApplication::applicationFilePath(), {"--acknowledge-and-crash", location});
                CHECK(process.waitForStarted(5000) && process.waitForFinished(10000));
                CHECK(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0);
                CHECK(process.readAllStandardOutput() == "ACK 4\n");
                const storage::RecoveryStore reopened(location);
                const auto restored = replayedFile(location);
                CHECK(restored.current().id == 4 && restored.current().draft == finalDecision());
                CHECK(restored.checkpoint() && restored.checkpoint()->draft == savedDraft());
                CHECK(restored.facts().unsaved && !restored.facts().resolved);
                CHECK(!restored.reviewed() && !restored.written() && !restored.staged());
                CHECK(restored.current().draft.decisions.size() == 3);
                const auto recovered = reopened.read();
                CHECK(recovered.succeeded && !recovered.incompleteTail && recovered.transactions.size() == 5);
                const auto last = recovered.transactions.back().state;
                const auto current = last.value("current").toObject();
                CHECK(last.value("schema").toInt() == 2 && !recovered.transactions.back().id.isEmpty());
                CHECK(current.value("id") == "4" && current.value("result") == "3");
                checkDraft(test_support::recordedDrafts(reopened).back(), finalDecision());
                const auto checkpoint = last.value("checkpoint").toObject();
                CHECK(checkpoint.value("id") == "2");
                CHECK(last.value("reviewed").isNull() && last.value("written").isNull()
                      && last.value("staged").isNull());
                const auto expected = recoverableFile();
                CHECK(payload(reopened, last.value("path")) == QByteArray::fromStdString(expected.path()));
                // The launch record carries the captures; later records do not repeat them.
                CHECK(!last.contains("snapshots"));
                const auto snapshots = recovered.transactions.front().state.value("snapshots").toObject();
                CHECK(snapshots.value("earlier").isNull());
                const auto left = snapshots.value("stage2").toObject();
                CHECK(payload(reopened, left.value("rawBytes")) == "left\n");
                CHECK(payload(reopened, left.value("objectId")) == "left-object");
                CHECK(payload(reopened, left.value("worktree").toObject().value("bytes")) == "left\r\n");
                const auto right = snapshots.value("stage3").toObject();
                CHECK(payload(reopened, right.value("rawBytes")) == "right\n");
                CHECK(payload(reopened, right.value("objectId")) == "right-object");
                CHECK(payload(reopened, right.value("commitId")) == "right-commit");
                const auto launch = snapshots.value("launchResult").toObject();
                CHECK(payload(reopened, launch.value("text").toObject().value("bytes"))
                      == "\xef\xbb\xbfmanual launch\r\n");
                CHECK(launch.value("text").toObject().value("format").toObject().value("bom").toBool());
                CHECK(launch.value("identity").toObject().value("inode") == "22");
                const auto provenance = snapshots.value("provenance").toObject();
                CHECK(payload(reopened, provenance.value("sessionId")) == recoverySessionId);
                CHECK(payload(reopened, provenance.value("worktreeRoot")) == "/disposable-worktree");
                CHECK(payload(reopened, provenance.value("gitDirectory")) == "/disposable-worktree/.git");
                const auto attributes = snapshots.value("conversion").toObject().value("attributes").toArray();
                QMap<QByteArray, QByteArray> policy;
                for (const auto &attribute : attributes)
                    policy.insert(payload(reopened, attribute.toObject().value("key")),
                                  payload(reopened, attribute.toObject().value("value")));
                CHECK(policy == QMap<QByteArray, QByteArray>{{"eol", "crlf"}, {"text", "set"}});
            });
        suite.run("failed recovery retains pending input separately and retries its completed record once", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            const auto faults = std::make_shared<test_support::SyncFaults>();
            TestSession model(directory.path() + "/recovery", faults);
            CHECK(model.appendFile(recoverableFile()));
            const auto original = model.file("recoverable")->current().draft;
            const test_support::ArmedFault fault(*faults, test_support::SyncFaults::Point::JournalSync);
            CHECK(model.replaceDraft("recoverable", 1, unsavedDraft()) == Transition::Blocked);
            CHECK(faults->failures == 1 && model.hasPendingRecovery());
            CHECK(model.file("recoverable")->current().id == 1
                  && model.file("recoverable")->current().draft == original);
            CHECK(model.pendingDraft("recoverable") && *model.pendingDraft("recoverable") == unsavedDraft());
            CHECK(role(model, SessionModel::PendingInputRole).toBool());
            CHECK(role(model, SessionModel::PendingTextRole).toString().toUtf8()
                  == QByteArray::fromStdString(unsavedDraft().text));
            CHECK(model.pendingDraft("recoverable")->decisions.size() == 2);
            CHECK(role(model, SessionModel::RecoveryStateRole).value<FileStates::RecoveryState>()
                  == FileStates::RecoveryState::Failed);
            CHECK(role(model, SessionModel::RecoveryErrorRole).toString().contains("retry"));
            CHECK(!role(model, SessionModel::CanUndoRole).toBool() && !model.canFinish());
            CHECK(model.replaceDraft("recoverable", 1, draft("must not overwrite pending")) == Transition::Blocked);
            CHECK(model.recordCheckpoint("recoverable", 1, Outcome::Succeeded) == Transition::Blocked);
            CHECK(model.undo("recoverable", 1) == Transition::Blocked);
            storage::RecoveryStore store(model.recoveryDirectory());
            const auto before = store.read();
            CHECK(before.succeeded && before.transactions.size() == 2);
            const auto request = before.transactions.back().id;
            CHECK(model.retryPendingRecovery("recoverable"));
            CHECK(!model.hasPendingRecovery() && !model.pendingDraft("recoverable"));
            CHECK(model.file("recoverable")->current().id == 2
                  && model.file("recoverable")->current().draft == unsavedDraft());
            CHECK(role(model, SessionModel::RecoveryStateRole).value<FileStates::RecoveryState>()
                  == FileStates::RecoveryState::Durable);
            CHECK(role(model, SessionModel::RecoveryErrorRole).toString().isEmpty());
            CHECK(model.file("recoverable")->facts().unsaved && !model.file("recoverable")->checkpoint());
            const auto after = store.read();
            CHECK(after.succeeded && after.transactions.size() == 2 && after.transactions.back().id == request);
            checkDraft(test_support::recordedDrafts(store).back(), unsavedDraft());
            CHECK(model.undo("recoverable", 2) == Transition::Applied);
            CHECK(model.file("recoverable")->current().draft == original);
        });
        suite.run("empty model and invalid indexes", [] {
            TestSession model;
            CHECK(model.totalFiles() == 0 && model.rowCount() == 0 && model.resolvedFiles() == 0);
            CHECK(!model.canFinish() && model.finishBlockers().isEmpty());
            CHECK(model.handoff().value("outcome").toString() == "No files are loaded in this session.");
            CHECK(!model.file("missing") && !model.data({}, SessionModel::PathRole).isValid());
            CHECK(model.appendFile(FileSession("id", "file", draft())));
            CHECK(model.rowCount(model.index(0)) == 0 && !model.index(1).isValid());
            CHECK(!model.data(model.index(0), -1).isValid());
            TestSession other;
            CHECK(other.appendFile(FileSession("other", "path", draft())));
            CHECK(!model.data(other.index(0), SessionModel::PathRole).isValid());
        });
        suite.run("recovery root ignores the organization name and finds earlier stores", [] {
            test_support::ScopedApplicationIdentity identity("Branch Truce", {});
            const auto root =
                QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Branch Truce/recovery/v1";
            // The earlier build's root: AppLocalDataLocation with no organization name.
            const auto earlier =
                QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/recovery/v1";
            CHECK(earlier == root && recoveryRoot() == root);
            const auto stored = root + "/" + QUuid::createUuid().toString(QUuid::WithoutBraces);
            {
                TestSession written(stored);
                CHECK(written.appendFile(recoverableFile()));
                CHECK(written.replaceDraft("recoverable", 1, unsavedDraft()) == Transition::Applied);
            }
            QCoreApplication::setOrganizationName("Example Organization");
            CHECK(recoveryRoot() == root);
            TestSession model;
            CHECK(QFileInfo(model.recoveryDirectory()).absolutePath() == root);
            CHECK(QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot).contains(QFileInfo(stored).fileName()));
            const auto replay = replayRecovery(storage::RecoveryStore(stored));
            CHECK(replay.succeeded && replay.userWork && replay.files.size() == 1
                  && replay.files.front().current().draft == unsavedDraft());
        });
        suite.run("named roles preserve byte paths, Unicode and 64-bit decision order", [] {
            TestSession model;
            auto unicode = draft("A\xc3\xa9\xf0\x9f\x98\x80\n");
            unicode.decisions["left"].order = std::numeric_limits<std::uint64_t>::max();
            unicode.decisions["right"] = {DecisionValue::Ignored, {}};
            const std::string path = std::string(":literal\t\n") + static_cast<char>(0xff);
            CHECK(model.appendFile(FileSession("id", path, unicode)));
            CHECK(model.file("id")->path() == path);
            CHECK(model.file("id")->current().draft.text == unicode.text);
            CHECK(role(model, SessionModel::CurrentRevisionRole).toString() == "1");
            CHECK(model.file("id")->current().result == 1 && model.file("id")->current().decisions == 1);
            const auto decisions = role(model, SessionModel::DecisionsRole).toList();
            CHECK(decisions.size() == 2);
            CHECK(decisions[0].toMap().value("value").toString() == "accepted");
            CHECK(decisions[0].toMap().value("order").toString() == "18446744073709551615");
            CHECK(decisions[1].toMap().value("value").toString() == "ignored");
            CHECK(!decisions[1].toMap().contains("order"));
            const auto names = model.roleNames();
            // Progress and the view roles follow the recovery roles.
            CHECK(names.size() == SessionModel::ConflictCountRole - SessionModel::FileIdRole + 1);
            CHECK(names.value(SessionModel::ProgressRole) == "progress");
            CHECK(names.value(SessionModel::StatusIconRole) == "statusIcon");
            CHECK(names.value(SessionModel::StatusLineRole) == "statusLine");
            CHECK(names.value(SessionModel::ChipsRole) == "chips");
            CHECK(names.value(SessionModel::SummaryRole) == "summary");
            CHECK(names.value(SessionModel::ConflictCountRole) == "conflictCount");
            QSet<QByteArray> unique;
            for (auto it = names.cbegin(); it != names.cend(); ++it) {
                CHECK(!it.value().isEmpty() && !unique.contains(it.value()));
                unique.insert(it.value());
            }
        });
        suite.run("conflict counts follow marker history without erasing source decisions", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            TestSession model(directory.path() + "/recovery");
            CHECK(model.appendFile(test_support::splitFile()));
            const auto launch = model.file("choices")->current().draft;
            CHECK(role(model, SessionModel::ConflictCountRole).toInt() == 1);
            CHECK(role(model, SessionModel::UndecidedRole).toInt() > 1);
            auto edited = launch;
            edited.text = "manual result without conflict markers\n";
            edited.unresolvedMarkers = 0;
            CHECK(model.replaceDraft("choices", 1, edited) == Transition::Applied);
            CHECK(role(model, SessionModel::ConflictCountRole).toInt() == 0);
            CHECK(role(model, SessionModel::UndecidedRole).toInt() > 0);
            CHECK(model.undo("choices", model.file("choices")->current().id) == Transition::Applied);
            CHECK(model.file("choices")->current().draft == launch);
            CHECK(role(model, SessionModel::ConflictCountRole).toInt() == 1);
            CHECK(model.redo("choices", model.file("choices")->current().id) == Transition::Applied);
            CHECK(model.file("choices")->current().draft == edited);
            CHECK(role(model, SessionModel::ConflictCountRole).toInt() == 0);
            CHECK(replayedFile(model.recoveryDirectory()).current().draft == edited);
        });
        suite.run("saved and reviewed drafts do not receive the published check tile", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            TestSession model(directory.path() + "/recovery");
            CHECK(model.appendFile(sessionFile("status")));
            const auto revision = model.file("status")->current().id;
            CHECK(role(model, SessionModel::StatusIconRole).toString() != "emblem-checked");
            CHECK(model.recordCheckpoint("status", revision, Outcome::Succeeded) == Transition::Applied);
            CHECK(role(model, SessionModel::StatusIconRole).toString() != "emblem-checked");
            CHECK(model.beginMarkResolved("status", revision) == Transition::Applied);
            CHECK(role(model, SessionModel::StatusIconRole).toString() != "emblem-checked");
            CHECK(model.recordWrite("status", revision, Outcome::Succeeded) == Transition::Applied);
            CHECK(role(model, SessionModel::StatusIconRole).toString() != "emblem-checked");
            CHECK(model.recordStage("status", revision, Outcome::Succeeded, "verified-object", "100644")
                  == Transition::Applied);
            CHECK(role(model, SessionModel::StatusIconRole).toString() == "emblem-checked");
        });
        suite.run("row insertion and notifications occur only for applied commands", [] {
            TestSession model;
            int summaries = 0;
            int changes = 0;
            int insertions = 0;
            QObject::connect(&model.rows, &SessionModel::summaryChanged, &model, [&] { ++summaries; });
            QObject::connect(&model.rows, &QAbstractItemModel::rowsInserted, &model,
                             [&](const QModelIndex &parent, int first, int last) {
                                 CHECK(!parent.isValid() && first == last);
                                 ++insertions;
                             });
            QObject::connect(&model.rows, &QAbstractItemModel::dataChanged, &model,
                             [&](const QModelIndex &top, const QModelIndex &bottom, const QList<int> &roles) {
                                 CHECK(top.row() == 0 && top == bottom && roles.isEmpty());
                                 ++changes;
                             });
            CHECK(model.appendFile(FileSession("id", "file", draft())));
            const QPersistentModelIndex first(model.index(0));
            CHECK(!model.appendFile(FileSession("id", "duplicate", draft())));
            CHECK(summaries == 1 && insertions == 1 && changes == 0);
            CHECK(model.replaceDraft("missing", 1, draft("missing")) == Transition::StaleRevision);
            CHECK(model.replaceDraft("id", 1, draft()) == Transition::NoChange);
            CHECK(model.replaceDraft("id", 99, draft("stale")) == Transition::StaleRevision);
            CHECK(model.recordWrite("id", 1, Outcome::Succeeded) == Transition::StaleRevision);
            CHECK(summaries == 1 && changes == 0);
            CHECK(model.replaceDraft("id", 1, draft("changed")) == Transition::Applied);
            CHECK(changes == 1 && summaries == 2);
            CHECK(model.beginMarkResolved("id", 2) == Transition::Applied);
            CHECK(model.recordWrite("id", 2, Outcome::Succeeded) == Transition::InvalidOutcome);
            CHECK(changes == 2 && summaries == 3);
            CHECK(model.appendFile(FileSession("second", "second", draft())));
            CHECK(first.isValid() && first.row() == 0 && first.data(SessionModel::FileIdRole) == "id");
            CHECK(insertions == 2 && summaries == 4);
        });
        suite.run("Save and publication roles stay distinct and handoff includes every file", [] {
            TestSession model;
            CHECK(model.appendFile(FileSession("first", "a", draft())));
            CHECK(model.recordCheckpoint("first", 1, Outcome::Succeeded) == Transition::Applied);
            CHECK(!role(model, SessionModel::UnsavedRole).toBool());
            CHECK(role(model, SessionModel::ReviewNeededRole).toBool());
            CHECK(!model.file("first")->reviewed());
            CHECK(!model.file("first")->written());
            CHECK(!model.file("first")->staged());
            CHECK(model.file("first")->checkpoint()->draft.text == "result\n");
            resolve(model, "first");
            CHECK(model.canFinish() && model.resolvedFiles() == 1 && model.finishBlockers().isEmpty());
            CHECK(model.file("first")->staged()->objectId == "verified-object");
            CHECK(model.file("first")->staged()->mode == "100644");
            CHECK(model.appendFile(FileSession("second", "b", draft(), Support::OutsideApp, "binary")));
            CHECK(model.totalFiles() == 2 && model.resolvedFiles() == 1 && !model.canFinish());
            CHECK(role(model, SessionModel::SupportRole, 1).value<FileStates::SupportKind>()
                  == FileStates::SupportKind::OutsideApp);
            CHECK(role(model, SessionModel::SupportDetailRole, 1).toString() == "binary");
            CHECK(model.finishBlockers().size() == 1);
            CHECK(model.finishBlockers()[0].toMap().value("fileId").toString() == "second");
            CHECK(model.setSupport("second", Support::Unreadable, "read failed") == Transition::Applied);
            CHECK(role(model, SessionModel::SupportRole, 1).value<FileStates::SupportKind>()
                  == FileStates::SupportKind::Unreadable);
            CHECK(!model.canFinish());
            CHECK(model.setSupport("second", Support::Supported) == Transition::Applied);
            resolve(model, "second");
            CHECK(model.canFinish() && model.resolvedFiles() == 2);
            CHECK(model.recordCheckpoint("second", 1, Outcome::Failed, "disk full") == Transition::Applied);
            CHECK(!model.canFinish() && model.resolvedFiles() == 1);
            CHECK(model.file("second")->checkpointOutcome()->detail == "disk full");
            CHECK(role(model, SessionModel::StatusRole, 1).toString() == "Draft save failed");
        });
        suite.run("the English catalog supplies the plural forms of numerus messages", [] {
            const auto left = [](int count) {
                return QCoreApplication::translate("FilePresentation", "%n conflict(s) left", nullptr, count);
            };
            CHECK(left(1) == "1 conflict left" && left(2) == "2 conflicts left");
            CHECK(QCoreApplication::translate("SessionFooter", "%1 of %n file(s) resolved", nullptr, 1).arg(0)
                  == "0 of 1 file resolved");
        });
        suite.run("handoff counts use singular and plural file wording", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            TestSession model(directory.path() + "/recovery");
            CHECK(model.appendFile(sessionFile("first")));
            CHECK(model.handoff().value("outcome").toString().startsWith("1 of 1 file in this session remains"));
            resolve(model, "first");
            CHECK(model.handoff().value("outcome").toString().startsWith("1 file in this session is written"));
            CHECK(model.appendFile(sessionFile("second")));
            CHECK(model.handoff().value("outcome").toString().startsWith("1 of 2 files in this session remain"));
            resolve(model, "second");
            CHECK(model.handoff().value("outcome").toString().startsWith("All 2 files in this session are written"));
        });
        suite.run("handoff puts each mixed outcome in one category and pending input stays failed until retried", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            const auto faults = std::make_shared<test_support::SyncFaults>();
            TestSession model(directory.path() + "/recovery", faults);
            auto choices = draft();
            choices.decisions["left"] = {};
            CHECK(model.appendFile(sessionFile("undecided", choices)));
            CHECK(model.appendFile(sessionFile("saved")));
            CHECK(model.recordCheckpoint("saved", 1, Outcome::Succeeded) == Transition::Applied);
            CHECK(model.appendFile(sessionFile("review")));
            CHECK(model.appendFile(sessionFile("newer")));
            resolve(model, "newer");
            CHECK(model.replaceDraft("newer", 1, draft("newer draft\n")) == Transition::Applied);
            CHECK(model.appendFile(sessionFile("written")));
            CHECK(model.beginMarkResolved("written", 1) == Transition::Applied);
            CHECK(model.recordCheckpoint("written", 1, Outcome::Succeeded) == Transition::Applied);
            CHECK(model.recordWrite("written", 1, Outcome::Succeeded) == Transition::Applied);
            CHECK(model.appendFile(sessionFile("published")));
            resolve(model, "published");
            CHECK(model.appendFile(sessionFile("failed")));
            CHECK(model.beginMarkResolved("failed", 1) == Transition::Applied);
            CHECK(model.recordCheckpoint("failed", 1, Outcome::Succeeded) == Transition::Applied);
            CHECK(model.recordWrite("failed", 1, Outcome::Failed, "disk full") == Transition::Applied);
            CHECK(model.appendFile(FileSession("outside", "diagram.png", {}, Support::OutsideApp, "image content")));
            CHECK(model.appendFile(sessionFile("pending")));
            {
                const test_support::ArmedFault fault(*faults, test_support::SyncFaults::Point::JournalSync);
                CHECK(model.replaceDraft("pending", 1, draft("pending input\n")) == Transition::Blocked);
                CHECK(faults->failures == 1);
            }
            CHECK(model.hasPendingRecovery() && !model.canFinish());

            struct Expected {
                const char *category;
                FileStates::NextAction action;
            };
            using enum FileStates::NextAction;
            const QMap<QString, Expected> expected{
                {"undecided", {"undecided", Decide}},          {"saved", {"saved-draft", MarkResolved}},
                {"review", {"review-needed", MarkResolved}},   {"newer", {"newer-draft", ApplyOrDiscard}},
                {"written", {"written-unresolved", CheckGit}}, {"published", {"published", None}},
                {"failed", {"failed", MarkResolved}},          {"outside", {"outside-app", ExplainOutsideApp}},
                {"pending", {"failed", RetryRecovery}}};
            auto summary = model.handoff();
            auto entries = handoffEntries(summary);
            CHECK(entries.size() == expected.size());
            for (auto wanted = expected.cbegin(); wanted != expected.cend(); ++wanted) {
                const auto entry = entries.value(wanted.key());
                if (entry.value("category").toString() != wanted->category
                    || entry.value("action").value<FileStates::NextAction>() != wanted->action)
                    throw std::runtime_error((wanted.key() + ": " + entry.value("category").toString() + "/"
                                              + QString::number(entry.value("action").toInt()))
                                                 .toStdString());
                CHECK(entry.value("outstanding").toBool() == (wanted.key() != "published"));
                CHECK(entry.value("outstanding").toBool() != entry.value("blockers").toStringList().isEmpty());
                CHECK(entry.value("outstanding").toBool() != entry.value("actionLabel").toString().isEmpty());
            }
            QStringList order;
            for (const auto &group : summary.value("groups").toList())
                order << group.toMap().value("category").toString();
            CHECK(order
                  == QStringList({"undecided", "saved-draft", "review-needed", "newer-draft", "written-unresolved",
                                  "failed", "outside-app", "published"}));
            const auto counts = summary.value("counts").toMap();
            CHECK(counts.value("failed").toInt() == 2 && counts.value("review-needed").toInt() == 1);
            CHECK(counts.value("outside-app").toInt() == 1 && counts.value("published").toInt() == 1);
            CHECK(!summary.value("clean").toBool());
            CHECK(summary.value("totalFiles").toInt() == 9 && summary.value("outstandingFiles").toInt() == 8);
            CHECK(summary.value("outcome").toString()
                  == "8 of 9 files in this session remain outstanding. This session is not finished.");
            CHECK(entries.value("pending").value("status").toString() == "Recovery failed; pending input retained");
            CHECK(entries.value("pending")
                      .value("blockers")
                      .toStringList()
                      .contains("Pending input needs recovery retry"));
            CHECK(entries.value("pending").value("actionLabel").toString() == "Retry recovery");
            CHECK(entries.value("pending").value("unsaved").toBool());
            CHECK(entries.value("review").value("actionLabel").toString() == "Save or Mark resolved");
            CHECK(entries.value("undecided").value("undecided").toULongLong() == 1);
            CHECK(entries.value("failed").value("status").toString() == "Repository write failed");
            CHECK(entries.value("newer").value("status").toString()
                  == "Earlier result staged; current draft not applied");

            CHECK(model.retryRecovery("pending") == Transition::Applied);
            CHECK(!model.hasPendingRecovery() && model.file("pending")->current().draft == draft("pending input\n"));
            summary = model.handoff();
            entries = handoffEntries(summary);
            CHECK(entries.value("pending").value("category").toString() == "review-needed");
            CHECK(entries.value("pending").value("action").value<FileStates::NextAction>()
                  == FileStates::NextAction::MarkResolved);
            CHECK(entries.value("pending").value("status").toString() == "Current result needs review");
            CHECK(!entries.value("pending")
                       .value("blockers")
                       .toStringList()
                       .contains("Pending input needs recovery retry"));
            CHECK(summary.value("counts").toMap().value("failed").toInt() == 1);
            CHECK(summary.value("counts").toMap().value("review-needed").toInt() == 2);
            CHECK(summary.value("outstandingFiles").toInt() == 8 && !summary.value("clean").toBool());
        });
        suite.run("a failed write, stage or recovery shows the handoff's icon in the list", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            TestSession model(directory.path() + "/recovery");
            for (const auto *id : {"write", "stage", "recovery"}) CHECK(model.appendFile(sessionFile(id)));
            const auto icon = [&](const std::string &id) {
                return role(model, SessionModel::StatusIconRole, model.rowOf(id)).toString();
            };
            // A fresh draft needs review and Save; the cached facts must follow each outcome.
            CHECK(icon("write") == "vcs-locally-modified");
            CHECK(model.beginMarkResolved("write", 1) == Transition::Applied);
            CHECK(model.recordCheckpoint("write", 1, Outcome::Succeeded) == Transition::Applied);
            CHECK(icon("write") == "document-save");
            CHECK(model.recordWrite("write", 1, Outcome::Failed, "disk full") == Transition::Applied);
            CHECK(model.beginMarkResolved("stage", 1) == Transition::Applied);
            CHECK(model.recordCheckpoint("stage", 1, Outcome::Succeeded) == Transition::Applied);
            CHECK(model.recordWrite("stage", 1, Outcome::Succeeded) == Transition::Applied);
            CHECK(model.recordStage("stage", 1, Outcome::Failed, {}, {}, "index locked") == Transition::Applied);
            model.recordRecoveryFailure("recovery", "store unavailable");
            const auto entries = handoffEntries(model.handoff());
            for (const auto *id : {"write", "stage", "recovery"}) {
                const auto entry = entries.value(id);
                CHECK(entry.value("category").toString() == "failed");
                CHECK(entry.value("icon").toString() == "dialog-error");
                CHECK(icon(id) == "dialog-error");
                CHECK(role(model, SessionModel::StatusRole, model.rowOf(id)) == entry.value("status"));
            }
            CHECK(role(model, SessionModel::StatusRole, model.rowOf("write")).toString() == "Repository write failed");
            CHECK(role(model, SessionModel::StatusRole, model.rowOf("recovery")).toString()
                  == "Recovery requires attention; work remains outstanding");
        });
        suite.run("rows carry one status line, toned chips, a summary and the escaped path", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            TestSession model(directory.path() + "/recovery");
            auto choices = draft();
            choices.decisions["left"] = {};
            CHECK(model.appendFile(sessionFile("undecided", choices)));
            CHECK(model.appendFile(sessionFile("published")));
            resolve(model, "published");
            CHECK(model.appendFile(FileSession("outside", "diagram.png", {}, Support::OutsideApp, "image content")));
            CHECK(model.appendFile(FileSession("unreadable", "broken.txt", {}, Support::Unreadable, {})));
            const std::string bytes = std::string(":literal\t") + static_cast<char>(0xff);
            CHECK(model.appendFile(FileSession("bytes", bytes, draft())));
            const std::vector<std::string> displayOrder{"undecided", "bytes", "published", "outside", "unreadable"};
            for (int row = 0; row < static_cast<int>(displayOrder.size()); ++row) {
                const auto &expected = displayOrder[static_cast<std::size_t>(row)];
                CHECK(model.rowOf(expected) == row);
                CHECK(model.fileAt(row)->id() == expected);
                CHECK(role(model, SessionModel::FileIdRole, row).toString().toStdString() == expected);
            }
            CHECK(model.rowOf("missing") == -1);
            CHECK(model.supportCount(Support::Supported) == 3 && model.supportCount(Support::OutsideApp) == 1
                  && model.supportCount(Support::Unreadable) == 1);
            const auto at = [&](const std::string &id, int value) { return role(model, value, model.rowOf(id)); };

            CHECK(at("undecided", SessionModel::StatusLineRole).toString() == "1 undecided");
            CHECK(at("published", SessionModel::StatusLineRole).toString() == "Resolved");
            CHECK(at("outside", SessionModel::StatusLineRole).toString() == "Handle outside the app");
            CHECK(at("unreadable", SessionModel::StatusLineRole).toString() == "Load error");
            // Views branch on the kind, never on the translated status text.
            CHECK(at("published", SessionModel::StatusKindRole).value<FileStates::StatusKind>()
                  == FileStates::StatusKind::Resolved);
            CHECK(at("unreadable", SessionModel::StatusKindRole).value<FileStates::StatusKind>()
                  == FileStates::StatusKind::Unreadable);
            CHECK(at("undecided", SessionModel::StatusIconRole).toString() == "vcs-conflicting");
            CHECK(at("published", SessionModel::StatusIconRole).toString() == "emblem-checked");
            CHECK(at("outside", SessionModel::StatusIconRole).toString() == "unknown");
            CHECK(at("unreadable", SessionModel::StatusIconRole).toString() == "dialog-error");

            CHECK(at("published", SessionModel::SummaryRole).toString()
                  == "Written and resolved. Undecided: 0. Saved. Reviewed. Resolved.");
            CHECK(at("undecided", SessionModel::SummaryRole).toString()
                  == "Source choices remain undecided. Undecided: 1. Not saved. Not reviewed. Not resolved.");
            const QSet<QString> tones{"neutral", "positive", "negative", "warning"};
            for (int row = 0; row < model.rowCount(); ++row) {
                CHECK(!role(model, SessionModel::SummaryRole, row).toString().contains(QStringLiteral(" \u00b7 ")));
                const auto chips = role(model, SessionModel::ChipsRole, row).toList();
                CHECK(chips.size() >= 5);
                // The first chip is the status sentence, without an icon.
                CHECK(chips.first().toMap().value("text") == role(model, SessionModel::StatusRole, row));
                CHECK(chips.first().toMap().value("icon").toString().isEmpty());
                for (const auto &chip : chips) {
                    const auto fields = chip.toMap();
                    CHECK(fields.size() == 3 && !fields.value("text").toString().isEmpty());
                    CHECK(tones.contains(fields.value("tone").toString()));
                }
            }
            const auto texts = [&](const std::string &id) {
                QStringList result;
                for (const auto &chip : at(id, SessionModel::ChipsRole).toList())
                    result << chip.toMap().value("text").toString();
                return result;
            };
            CHECK(texts("published")
                  == QStringList({"Written and resolved", "Manual Save checkpoint retained", "Undecided: 0", "Saved",
                                  "Reviewed", "Resolved"}));
            CHECK(texts("undecided")
                  == QStringList({"Source choices remain undecided", "Unsaved draft", "Undecided: 1", "Not saved",
                                  "Not reviewed", "Not resolved"}));
            // A file the app cannot edit has no draft chip.
            CHECK(!texts("outside").join("\n").contains("draft", Qt::CaseInsensitive));
            const auto undecidedChip = at("undecided", SessionModel::ChipsRole).toList().value(2).toMap();
            CHECK(undecidedChip.value("icon").toString() == "dialog-question"
                  && undecidedChip.value("tone").toString() == "negative");

            // One escaper for every surface: the path role, the handoff and the finish blockers.
            const auto escaped = QStringLiteral(":literal\\t\\xff");
            CHECK(at("bytes", SessionModel::PathRole).toString() == escaped);
            CHECK(at("bytes", Qt::DisplayRole).toString() == escaped);
            CHECK(model.displayPath("bytes") == escaped && model.displayPath("missing").isEmpty());
            CHECK(model.file("bytes")->path() == bytes);
            CHECK(handoffEntries(model.handoff()).value("bytes").value("path").toString() == escaped);
            bool blocked = false;
            for (const auto &entry : model.finishBlockers()) {
                if (entry.toMap().value("fileId").toString() != "bytes") continue;
                CHECK(entry.toMap().value("path").toString() == escaped);
                blocked = true;
            }
            CHECK(blocked);
        });
        suite.run("Qt history restores atomic decisions, invalidates review and fences discard callbacks", [] {
            TestSession model;
            CHECK(model.appendFile(FileSession("id", "file", draft())));
            resolve(model, "id");
            auto changed = draft("new result");
            changed.decisions["left"] = {DecisionValue::Ignored, {}};
            CHECK(model.replaceDraft("id", 1, changed) == Transition::Applied);
            CHECK(role(model, SessionModel::CanUndoRole).toBool());
            CHECK(model.file("id")->facts().earlierStaged && !model.canFinish());
            CHECK(model.undo("id", 2) == Transition::Applied);
            CHECK(model.file("id")->current().draft.text == "result\n");
            CHECK(role(model, SessionModel::DecisionsRole).toList()[0].toMap().value("value").toString() == "accepted");
            CHECK(role(model, SessionModel::CanRedoRole).toBool());
            CHECK(role(model, SessionModel::ReviewNeededRole).toBool() && !model.canFinish());
            CHECK(model.redo("id", 3) == Transition::Applied);
            CHECK(model.file("id")->current().draft.text == "new result");
            CHECK(role(model, SessionModel::DecisionsRole).toList()[0].toMap().value("value").toString() == "ignored");
            CHECK(model.discardToCheckpoint("id", 4) == Transition::Applied);
            CHECK(model.recordCheckpoint("id", 2, Outcome::Succeeded) == Transition::StaleRevision);
            CHECK(model.file("id")->checkpoint()->id == 1);
            CHECK(model.discardToStaged("id", 5) == Transition::Applied);
            CHECK(model.canFinish() && role(model, SessionModel::CurrentRevisionRole).toString() == "1");
            CHECK(model.recordCheckpoint("id", 4, Outcome::Failed) == Transition::StaleRevision);
            CHECK(model.canFinish());
        });
        suite.run("replay recomputes markers from the restored text", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            const auto lease = test_support::leasedStore(directory.path() + "/recovery");
            auto file = recoverableFile();
            CHECK(lease->commit(recoveryTransaction(file, RecoveryAction::Open, QUuid::createUuid().toString(), {}))
                      .succeeded);
            auto stripped = savedDraft();
            stripped.text += "left\n=======\nright\n>>>>>>> theirs\n";
            const auto before = file.current().draft.text;
            CHECK(file.replaceDraft(1, stripped) == Transition::Applied);
            CHECK(file.current().draft.unresolvedMarkers == 2);
            const auto transaction =
                recoveryTransaction(file, RecoveryAction::ReplaceDraft, QUuid::createUuid().toString(), before);
            // The record holds no marker count; replay scans the text again.
            CHECK(!transaction.state.value("current").toObject().contains("unresolvedMarkers"));
            CHECK(lease->commit(transaction).succeeded);
            const auto replay = replayRecovery(lease->store());
            CHECK(replay.succeeded && replay.files.size() == 1);
            const auto &restored = replay.files.front();
            CHECK(restored.current().draft.text == stripped.text && restored.current().draft.unresolvedMarkers == 2);
            CHECK(restored.markers().orphans.size() == 2 && restored.markers().blocks.empty());
            const auto blockers = restored.facts().blockers;
            CHECK(std::find(blockers.begin(), blockers.end(), Blocker::UnresolvedMarkers) != blockers.end());
        });
        suite.run("replay restores the text and markers a full decision left", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            TestSession model(directory.path() + "/recovery");
            CHECK(model.appendFile(test_support::markedFile(true)));
            const auto choose = [&](SourceSide side, ChoiceAction action) {
                const auto &file = *model.file("choices");
                const auto &target = test_support::choiceTarget(file, side, ChoiceScope::Section);
                return model.applyChoice(
                    "choices", previewChoice(file, file.current().id, target.source.contributionId, action), true);
            };
            const auto check = [&] {
                const auto &live = *model.file("choices");
                const auto replay = replayedFile(model.recoveryDirectory());
                CHECK(replay.current().draft == live.current().draft && replay.current().id == live.current().id);
                CHECK(replay.markers() == live.markers() && replay.markerSections() == live.markerSections());
                CHECK(!model.hasPendingRecovery());
            };
            CHECK(choose(SourceSide::Stage2, ChoiceAction::Accept) == Transition::Applied);
            CHECK(choose(SourceSide::Stage3, ChoiceAction::Ignore) == Transition::Applied);
            const auto chosen = model.file("choices")->current().draft;
            CHECK(chosen.text.starts_with("header\nleft first\nleft second\nbridge\n<<<<<<< ours\n"));
            CHECK(chosen.unresolvedMarkers == 1);
            check();
            CHECK(model.undo("choices", model.file("choices")->current().id) == Transition::Applied);
            CHECK(model.file("choices")->current().draft.text == test_support::markedText(true));
            check();
            CHECK(model.redo("choices", model.file("choices")->current().id) == Transition::Applied);
            CHECK(model.file("choices")->current().draft == chosen);
            check();
            auto reopened = replayedFile(model.recoveryDirectory());
            CHECK(reopened.undo(reopened.current().id) == Transition::Applied);
            CHECK(reopened.current().draft.text == test_support::markedText(true)
                  && reopened.current().draft.unresolvedMarkers == 2);
        });
        suite.run("a settled file reads ready to mark resolved until undo reopens a choice", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            TestSession model(directory.path() + "/recovery");
            CHECK(model.appendFile(test_support::markedFile()));
            const auto row = [&] { return model.rowOf("choices"); };
            const auto choose = [&](SourceSide side, std::size_t section) {
                const auto &file = *model.file("choices");
                const auto &target = test_support::choiceTarget(file, side, ChoiceScope::Section, section);
                return model.applyChoice("choices",
                                         previewChoice(file, file.current().id, target.source.contributionId,
                                                       ChoiceAction::ResolveUsingSide),
                                         true);
            };
            const auto settled = [&] {
                return role(model, SessionModel::ProgressRole, row()).toMap().value("settled").toBool();
            };
            const auto line = [&] { return role(model, SessionModel::StatusLineRole, row()).toString(); };
            CHECK(!settled() && line() != "Ready to mark resolved");
            CHECK(choose(SourceSide::Stage2, 0) == Transition::Applied);
            CHECK(!settled());
            CHECK(choose(SourceSide::Stage3, 1) == Transition::Applied);
            CHECK(settled() && line() == "Ready to mark resolved");
            // Ready is not reviewed or resolved, and the handoff keeps its existing category.
            CHECK(role(model, SessionModel::StatusRole, row()).toString() == "Current result needs review");
            CHECK(!role(model, SessionModel::ResolvedRole, row()).toBool());
            CHECK(model.undo("choices", model.file("choices")->current().id) == Transition::Applied);
            CHECK(!settled() && line() != "Ready to mark resolved");
            CHECK(model.redo("choices", model.file("choices")->current().id) == Transition::Applied);
            CHECK(settled());
            model.recordRecoveryFailure("choices", "store unavailable");
            CHECK(!settled() && line() != "Ready to mark resolved");
        });
        suite.run("replay derives the marker count against the captured sources", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            TestSession model(directory.path() + "/recovery");
            CHECK(model.appendFile(test_support::headingFile()));
            CHECK(model.file("choices")->current().draft.unresolvedMarkers == 1);
            auto launch = replayedFile(model.recoveryDirectory());
            CHECK(launch.current().draft.unresolvedMarkers == 1
                  && launch.markers() == model.file("choices")->markers());
            auto resolved = model.file("choices")->current().draft;
            resolved.text = "Changes\n=======\nintro\nleft line\ntail\n";
            CHECK(model.replaceDraft("choices", 1, resolved) == Transition::Applied);
            const auto &live = *model.file("choices");
            CHECK(live.current().draft.unresolvedMarkers == 0);
            auto replay = replayedFile(model.recoveryDirectory());
            CHECK(replay.current().draft == live.current().draft && replay.markers() == live.markers());
            CHECK(replay.current().draft.unresolvedMarkers == 0);
            CHECK(replay.beginMarkResolved(replay.current().id, true) == Transition::Applied);
        });
        suite.run("unconfirmed choices and marker blockers are visible without mutation", [] {
            TestSession model;
            auto undecided = draft();
            undecided.decisions["left"] = {};
            CHECK(model.appendFile(FileSession("id", "file", undecided)));
            int changes = 0;
            QObject::connect(&model.rows, &SessionModel::summaryChanged, &model, [&] { ++changes; });
            CHECK(model.beginMarkResolved("id", 1) == Transition::ConfirmationRequired);
            CHECK(changes == 0 && !model.file("id")->reviewed());
            CHECK(role(model, SessionModel::UndecidedRole).toULongLong() == 1);
            CHECK(model.beginMarkResolved("id", 1, true) == Transition::Applied);
            CHECK(model.file("id")->reviewed()->confirmedUndecided);
            undecided.text = "=======\n";
            CHECK(model.replaceDraft("id", 1, undecided) == Transition::Applied);
            CHECK(model.beginMarkResolved("id", 2, true) == Transition::Blocked);
            CHECK(changes == 2);
            CHECK(
                role(model, SessionModel::FinishBlockersRole).toStringList().contains("Known conflict markers remain"));
        });
        for (const auto outcome : {Outcome::Succeeded, Outcome::Failed, Outcome::Uncertain}) {
            suite.run("delayed write stays visible after a newer Save " + std::to_string(static_cast<int>(outcome)),
                      [=] {
                          TestSession model;
                          CHECK(model.appendFile(FileSession("id", "file", draft())));
                          CHECK(model.beginMarkResolved("id", 1) == Transition::Applied);
                          CHECK(model.recordCheckpoint("id", 1, Outcome::Succeeded) == Transition::Applied);
                          CHECK(model.replaceDraft("id", 1, draft("new saved draft")) == Transition::Applied);
                          CHECK(model.recordCheckpoint("id", 2, Outcome::Succeeded) == Transition::Applied);
                          int changes = 0;
                          QObject::connect(&model.rows, &SessionModel::summaryChanged, &model, [&] { ++changes; });
                          CHECK(model.recordWrite("id", 1, outcome, "delayed diagnostic") == Transition::Applied);
                          CHECK(changes == 1);
                          const auto &result = *model.file("id")->writeOutcome();
                          const auto status = outcome == Outcome::Succeeded ? "Written, unresolved"
                              : outcome == Outcome::Failed                  ? "Repository write failed"
                                                           : "Outcome uncertain; verification required";
                          CHECK(result.revision == 1);
                          CHECK(result.outcome == outcome && result.detail == "delayed diagnostic");
                          CHECK(role(model, SessionModel::StatusRole).toString() == status);
                          CHECK(role(model, SessionModel::CurrentRevisionRole).toString() == "2");
                          CHECK(model.file("id")->checkpoint()->id == 2);
                          CHECK(model.file("id")->reviewed()->revision.id == 1);
                          CHECK(!role(model, SessionModel::UnsavedRole).toBool()
                                && role(model, SessionModel::ReviewNeededRole).toBool());
                          CHECK(!model.canFinish() && model.resolvedFiles() == 0 && !model.finishBlockers().isEmpty());
                          const auto &written = model.file("id")->written();
                          CHECK(outcome == Outcome::Succeeded ? written && written->id == 1 : !written);
                      });
        }
        suite.run("manual Save and discard stay distinct from recovery and publication", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            const auto faults = std::make_shared<test_support::SyncFaults>();
            TestSession model(directory.path() + "/recovery", faults);
            CHECK(model.appendFile(recoverableFile()));
            CHECK(model.replaceDraft("recoverable", 1, savedDraft()) == Transition::Applied);
            CHECK(model.file("recoverable")->facts().unsaved && !model.file("recoverable")->checkpoint());
            const auto savedRevision = model.file("recoverable")->current().id;
            CHECK(model.recordCheckpoint("recoverable", savedRevision, Outcome::Succeeded) == Transition::Applied);
            CHECK(model.file("recoverable")->checkpoint()
                  && model.file("recoverable")->checkpoint()->draft == savedDraft());
            CHECK(!model.file("recoverable")->facts().unsaved && !model.file("recoverable")->written()
                  && !model.file("recoverable")->staged() && !model.file("recoverable")->reviewed());
            CHECK(role(model, SessionModel::StatusRole).toString()
                  == "Draft saved in Branch Truce; repository file unchanged");
            CHECK(model.replaceDraft("recoverable", savedRevision, unsavedDraft()) == Transition::Applied);
            const auto discardedRevision = model.file("recoverable")->current().id;
            CHECK(model.file("recoverable")->facts().unsaved
                  && model.file("recoverable")->checkpoint()->draft == savedDraft());
            {
                const test_support::ArmedFault fault(*faults, test_support::SyncFaults::Point::JournalSync);
                CHECK(model.discardToCheckpoint("recoverable", discardedRevision) == Transition::Blocked);
                CHECK(faults->failures == 1);
            }
            CHECK(model.hasPendingRecovery() && model.file("recoverable")->current().draft == unsavedDraft());
            CHECK(role(model, SessionModel::RecoveryStateRole).value<FileStates::RecoveryState>()
                  == FileStates::RecoveryState::Failed);
            CHECK(model.retryPendingRecovery("recoverable"));
            CHECK(model.file("recoverable")->current().draft == savedDraft());
            CHECK(!model.file("recoverable")->facts().unsaved && !model.file("recoverable")->written()
                  && !model.file("recoverable")->staged());
            storage::RecoveryStore store(model.recoveryDirectory());
            auto read = store.read();
            CHECK(read.succeeded && read.transactions.back().state.value("action") == "discard-to-checkpoint");
            const auto drafts = test_support::recordedDrafts(store);
            checkDraft(drafts.back(), savedDraft());
            bool sawManualSave = false;
            bool sawLaterEdit = false;
            for (qsizetype index = 0; index < read.transactions.size(); ++index) {
                const auto &state = read.transactions[index].state;
                if (state.value("action") == "manual-save") {
                    sawManualSave = true;
                    CHECK(state.value("written").isNull() && state.value("staged").isNull()
                          && state.value("reviewed").isNull());
                    // Save checkpoints the current draft, which the record holds.
                    CHECK(state.value("checkpoint").toObject().value("id")
                          == state.value("current").toObject().value("id"));
                    checkDraft(drafts[static_cast<std::size_t>(index)], savedDraft());
                } else if (sawManualSave && state.value("action") == "replace-draft")
                    sawLaterEdit = true;
            }
            CHECK(sawManualSave && sawLaterEdit);
            const auto restored = replayedFile(model.recoveryDirectory());
            CHECK(restored.current().draft == savedDraft() && restored.current().draft != unsavedDraft());
            CHECK(restored.checkpoint() && restored.checkpoint()->draft == savedDraft());
            CHECK(!restored.facts().unsaved && !restored.written() && !restored.staged());

            CHECK(model.replaceDraft("recoverable", model.file("recoverable")->current().id, unsavedDraft())
                  == Transition::Applied);
            const auto edited = *model.file("recoverable");
            {
                const test_support::ArmedFault fault(*faults, test_support::SyncFaults::Point::JournalSync);
                CHECK(model.recordCheckpoint("recoverable", edited.current().id, Outcome::Succeeded)
                      == Transition::Blocked);
                CHECK(faults->failures == 1);
            }
            CHECK(model.file("recoverable")->current().id == edited.current().id);
            CHECK(model.file("recoverable")->current().draft == unsavedDraft());
            CHECK(model.file("recoverable")->checkpoint()->draft == savedDraft());
            CHECK(model.file("recoverable")->facts().unsaved);
            CHECK(role(model, SessionModel::StatusRole).toString() == "Draft save failed");
            CHECK(role(model, SessionModel::UnsavedRole).toBool());
            CHECK(role(model, SessionModel::RecoveryStateRole).value<FileStates::RecoveryState>()
                  == FileStates::RecoveryState::SaveFailed);
            CHECK(model.file("recoverable")->current().draft.text == unsavedDraft().text);
            CHECK(role(model, SessionModel::FinishBlockersRole)
                      .toStringList()
                      .contains("Draft save failed; the edited result is still available"));
            CHECK(!model.file("recoverable")->written() && !model.file("recoverable")->staged());
            CHECK(model.discardToCheckpoint("recoverable", edited.current().id) == Transition::Blocked);
            CHECK(model.retryPendingRecovery("recoverable"));
            CHECK(!model.hasPendingRecovery() && model.file("recoverable")->checkpoint()->draft == unsavedDraft());
            CHECK(!model.file("recoverable")->facts().unsaved && !model.file("recoverable")->written()
                  && !model.file("recoverable")->staged());
            CHECK(role(model, SessionModel::StatusRole).toString()
                  == "Draft saved in Branch Truce; repository file unchanged");
            read = storage::RecoveryStore(model.recoveryDirectory()).read();
            CHECK(read.succeeded && read.transactions.back().state.value("action") == "manual-save");
            CHECK(read.transactions.back().state.value("written").isNull()
                  && read.transactions.back().state.value("staged").isNull());
            const auto retried = replayedFile(model.recoveryDirectory());
            CHECK(retried.current().draft == unsavedDraft() && retried.checkpoint()
                  && retried.checkpoint()->draft == unsavedDraft() && !retried.written() && !retried.staged());
        });
        suite.run("failed Save and discard reuse one transaction and block the session until retry", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            const auto faults = std::make_shared<test_support::SyncFaults>();
            TestSession model(directory.path() + "/recovery", faults);
            CHECK(model.appendFile(recoverableFile()));
            CHECK(model.appendFile(FileSession("other", "other.txt", draft())));
            CHECK(!model.hasPendingRecovery());
            CHECK(model.replaceDraft("recoverable", 1, savedDraft()) == Transition::Applied);
            const auto savedRevision = model.file("recoverable")->current().id;
            CHECK(model.recordCheckpoint("recoverable", savedRevision, Outcome::Succeeded) == Transition::Applied);
            CHECK(model.replaceDraft("recoverable", savedRevision, unsavedDraft()) == Transition::Applied);
            const auto discardedRevision = model.file("recoverable")->current().id;
            const auto otherRevision = model.file("other")->current().id;
            const auto otherDraft = model.file("other")->current().draft;
            storage::RecoveryStore store(model.recoveryDirectory());
            qsizetype records = 0;
            QString request;
            {
                const test_support::ArmedFault fault(*faults, test_support::SyncFaults::Point::JournalSync);
                CHECK(model.discardToCheckpoint("recoverable", discardedRevision) == Transition::Blocked);
                CHECK(faults->failures == 1);
            }
            CHECK(model.hasPendingRecovery() && model.file("recoverable")->current().draft == unsavedDraft());
            CHECK(model.pendingDraft("recoverable") && *model.pendingDraft("recoverable") == savedDraft());
            CHECK(model.file("recoverable")->checkpoint()->draft == savedDraft());
            CHECK(role(model, SessionModel::RecoveryStateRole).value<FileStates::RecoveryState>()
                  == FileStates::RecoveryState::Failed);
            CHECK(model.replaceDraft("other", otherRevision, draft("blocked other")) == Transition::Blocked);
            CHECK(model.undo("recoverable", discardedRevision) == Transition::Blocked);
            CHECK(model.recordCheckpoint("recoverable", discardedRevision, Outcome::Succeeded) == Transition::Blocked);
            CHECK(model.file("other")->current().id == otherRevision
                  && model.file("other")->current().draft == otherDraft);
            CHECK(model.file("recoverable")->current().draft == unsavedDraft());
            {
                const auto pending = store.read();
                CHECK(pending.succeeded
                      && pending.transactions.back().state.value("action") == "discard-to-checkpoint");
                records = pending.transactions.size();
                request = pending.transactions.back().id;
                checkDraft(test_support::recordedDrafts(store).back(), savedDraft());
            }
            CHECK(model.retryPendingRecovery("recoverable"));
            CHECK(!model.hasPendingRecovery() && model.file("recoverable")->current().draft == savedDraft());
            CHECK(model.file("recoverable")->current().draft.decisions.at("right").value == DecisionValue::Ignored);
            CHECK(!model.file("recoverable")->written() && !model.file("recoverable")->staged());
            {
                const auto committed = store.read();
                CHECK(committed.succeeded && committed.transactions.size() == records);
                CHECK(committed.transactions.back().id == request);
                CHECK(committed.transactions.back().state.value("action") == "discard-to-checkpoint");
                CHECK(committed.transactions.back().state.value("written").isNull()
                      && committed.transactions.back().state.value("staged").isNull());
                checkDraft(test_support::recordedDrafts(store).back(), savedDraft());
            }
            CHECK(model.replaceDraft("other", otherRevision, draft("other stayed editable")) == Transition::Applied);

            CHECK(model.replaceDraft("recoverable", model.file("recoverable")->current().id, unsavedDraft())
                  == Transition::Applied);
            const auto editedRevision = model.file("recoverable")->current().id;
            const auto otherEdited = model.file("other")->current();
            {
                const test_support::ArmedFault fault(*faults, test_support::SyncFaults::Point::JournalSync);
                CHECK(model.recordCheckpoint("recoverable", editedRevision, Outcome::Succeeded) == Transition::Blocked);
                CHECK(faults->failures == 1);
            }
            CHECK(model.file("recoverable")->current().id == editedRevision);
            CHECK(model.file("recoverable")->current().draft == unsavedDraft());
            CHECK(model.pendingDraft("recoverable") && *model.pendingDraft("recoverable") == unsavedDraft());
            CHECK(model.file("recoverable")->checkpoint()->draft == savedDraft());
            CHECK(model.file("recoverable")->facts().unsaved && !model.file("recoverable")->written()
                  && !model.file("recoverable")->staged());
            CHECK(role(model, SessionModel::StatusRole).toString() == "Draft save failed");
            CHECK(role(model, SessionModel::RecoveryStateRole).value<FileStates::RecoveryState>()
                  == FileStates::RecoveryState::SaveFailed);
            CHECK(model.file("recoverable")->current().draft.text == unsavedDraft().text);
            CHECK(model.replaceDraft("other", otherEdited.id, draft("must stay blocked")) == Transition::Blocked);
            CHECK(model.undo("recoverable", editedRevision) == Transition::Blocked);
            CHECK(model.discardToCheckpoint("recoverable", editedRevision) == Transition::Blocked);
            CHECK(model.file("other")->current().id == otherEdited.id
                  && model.file("other")->current().draft == otherEdited.draft);
            CHECK(model.file("recoverable")->current().draft == unsavedDraft());
            {
                const auto pending = store.read();
                CHECK(pending.succeeded && pending.transactions.back().state.value("action") == "manual-save");
                records = pending.transactions.size();
                request = pending.transactions.back().id;
                CHECK(pending.transactions.back().state.value("written").isNull()
                      && pending.transactions.back().state.value("staged").isNull());
                checkDraft(test_support::recordedDrafts(store).back(), unsavedDraft());
            }
            CHECK(model.retryPendingRecovery("recoverable"));
            CHECK(!model.hasPendingRecovery() && model.file("recoverable")->current().draft == unsavedDraft());
            CHECK(model.file("recoverable")->checkpoint()
                  && model.file("recoverable")->checkpoint()->draft == unsavedDraft());
            CHECK(model.file("recoverable")->current().draft.decisions.at("right").value == DecisionValue::Accepted);
            CHECK(!model.file("recoverable")->facts().unsaved && !model.file("recoverable")->written()
                  && !model.file("recoverable")->staged());
            CHECK(role(model, SessionModel::StatusRole).toString()
                  == "Draft saved in Branch Truce; repository file unchanged");
            {
                const auto committed = store.read();
                CHECK(committed.succeeded && committed.transactions.size() == records);
                CHECK(committed.transactions.back().id == request);
                CHECK(committed.transactions.back().state.value("action") == "manual-save");
                CHECK(committed.transactions.back().state.value("written").isNull()
                      && committed.transactions.back().state.value("staged").isNull());
                checkDraft(test_support::recordedDrafts(store).back(), unsavedDraft());
            }
            CHECK(model.replaceDraft("other", otherEdited.id, draft("unblocked after retry")) == Transition::Applied);
            const auto replay = replayRecovery(storage::RecoveryStore(model.recoveryDirectory()));
            CHECK(replay.succeeded && replay.files.size() == 2);
            const FileSession *restored = nullptr;
            for (const auto &file : replay.files)
                if (file.id() == "recoverable") restored = &file;
            CHECK(restored != nullptr && restored->current().draft == unsavedDraft());
            CHECK(restored->current().draft != savedDraft() && restored->checkpoint()
                  && restored->checkpoint()->draft == unsavedDraft());
            CHECK(!restored->facts().unsaved && !restored->written() && !restored->staged());
        });
        return suite.report();
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // The English catalog supplies the plural forms these checks read.
    branch_truce::installTranslations(app);
    if (app.arguments().size() == 3 && app.arguments()[1] == "--acknowledge-and-crash")
        return acknowledgeAndCrash(app.arguments()[2]);
    return SessionModelTests{}.run();
}
