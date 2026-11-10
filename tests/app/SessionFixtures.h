// Sessions, drafts and recovery records the session model, recovery and selected-file suites share.
#pragma once

#include "TestHarness.h"
#include "TestSession.h"
#include "app/Selection.h"
#include "app/SessionModel.h"
#include "fixtures/ChoiceFile.h"
#include "recovery/RecoveryRestore.h"
#include "storage/RecoveryStore.h"
#include <QJsonValue>
#include <QMap>
#include <QString>
#include <QUuid>
#include <QVariantMap>
#include <stdexcept>
#include <string>

namespace session_fixtures {
using namespace branch_truce;
using test_support::TestSession;

struct NoSelectionGuard : SelectionGuard {
    [[nodiscard]] bool holdsSelection() const override { return false; }
};
inline constexpr auto recoverySessionId = "42c4588c-5cc2-4fba-964f-3e98a066e671";

inline Draft draft(std::string text = "result\n")
{
    return {std::move(text), {{"left", {DecisionValue::Accepted, 1}}}, 0};
}
inline QVariant role(const TestSession &model, int value, int row = 0) { return model.data(model.index(row), value); }
inline void resolve(TestSession &model, const std::string &id)
{
    const auto revision = model.file(id)->current().id;
    CHECK(model.beginMarkResolved(id, revision) == Transition::Applied);
    CHECK(model.recordCheckpoint(id, revision, Outcome::Succeeded) == Transition::Applied);
    CHECK(model.recordWrite(id, revision, Outcome::Succeeded) == Transition::Applied);
    CHECK(model.recordStage(id, revision, Outcome::Succeeded, "verified-object", "100644") == Transition::Applied);
}
inline FileSession recoverableFile()
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
    return FileSession("recoverable", captured.path, draft(captured.launchResult.text.utf8), {.snapshots = captured});
}
// A supported file of the recoverable session under another identity.
inline FileSession sessionFile(const std::string &id, Draft value = draft())
{
    auto captured = *recoverableFile().snapshots();
    captured.path = id + ".txt";
    captured.launchResult.text = test_support::choiceText(value.text);
    return FileSession(id, captured.path, std::move(value), {.snapshots = captured});
}
// fileId -> handoff entry; every file must appear in exactly one group.
inline QMap<QString, QVariantMap> handoffEntries(const QVariantMap &summary)
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
inline QByteArray payload(const storage::RecoveryStore &store, const QJsonValue &reference)
{
    QString error;
    const auto bytes = store.payload(reference.toString().toLatin1(), &error);
    CHECK(error.isEmpty());
    return bytes;
}
// A draft a record holds: its text and decisions. Records do not hold the marker count.
inline void checkDraft(const Draft &recorded, const Draft &expected)
{
    CHECK(recorded.text == expected.text);
    CHECK(recorded.decisions == expected.decisions);
}
inline Draft savedDraft()
{
    auto value = draft("saved combined \xc3\xa9\n");
    value.decisions["right"] = {DecisionValue::Ignored, {}};
    return value;
}
inline Draft unsavedDraft()
{
    auto value = savedDraft();
    value.text += "<<<<<<< ours\nacknowledged unsaved \xf0\x9f\x98\x80\n=======\nother\n>>>>>>> theirs\n";
    value.decisions["right"] = {DecisionValue::Accepted, 2};
    value.unresolvedMarkers = 1;
    return value;
}
inline Draft finalDecision()
{
    auto value = unsavedDraft();
    value.decisions["later contribution"] = {DecisionValue::Ignored, {}};
    return value;
}
inline FileSession replayedFile(const QString &directory)
{
    const auto replay = replayRecovery(storage::RecoveryStore(directory));
    if (!replay.succeeded) throw std::runtime_error("recovery replay: " + replay.error.toStdString());
    CHECK(replay.succeeded && replay.error.isEmpty() && replay.files.size() == 1);
    return replay.files.front();
}
inline void checkRecoveredHistory(TestSession &model, const Draft &before, const Draft &chosen)
{
    const auto captures = *model.file("choices")->snapshots();
    const auto check = [&](const Draft &expected) {
        const auto replay = replayedFile(model.recoveryDirectory());
        CHECK(replay.current().draft == expected && replay.current().id == model.file("choices")->current().id);
        CHECK(*replay.snapshots() == captures && replay.publication().checkpoint()
              && replay.revision(*replay.publication().checkpoint())->draft == before);
        CHECK(!replay.publication().reviewed() && !replay.publication().written() && !replay.publication().staged()
              && !replay.facts().resolved);
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

inline QString recordIntent(TestSession &model, const std::string &id, MutationKind kind)
{
    const auto mutation = QUuid::createUuid().toString(QUuid::WithoutBraces);
    CHECK(model.commitRecord(mutationRecoveryTransaction(*model.file(id), {mutation, kind, {}})).succeeded);
    return mutation;
}

inline void preparePublication(TestSession &model, const std::string &id, bool staging)
{
    const auto revision = model.file(id)->current().id;
    CHECK(model.beginMarkResolved(id, revision, true) == Transition::Applied);
    CHECK(model.recordCheckpoint(id, revision, Outcome::Succeeded) == Transition::Applied);
    if (staging) CHECK(model.recordWrite(id, revision, Outcome::Succeeded) == Transition::Applied);
}

inline void completePublication(TestSession &model, const std::string &id, bool staging)
{
    const auto revision = model.file(id)->current().id;
    CHECK((staging ? model.recordStage(id, revision, Outcome::Succeeded, "verified-object", "100644")
                   : model.recordWrite(id, revision, Outcome::Succeeded))
          == Transition::Applied);
}

// Readable case names for the enums a table of cases runs over.
inline std::string nameOf(ChoiceScope scope) { return scope == ChoiceScope::Line ? "line" : "section"; }
inline std::string nameOf(ChoiceAction action)
{
    switch (action) {
    case ChoiceAction::Accept: return "accept";
    case ChoiceAction::Ignore: return "ignore";
    case ChoiceAction::ResolveUsingSide: return "resolve using the side";
    case ChoiceAction::CombineLeftFirst: return "combine left first";
    case ChoiceAction::CombineRightFirst: return "combine right first";
    }
    return "unknown action";
}
inline std::string nameOf(NonConflictingScope scope)
{
    return scope == NonConflictingScope::Left ? "left" : scope == NonConflictingScope::Right ? "right" : "both sides";
}
inline std::string nameOf(SourceSide side)
{
    return side == SourceSide::Stage2 ? "left" : side == SourceSide::Stage3 ? "right" : "earlier";
}
inline std::string nameOf(Outcome outcome)
{
    return outcome == Outcome::Succeeded ? "succeeded" : outcome == Outcome::Failed ? "failed" : "uncertain";
}
}
