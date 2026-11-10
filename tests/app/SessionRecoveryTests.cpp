// The session store's recovery: each command's journal record, replay after a restart or a crash,
// failed commits that keep their input for a retry, and mutation intents and their outcomes.
#include "SessionFixtures.h"
#include "TestHarness.h"
#include "TestSession.h"
#include "app/Translations.h"
#include "fixtures/ApplicationIdentity.h"
#include "fixtures/EventLoop.h"
#include "fixtures/RecoveryJournal.h"
#include "fixtures/SyncFaults.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QMap>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <cstdlib>
#include <unistd.h>

using namespace branch_truce;
using namespace session_fixtures;

namespace {
// Types `text` into the file one character at a time from byte `at`, as the editor sends keys.
void typeInto(TestSession &model, const std::string &id, std::size_t at, const std::string &text)
{
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto revision = model.file(id)->current().id;
        CHECK(model.typeResult(id, revision, {{at + index, at + index}, text.substr(index, 1)}) == Transition::Applied);
    }
}

qsizetype records(const TestSession &model)
{
    return storage::RecoveryStore(model.recoveryDirectory()).read().transactions.size();
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
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // The English catalog supplies the plural forms these checks read.
    branch_truce::installTranslations(app);
    if (app.arguments().size() == 3 && app.arguments()[1] == "--acknowledge-and-crash")
        return acknowledgeAndCrash(app.arguments()[2]);
    test_support::Suite suite("SessionRecoveryTests");
    for (const auto scope : {ChoiceScope::Line, ChoiceScope::Section}) {
        for (const auto action : {ChoiceAction::CombineLeftFirst, ChoiceAction::CombineRightFirst}) {
            suite.run(
                "ordered combination undo and redo recover as complete transactions: " + nameOf(scope) + ", "
                    + nameOf(action),
                [=] {
                    QTemporaryDir directory;
                    CHECK(directory.isValid());
                    TestSession model(directory.path() + "/recovery");
                    CHECK(model.appendFile(test_support::combinationFile(scope)));
                    CHECK(model.editResult("choices", 1, {{0, 0}, "manual prefix\n"}) == Transition::Applied);
                    CHECK(model.recordCheckpoint("choices", 2, Outcome::Succeeded) == Transition::Applied);
                    const auto before = model.file("choices")->current().draft;
                    const auto target = test_support::choiceTarget(*model.file("choices"), SourceSide::Stage2, scope);
                    const auto preview = previewChoice(*model.file("choices"), 2, target.source.contributionId, action);
                    const auto count = storage::RecoveryStore(model.recoveryDirectory()).read().transactions.size();
                    CHECK(model.applyChoice("choices", preview) == Transition::ConfirmationRequired);
                    CHECK(storage::RecoveryStore(model.recoveryDirectory()).read().transactions.size() == count);
                    CHECK(model.applyChoice("choices", preview, true) == Transition::Applied);
                    CHECK(storage::RecoveryStore(model.recoveryDirectory()).read().transactions.size() == count + 1);
                    const auto chosen = model.file("choices")->current().draft;
                    CHECK(chosen != before && chosen.text.find(preview.replacement) != std::string::npos);
                    checkRecoveredHistory(model, before, chosen);
                });
        }
    }
    for (const auto scope : {NonConflictingScope::Left, NonConflictingScope::Right, NonConflictingScope::Both}) {
        suite.run("file batch undo and redo recover together with protected manual work: " + nameOf(scope), [=] {
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
        suite.run("whole-file replacement undo and redo recover text and decisions: " + nameOf(side), [=] {
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
        suite.run(std::string("a failed compound command retains one atomic recovery candidate: ")
                      + (operation == 0       ? "combination"
                             : operation == 1 ? "file batch"
                                              : "whole file"),
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
                      CHECK(model.file("choices")->current().id == 1
                            && model.file("choices")->current().draft == before);
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
                CHECK(replay.mutations[i].completed && replay.mutations[i].id == identities[static_cast<qsizetype>(i)]);
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
        CHECK(model.appendFile(
            FileSession("other", captures.path, draft(captures.launchResult.text.utf8), {.snapshots = captures})));
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
        suite.run("command undo and redo each replay complete text and decisions: " + nameOf(action), [=] {
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
                CHECK(replay.publication().checkpoint()
                      && replay.revision(*replay.publication().checkpoint())->draft == before);
                CHECK(!replay.publication().reviewed() && !replay.publication().written()
                      && !replay.publication().staged() && !replay.facts().resolved);
            };
            CHECK(chosen != before);
            check(chosen);
            CHECK(model.undo("choices", model.file("choices")->current().id) == Transition::Applied);
            check(before);
            CHECK(model.redo("choices", model.file("choices")->current().id) == Transition::Applied);
            check(chosen);
            auto reopened = replayedFile(model.recoveryDirectory());
            CHECK(reopened.undo(reopened.current().id) == Transition::Applied && reopened.current().draft == before);
            CHECK(reopened.redo(reopened.current().id) == Transition::Applied && reopened.current().draft == chosen);
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
        suite.run(
            std::string("durable discard replays instead of the abandoned draft: ")
                + (checkpoint == 0       ? "no Save"
                       : checkpoint == 1 ? "to the Save"
                                         : "to the staged result"),
            [=] {
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
                CHECK(restored.publication().checkpoint().has_value() == (checkpoint != 0));
                CHECK(restored.facts().resolved == (checkpoint == 2));
                CHECK(replayedFile(location).current().draft == retained);
            });
    }
    for (const int corruption : {0, 1, 2}) {
        suite.run(
            std::string("an invalid record rejects the entire replay: ")
                + (corruption == 0       ? "schema"
                       : corruption == 1 ? "session identity"
                                         : "file identity"),
            [=] {
                QTemporaryDir directory;
                CHECK(directory.isValid());
                const auto lease = test_support::leasedStore(directory.path() + "/recovery");
                const auto &store = lease->store();
                auto file = recoverableFile();
                CHECK(lease->commit(recoveryTransaction(file, RecoveryAction::Open, QUuid::createUuid().toString(), {}))
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
        "an immediate process exit after acknowledgment preserves edits decisions captures and Save separately", [] {
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
            CHECK(restored.publication().checkpoint()
                  && restored.revision(*restored.publication().checkpoint())->draft == savedDraft());
            CHECK(restored.facts().unsaved && !restored.facts().resolved);
            CHECK(!restored.publication().reviewed() && !restored.publication().written()
                  && !restored.publication().staged());
            CHECK(restored.current().draft.decisions.size() == 3);
            const auto recovered = reopened.read();
            CHECK(recovered.succeeded && !recovered.incompleteTail && recovered.transactions.size() == 5);
            const auto last = recovered.transactions.back().state;
            const auto current = last.value("current").toObject();
            CHECK(last.value("schema").toInt() == 3 && !recovered.transactions.back().id.isEmpty());
            CHECK(current.value("id") == "4" && current.value("result") == "3");
            checkDraft(test_support::recordedDrafts(reopened).back(), finalDecision());
            const auto checkpoint = last.value("checkpoint").toObject();
            CHECK(checkpoint.value("id") == "2");
            CHECK(last.value("reviewed").isNull() && last.value("written").isNull() && last.value("staged").isNull());
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
            CHECK(payload(reopened, launch.value("text").toObject().value("bytes")) == "\xef\xbb\xbfmanual launch\r\n");
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
        CHECK(model.file("recoverable")->current().id == 1 && model.file("recoverable")->current().draft == original);
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
        CHECK(model.file("recoverable")->facts().unsaved && !model.file("recoverable")->publication().checkpoint());
        const auto after = store.read();
        CHECK(after.succeeded && after.transactions.size() == 2 && after.transactions.back().id == request);
        checkDraft(test_support::recordedDrafts(store).back(), unsavedDraft());
        CHECK(model.undo("recoverable", 2) == Transition::Applied);
        CHECK(model.file("recoverable")->current().draft == original);
    });
    suite.run("recovery root ignores the organization name and finds earlier stores", [] {
        test_support::ScopedApplicationIdentity identity("Branch Truce", {});
        const auto root =
            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Branch Truce/recovery/v1";
        // The earlier build's root: AppLocalDataLocation with no organization name.
        const auto earlier = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/recovery/v1";
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
    suite.run("replay derives the marker count against the captured sources", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        TestSession model(directory.path() + "/recovery");
        CHECK(model.appendFile(test_support::headingFile()));
        CHECK(model.file("choices")->current().draft.unresolvedMarkers == 1);
        auto launch = replayedFile(model.recoveryDirectory());
        CHECK(launch.current().draft.unresolvedMarkers == 1 && launch.markers() == model.file("choices")->markers());
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
    suite.run("manual Save and discard stay distinct from recovery and publication", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        const auto faults = std::make_shared<test_support::SyncFaults>();
        TestSession model(directory.path() + "/recovery", faults);
        CHECK(model.appendFile(recoverableFile()));
        CHECK(model.replaceDraft("recoverable", 1, savedDraft()) == Transition::Applied);
        CHECK(model.file("recoverable")->facts().unsaved && !model.file("recoverable")->publication().checkpoint());
        const auto savedRevision = model.file("recoverable")->current().id;
        CHECK(model.recordCheckpoint("recoverable", savedRevision, Outcome::Succeeded) == Transition::Applied);
        CHECK(model.file("recoverable")->publication().checkpoint()
              && model.file("recoverable")->revision(*model.file("recoverable")->publication().checkpoint())->draft
                  == savedDraft());
        CHECK(!model.file("recoverable")->facts().unsaved && !model.file("recoverable")->publication().written()
              && !model.file("recoverable")->publication().staged()
              && !model.file("recoverable")->publication().reviewed());
        CHECK(role(model, SessionModel::StatusRole).toString()
              == "Draft saved in Branch Truce; repository file unchanged");
        CHECK(model.replaceDraft("recoverable", savedRevision, unsavedDraft()) == Transition::Applied);
        const auto discardedRevision = model.file("recoverable")->current().id;
        CHECK(model.file("recoverable")->facts().unsaved
              && model.file("recoverable")->revision(*model.file("recoverable")->publication().checkpoint())->draft
                  == savedDraft());
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
        CHECK(!model.file("recoverable")->facts().unsaved && !model.file("recoverable")->publication().written()
              && !model.file("recoverable")->publication().staged());
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
        CHECK(restored.publication().checkpoint()
              && restored.revision(*restored.publication().checkpoint())->draft == savedDraft());
        CHECK(!restored.facts().unsaved && !restored.publication().written() && !restored.publication().staged());

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
        CHECK(model.file("recoverable")->revision(*model.file("recoverable")->publication().checkpoint())->draft
              == savedDraft());
        CHECK(model.file("recoverable")->facts().unsaved);
        CHECK(role(model, SessionModel::StatusRole).toString() == "Draft save failed");
        CHECK(role(model, SessionModel::UnsavedRole).toBool());
        CHECK(role(model, SessionModel::RecoveryStateRole).value<FileStates::RecoveryState>()
              == FileStates::RecoveryState::SaveFailed);
        CHECK(model.file("recoverable")->current().draft.text == unsavedDraft().text);
        CHECK(role(model, SessionModel::FinishBlockersRole)
                  .toStringList()
                  .contains("Draft save failed; the edited result is still available"));
        CHECK(!model.file("recoverable")->publication().written()
              && !model.file("recoverable")->publication().staged());
        CHECK(model.discardToCheckpoint("recoverable", edited.current().id) == Transition::Blocked);
        CHECK(model.retryPendingRecovery("recoverable"));
        CHECK(!model.hasPendingRecovery()
              && model.file("recoverable")->revision(*model.file("recoverable")->publication().checkpoint())->draft
                  == unsavedDraft());
        CHECK(!model.file("recoverable")->facts().unsaved && !model.file("recoverable")->publication().written()
              && !model.file("recoverable")->publication().staged());
        CHECK(role(model, SessionModel::StatusRole).toString()
              == "Draft saved in Branch Truce; repository file unchanged");
        read = storage::RecoveryStore(model.recoveryDirectory()).read();
        CHECK(read.succeeded && read.transactions.back().state.value("action") == "manual-save");
        CHECK(read.transactions.back().state.value("written").isNull()
              && read.transactions.back().state.value("staged").isNull());
        const auto retried = replayedFile(model.recoveryDirectory());
        CHECK(retried.current().draft == unsavedDraft() && retried.publication().checkpoint()
              && retried.revision(*retried.publication().checkpoint())->draft == unsavedDraft()
              && !retried.publication().written() && !retried.publication().staged());
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
        CHECK(model.file("recoverable")->revision(*model.file("recoverable")->publication().checkpoint())->draft
              == savedDraft());
        CHECK(role(model, SessionModel::RecoveryStateRole).value<FileStates::RecoveryState>()
              == FileStates::RecoveryState::Failed);
        CHECK(model.replaceDraft("other", otherRevision, draft("blocked other")) == Transition::Blocked);
        CHECK(model.undo("recoverable", discardedRevision) == Transition::Blocked);
        CHECK(model.recordCheckpoint("recoverable", discardedRevision, Outcome::Succeeded) == Transition::Blocked);
        CHECK(model.file("other")->current().id == otherRevision && model.file("other")->current().draft == otherDraft);
        CHECK(model.file("recoverable")->current().draft == unsavedDraft());
        {
            const auto pending = store.read();
            CHECK(pending.succeeded && pending.transactions.back().state.value("action") == "discard-to-checkpoint");
            records = pending.transactions.size();
            request = pending.transactions.back().id;
            checkDraft(test_support::recordedDrafts(store).back(), savedDraft());
        }
        CHECK(model.retryPendingRecovery("recoverable"));
        CHECK(!model.hasPendingRecovery() && model.file("recoverable")->current().draft == savedDraft());
        CHECK(model.file("recoverable")->current().draft.decisions.at("right").value == DecisionValue::Ignored);
        CHECK(!model.file("recoverable")->publication().written()
              && !model.file("recoverable")->publication().staged());
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
        CHECK(model.file("recoverable")->revision(*model.file("recoverable")->publication().checkpoint())->draft
              == savedDraft());
        CHECK(model.file("recoverable")->facts().unsaved && !model.file("recoverable")->publication().written()
              && !model.file("recoverable")->publication().staged());
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
        CHECK(model.file("recoverable")->publication().checkpoint()
              && model.file("recoverable")->revision(*model.file("recoverable")->publication().checkpoint())->draft
                  == unsavedDraft());
        CHECK(model.file("recoverable")->current().draft.decisions.at("right").value == DecisionValue::Accepted);
        CHECK(!model.file("recoverable")->facts().unsaved && !model.file("recoverable")->publication().written()
              && !model.file("recoverable")->publication().staged());
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
        CHECK(restored->current().draft != savedDraft() && restored->publication().checkpoint()
              && restored->revision(*restored->publication().checkpoint())->draft == unsavedDraft());
        CHECK(!restored->facts().unsaved && !restored->publication().written() && !restored->publication().staged());
    });
    suite.run("typed edits apply at once, one undo takes back their burst and one record holds it", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        TestSession model(directory.path() + "/recovery");
        CHECK(model.appendFile(test_support::choiceFile()));
        const auto launch = model.file("choices")->current();
        const auto before = records(model);
        typeInto(model, "choices", 0, "abc");
        const auto typed = model.file("choices")->current();
        CHECK(typed.id == launch.id + 3 && typed.draft.text == "abc" + launch.draft.text);
        // Applied, aligned later and not yet acknowledged.
        CHECK(model.file("choices")->pendingAlignment() != nullptr);
        CHECK(model.recording("choices") && model.holdsUserWork() && records(model) == before);
        CHECK(role(model, SessionModel::RecoveryStateRole).value<FileStates::RecoveryState>()
              == FileStates::RecoveryState::Recording);
        // The burst ends after a pause, and its record is written behind the editor.
        test_support::waitUntil([&] { return !model.recording(); }, "the typed burst was not recorded");
        CHECK(records(model) == before + 1 && !model.hasPendingRecovery());
        CHECK(role(model, SessionModel::RecoveryStateRole).value<FileStates::RecoveryState>()
              == FileStates::RecoveryState::Durable);
        const auto replayed = replayedFile(model.recoveryDirectory());
        CHECK(replayed.current().id == typed.id && replayed.current().draft.text == typed.draft.text);
        CHECK(model.undo("choices", typed.id) == Transition::Applied);
        CHECK(model.file("choices")->current().draft.text == launch.draft.text && !model.file("choices")->canUndo());
        CHECK(replayedFile(model.recoveryDirectory()).current().draft.text == launch.draft.text);
    });
    suite.run("a typed line break starts a new undo step", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        TestSession model(directory.path() + "/recovery");
        CHECK(model.appendFile(test_support::choiceFile()));
        const auto launch = model.file("choices")->current().draft.text;
        typeInto(model, "choices", 0, "a\nb");
        CHECK(model.recordTyping() && !model.recording());
        CHECK(model.undo("choices", model.file("choices")->current().id) == Transition::Applied);
        CHECK(model.file("choices")->current().draft.text == "a" + launch);
        CHECK(model.undo("choices", model.file("choices")->current().id) == Transition::Applied);
        CHECK(model.file("choices")->current().draft.text == launch);
        const auto drafts = test_support::recordedDrafts(storage::RecoveryStore(model.recoveryDirectory()));
        CHECK(drafts.size() == 5);
        CHECK(drafts[1].text == "a" + launch && drafts[2].text == "a\nb" + launch);
    });
    suite.run("a command records the typing before it, so the journal keeps the order of edits", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        TestSession model(directory.path() + "/recovery");
        CHECK(model.appendFile(test_support::choiceFile()));
        const auto before = records(model);
        typeInto(model, "choices", 0, "saved ");
        const auto typed = model.file("choices")->current();
        CHECK(model.recordCheckpoint("choices", typed.id, Outcome::Succeeded) == Transition::Applied);
        CHECK(!model.recording() && records(model) == before + 2);
        const auto replayed = replayedFile(model.recoveryDirectory());
        CHECK(replayed.publication().checkpoint() == typed.id);
        CHECK(replayed.revision(typed.id)->draft.text == typed.draft.text);
        // The next key starts a new burst rather than joining the one the Save recorded.
        typeInto(model, "choices", 0, "x");
        CHECK(model.undo("choices", model.file("choices")->current().id) == Transition::Applied);
        CHECK(model.file("choices")->current().draft.text == typed.draft.text);
    });
    suite.run("typing that puts every recorded field back leaves no record and no undo step", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        TestSession model(directory.path() + "/recovery");
        CHECK(model.appendFile(test_support::choiceFile()));
        CHECK(model.editResult("choices", 1, {{0, 0}, "hand "}) == Transition::Applied);
        const auto edited = model.file("choices")->current();
        const auto before = records(model);
        typeInto(model, "choices", 2, "q");
        const auto revision = model.file("choices")->current().id;
        CHECK(model.typeResult("choices", revision, {{2, 3}, ""}) == Transition::Applied);
        // Inside the hand-edited range, so text, decisions, hand-edit flags and ranges are as they
        // were: the file is back at the revision before the burst at once.
        CHECK(model.file("choices")->current().id == edited.id && !model.recording());
        CHECK(model.file("choices")->correspondence()->handEditedRanges() == edited.correspondence->handEditedRanges());
        CHECK(model.recordTyping() && records(model) == before);
        const auto replayed = replayedFile(model.recoveryDirectory());
        CHECK(replayed.current().id == edited.id && replayed.current().draft.text == edited.draft.text);
        CHECK(model.undo("choices", edited.id) == Transition::Applied && !model.file("choices")->canUndo());
    });
    suite.run("a failed typed record keeps every later typed edit pending in order until one retry", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        const auto faults = std::make_shared<test_support::SyncFaults>();
        TestSession model(directory.path() + "/recovery", faults);
        CHECK(model.appendFile(test_support::choiceFile()));
        const auto launch = model.file("choices")->current();
        const auto before = records(model);
        {
            const test_support::ArmedFault fault(*faults, test_support::SyncFaults::Point::JournalSync);
            // The line break ends the first burst, whose record goes to the worker; the rest of
            // the typing is applied before its outcome arrives.
            typeInto(model, "choices", 0, "a\nb");
            CHECK(model.file("choices")->current().draft.text == "a\nb" + launch.draft.text);
            CHECK(!model.recordTyping());
            CHECK_EQ(faults->failures.load(), 1);
        }
        // The file shows its acknowledged state and the editor the newest pending draft.
        CHECK(model.hasPendingRecovery() && !model.recording());
        CHECK(model.file("choices")->current().id == launch.id
              && model.file("choices")->current().draft.text == launch.draft.text);
        CHECK(model.pendingDraft("choices") && model.pendingDraft("choices")->text == "a\nb" + launch.draft.text);
        CHECK(model.pendingAction("choices") == SessionStore::Action::ReplaceDraft);
        CHECK(!model.pendingError("choices").isEmpty());
        CHECK(role(model, SessionModel::RecoveryStateRole).value<FileStates::RecoveryState>()
              == FileStates::RecoveryState::Failed);
        CHECK(model.typeResult("choices", launch.id, {{0, 0}, "z"}) == Transition::Blocked);
        CHECK(model.retryPendingRecovery("choices"));
        CHECK(!model.hasPendingRecovery() && !model.pendingDraft("choices"));
        const auto typed = model.file("choices")->current();
        CHECK(typed.draft.text == "a\nb" + launch.draft.text);
        // Each record once, in the order typed.
        const auto drafts = test_support::recordedDrafts(storage::RecoveryStore(model.recoveryDirectory()));
        CHECK(records(model) == before + 2);
        CHECK(drafts[drafts.size() - 2].text == "a" + launch.draft.text && drafts.back().text == typed.draft.text);
        const auto replayed = replayedFile(model.recoveryDirectory());
        CHECK(replayed.current().id == typed.id && replayed.current().draft.text == typed.draft.text);
        CHECK(model.undo("choices", typed.id) == Transition::Applied);
        CHECK(model.file("choices")->current().draft.text == "a" + launch.draft.text);
    });
    suite.run("closing the store records the typing it holds", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        const auto location = directory.path() + "/recovery";
        std::string typed;
        {
            TestSession model(location);
            CHECK(model.appendFile(test_support::choiceFile()));
            typeInto(model, "choices", 0, "kept ");
            typed = model.file("choices")->current().draft.text;
            CHECK(model.recording());
        }
        CHECK(replayedFile(location).current().draft.text == typed);
    });
    return suite.report();
}
