// The session store's recovery: each command's journal record, replay after a restart or a crash,
// failed commits that keep their input for a retry, and mutation intents and their outcomes.
#include "SessionFixtures.h"
#include "TestHarness.h"
#include "TestSession.h"
#include "app/Translations.h"
#include "fixtures/ApplicationIdentity.h"
#include "fixtures/Await.h"
#include "fixtures/EventLoop.h"
#include "fixtures/RecoveryJournal.h"
#include "fixtures/SyncFaults.h"
#include "problems/ProblemText.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QMap>
#include <QProcess>
#include <QSemaphore>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <cstdlib>
#include <unistd.h>

using test_support::await;
using test_support::awaitAligned;

using namespace branch_truce;
using namespace session_fixtures;

namespace {
// Types `text` into the file one character at a time from byte `at`, as the editor sends keys.
void typeInto(SessionStore &store, const std::string &id, std::size_t at, const std::string &text)
{
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto revision = store.file(id)->current().id;
        CHECK(store.typeResult(id, revision, {{at + index, at + index}, text.substr(index, 1)}) == Transition::Applied);
    }
}

qsizetype records(const SessionStore &store)
{
    return storage::RecoveryStore(store.recoveryDirectory()).read().transactions.size();
}

// Holds exactly one persistence operation. Unwinding releases it before the store drains.
class DelayedFiles : public test_support::SyncFaults {
public:
    mutable std::atomic_bool armed{false}, entered{false};
    mutable QSemaphore release;
    int sync(int descriptor) const override
    {
        if (armed.exchange(false)) {
            entered = true;
            if (!release.tryAcquire(1, 5000)) throw std::runtime_error("persistence gate timed out");
        }
        return SyncFaults::sync(descriptor);
    }
};
struct ReleaseFiles {
    std::shared_ptr<DelayedFiles> files;
    ~ReleaseFiles() { files->release.release(); }
};

int acknowledgeAndCrash(const QString &directory)
{
    SessionStore store(directory);
    CHECK(store.appendFile(recoverableFile()));
    CHECK(await(store.replaceDraft("recoverable", 1, savedDraft())) == Transition::Applied);
    CHECK(await(store.recordCheckpoint("recoverable", 2, Outcome::Succeeded)) == Transition::Applied);
    CHECK(await(store.replaceDraft("recoverable", 2, unsavedDraft())) == Transition::Applied);
    CHECK(await(store.replaceDraft("recoverable", 3, finalDecision())) == Transition::Applied);
    CHECK(!store.hasPendingRecovery() && store.file("recoverable")->current().id == 4);
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
                    SessionStore store(directory.path() + "/recovery");
                    CHECK(store.appendFile(test_support::combinationFile(scope)));
                    CHECK(awaitAligned(store, store.editResult("choices", 1, {{0, 0}, "manual prefix\n"}))
                          == Transition::Applied);
                    CHECK(await(store.recordCheckpoint("choices", 2, Outcome::Succeeded)) == Transition::Applied);
                    const auto before = store.file("choices")->current().draft;
                    const auto target = test_support::choiceTarget(*store.file("choices"), SourceSide::Stage2, scope);
                    const auto preview = previewChoice(*store.file("choices"), 2, target.source.contributionId, action);
                    const auto count = storage::RecoveryStore(store.recoveryDirectory()).read().transactions.size();
                    CHECK(await(store.applyChoice("choices", preview)) == Transition::ConfirmationRequired);
                    CHECK(storage::RecoveryStore(store.recoveryDirectory()).read().transactions.size() == count);
                    CHECK(await(store.applyChoice("choices", preview, true)) == Transition::Applied);
                    CHECK(storage::RecoveryStore(store.recoveryDirectory()).read().transactions.size() == count + 1);
                    const auto chosen = store.file("choices")->current().draft;
                    CHECK(chosen != before && chosen.text.find(preview.replacement) != std::string::npos);
                    checkRecoveredHistory(store, before, chosen);
                });
        }
    }
    for (const auto scope : {NonConflictingScope::Left, NonConflictingScope::Right, NonConflictingScope::Both}) {
        suite.run("file batch undo and redo recover together with protected manual work: " + nameOf(scope), [=] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            SessionStore store(directory.path() + "/recovery");
            CHECK(store.appendFile(test_support::batchFile()));
            CHECK(awaitAligned(store, store.editResult("choices", 1, {{0, 0}, "manual prefix\n"}))
                  == Transition::Applied);
            CHECK(await(store.recordCheckpoint("choices", 2, Outcome::Succeeded)) == Transition::Applied);
            const auto before = store.file("choices")->current().draft;
            const auto preview = previewNonConflicting(*store.file("choices"), 2, scope);
            const auto count = storage::RecoveryStore(store.recoveryDirectory()).read().transactions.size();
            CHECK(await(store.applyNonConflicting("choices", preview)) == Transition::ConfirmationRequired);
            CHECK(storage::RecoveryStore(store.recoveryDirectory()).read().transactions.size() == count);
            CHECK(await(store.applyNonConflicting("choices", preview, true)) == Transition::Applied);
            CHECK(storage::RecoveryStore(store.recoveryDirectory()).read().transactions.size() == count + 1);
            const auto chosen = store.file("choices")->current().draft;
            CHECK(chosen.text.starts_with("manual prefix\n") && chosen != before);
            checkRecoveredHistory(store, before, chosen);
        });
    }
    for (const auto side : {SourceSide::Stage2, SourceSide::Stage3}) {
        suite.run("whole-file replacement undo and redo recover text and decisions: " + nameOf(side), [=] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            SessionStore store(directory.path() + "/recovery");
            CHECK(store.appendFile(test_support::choiceFile()));
            CHECK(awaitAligned(store, store.editResult("choices", 1, {{0, 0}, "manual prefix\n"}))
                  == Transition::Applied);
            CHECK(await(store.recordCheckpoint("choices", 2, Outcome::Succeeded)) == Transition::Applied);
            const auto before = store.file("choices")->current().draft;
            const auto preview = previewWholeFile(*store.file("choices"), 2, side);
            const auto count = storage::RecoveryStore(store.recoveryDirectory()).read().transactions.size();
            CHECK(await(store.applyWholeFile("choices", preview)) == Transition::ConfirmationRequired);
            CHECK(storage::RecoveryStore(store.recoveryDirectory()).read().transactions.size() == count);
            CHECK(await(store.applyWholeFile("choices", preview, true)) == Transition::Applied);
            CHECK(storage::RecoveryStore(store.recoveryDirectory()).read().transactions.size() == count + 1);
            const auto chosen = store.file("choices")->current().draft;
            CHECK(chosen.text == preview.replacement && chosen != before);
            checkRecoveredHistory(store, before, chosen);
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
                      SessionStore store(directory.path() + "/recovery", faults);
                      CHECK(store.appendFile(operation == 1 ? test_support::batchFile() : test_support::choiceFile()));
                      test_support::awaitRecovery(store);
                      const auto before = store.file("choices")->current().draft;
                      const auto target =
                          test_support::choiceTarget(*store.file("choices"), SourceSide::Stage2, ChoiceScope::Section);
                      const auto combination = previewChoice(*store.file("choices"), 1, target.source.contributionId,
                                                             ChoiceAction::CombineRightFirst);
                      const auto batch = previewNonConflicting(*store.file("choices"), 1);
                      const auto whole = previewWholeFile(*store.file("choices"), 1, SourceSide::Stage3);
                      const test_support::ArmedFault fault(*faults, test_support::SyncFaults::Point::JournalSync);
                      const auto result = operation == 0 ? await(store.applyChoice("choices", combination, true))
                          : operation == 1               ? await(store.applyNonConflicting("choices", batch, true))
                                                         : await(store.applyWholeFile("choices", whole, true));
                      CHECK(result == Transition::Blocked && faults->failures == 1);
                      CHECK(store.file("choices")->current().id == 1
                            && store.file("choices")->current().draft == before);
                      CHECK(store.pendingDraft("choices") && *store.pendingDraft("choices") != before);
                      const auto chosen = *store.pendingDraft("choices");
                      CHECK(await(store.undo("choices", 1)) == Transition::Blocked);
                      CHECK(await(store.retryRecovery("choices")) == Transition::Applied);
                      CHECK(store.file("choices")->current().draft == chosen && !store.hasPendingRecovery());
                      CHECK(replayedFile(store.recoveryDirectory()).current().draft == chosen);
                      const auto read = storage::RecoveryStore(store.recoveryDirectory()).read();
                      CHECK(read.succeeded && read.transactions.size() == 2);
                      CHECK(await(store.undo("choices", store.file("choices")->current().id)) == Transition::Applied);
                      CHECK(replayedFile(store.recoveryDirectory()).current().draft == before);
                  });
    }
    for (const bool staging : {false, true}) {
        const auto kind = staging ? MutationKind::Staging : MutationKind::Publication;
        const auto kindName = staging ? std::string("staging") : std::string("publication");
        suite.run("successive successful outcomes retain every earlier completion " + kindName, [=] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            SessionStore store(directory.path() + "/recovery");
            CHECK(store.appendFile(recoverableFile()));
            test_support::awaitRecovery(store);
            QStringList identities;
            std::vector<RevisionId> revisions;
            for (const auto &text : {std::string("first published\n"), std::string("second published\n")}) {
                const auto current = store.file("recoverable")->current();
                auto next = current.draft;
                next.text = text;
                CHECK(await(store.replaceDraft("recoverable", current.id, next)) == Transition::Applied);
                preparePublication(store, "recoverable", staging);
                revisions.push_back(store.file("recoverable")->current().id);
                identities.push_back(recordIntent(store, "recoverable", kind));
                completePublication(store, "recoverable", staging);
            }
            const auto replay = replayRecovery(storage::RecoveryStore(store.recoveryDirectory()));
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
            SessionStore store(directory.path() + "/recovery");
            CHECK(store.appendFile(recoverableFile()));
            test_support::awaitRecovery(store);
            preparePublication(store, "recoverable", staging);
            const auto first = recordIntent(store, "recoverable", kind);
            completePublication(store, "recoverable", staging);
            const auto interrupted = recordIntent(store, "recoverable", kind);
            const auto replay = replayRecovery(storage::RecoveryStore(store.recoveryDirectory()));
            CHECK(replay.succeeded && replay.mutations.size() == 2);
            CHECK(replay.mutations[0].id == first && replay.mutations[0].completed);
            CHECK(replay.mutations[1].id == interrupted && !replay.mutations[1].completed);
            CHECK(replay.mutations[0].target.current().id == replay.mutations[1].target.current().id);
        });
        suite.run("a later revision outcome leaves an unmatched earlier interruption uncertain " + kindName, [=] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            SessionStore store(directory.path() + "/recovery");
            CHECK(store.appendFile(recoverableFile()));
            test_support::awaitRecovery(store);
            preparePublication(store, "recoverable", staging);
            const auto interrupted = recordIntent(store, "recoverable", kind);
            auto edited = store.file("recoverable")->current().draft;
            edited.text = "newer successful revision\n";
            CHECK(await(store.replaceDraft("recoverable", 1, edited)) == Transition::Applied);
            preparePublication(store, "recoverable", staging);
            const auto completed = recordIntent(store, "recoverable", kind);
            completePublication(store, "recoverable", staging);
            const auto replay = replayRecovery(storage::RecoveryStore(store.recoveryDirectory()));
            CHECK(replay.succeeded && replay.mutations.size() == 2);
            CHECK(replay.mutations[0].id == interrupted && !replay.mutations[0].completed);
            CHECK(replay.mutations[1].id == completed && replay.mutations[1].completed);
        });
    }
    suite.run("outcomes match the file and kind and failed staging does not complete an intent", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        SessionStore store(directory.path() + "/recovery");
        CHECK(store.appendFile(recoverableFile()));
        test_support::awaitRecovery(store);
        auto captures = *recoverableFile().snapshots();
        captures.path = "other.txt";
        CHECK(store.appendFile(
            FileSession("other", captures.path, draft(captures.launchResult.text.utf8), {.snapshots = captures})));
        test_support::awaitRecovery(store);
        preparePublication(store, "recoverable", false);
        preparePublication(store, "other", false);
        recordIntent(store, "recoverable", MutationKind::Publication);
        recordIntent(store, "recoverable", MutationKind::Staging);
        recordIntent(store, "other", MutationKind::Publication);
        completePublication(store, "recoverable", false);
        CHECK(await(store.recordStage("recoverable", 1, Outcome::Failed, {}, {}, "stage failed"))
              == Transition::Applied);
        auto replay = replayRecovery(storage::RecoveryStore(store.recoveryDirectory()));
        CHECK(replay.succeeded && replay.mutations.size() == 3);
        CHECK(replay.mutations[0].completed && !replay.mutations[1].completed && !replay.mutations[2].completed);
        completePublication(store, "recoverable", true);
        replay = replayRecovery(storage::RecoveryStore(store.recoveryDirectory()));
        CHECK(replay.succeeded && replay.mutations[1].completed && !replay.mutations[2].completed);
        completePublication(store, "other", false);
        replay = replayRecovery(storage::RecoveryStore(store.recoveryDirectory()));
        CHECK(replay.succeeded && replay.mutations[2].completed);
    });
    for (const auto action : {ChoiceAction::Accept, ChoiceAction::Ignore, ChoiceAction::ResolveUsingSide}) {
        suite.run("command undo and redo each replay complete text and decisions: " + nameOf(action), [=] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            SessionStore store(directory.path() + "/recovery");
            CHECK(store.appendFile(test_support::choiceFile()));
            CHECK(awaitAligned(store, store.editResult("choices", 1, {{0, 0}, "manual prefix\n"}))
                  == Transition::Applied);
            CHECK(await(store.recordCheckpoint("choices", 2, Outcome::Succeeded)) == Transition::Applied);
            const auto before = store.file("choices")->current().draft;
            const auto target =
                test_support::choiceTarget(*store.file("choices"), SourceSide::Stage3, ChoiceScope::Section);
            const auto preview = previewChoice(*store.file("choices"), 2, target.source.contributionId, action);
            CHECK(await(store.applyChoice("choices", preview, true)) == Transition::Applied);
            const auto chosen = store.file("choices")->current().draft;
            CHECK(chosen != before);
            checkRecoveredHistory(store, before, chosen);
        });
    }
    suite.run("a failed choice retains its atomic candidate until durable retry", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        const auto faults = std::make_shared<test_support::SyncFaults>();
        auto [store, rows] = test_support::TestSession(directory.path() + "/recovery", faults);
        CHECK(store.appendFile(test_support::choiceFile()));
        test_support::awaitRecovery(store);
        const auto before = store.file("choices")->current().draft;
        const auto target =
            test_support::choiceTarget(*store.file("choices"), SourceSide::Stage3, ChoiceScope::Section);
        const auto preview =
            previewChoice(*store.file("choices"), 1, target.source.contributionId, ChoiceAction::ResolveUsingSide);
        const test_support::ArmedFault fault(*faults, test_support::SyncFaults::Point::JournalSync);
        CHECK(await(store.applyChoice("choices", preview, true)) == Transition::Blocked);
        CHECK(faults->failures == 1 && store.file("choices")->current().draft == before);
        CHECK(store.pendingDraft("choices") && store.pendingDraft("choices")->decisions != before.decisions);
        const auto pending = *store.pendingDraft("choices");
        CHECK(role(rows, SessionModel::PendingInputRole).toBool());
        CHECK(role(rows, SessionModel::RecoveryStateRole).value<FileStates::RecoveryState>()
              == FileStates::RecoveryState::Failed);
        CHECK(!store.hasRecoveryFailure("choices") && !role(rows, SessionModel::CanUndoRole).toBool());
        CHECK(await(store.undo("choices", 1)) == Transition::Blocked);
        CHECK(await(store.retryRecovery("choices")) == Transition::Applied);
        CHECK(!store.hasPendingRecovery() && store.file("choices")->current().draft == pending);
        CHECK(replayedFile(store.recoveryDirectory()).current().draft == pending);
        const auto records = storage::RecoveryStore(store.recoveryDirectory()).read();
        CHECK(records.succeeded && records.transactions.size() == 2);
    });
    for (const int checkpoint : {0, 1, 2}) {
        suite.run(std::string("durable discard replays instead of the abandoned draft: ")
                      + (checkpoint == 0       ? "no Save"
                             : checkpoint == 1 ? "to the Save"
                                               : "to the staged result"),
                  [=] {
                      QTemporaryDir directory;
                      CHECK(directory.isValid());
                      const auto location = directory.path() + "/recovery";
                      Draft retained;
                      {
                          SessionStore store(location);
                          CHECK(store.appendFile(recoverableFile()));
                          test_support::awaitRecovery(store);
                          if (checkpoint != 0) {
                              CHECK(await(store.replaceDraft("recoverable", 1, savedDraft())) == Transition::Applied);
                              if (checkpoint == 2)
                                  resolve(store, "recoverable");
                              else
                                  CHECK(await(store.recordCheckpoint("recoverable", 2, Outcome::Succeeded))
                                        == Transition::Applied);
                          }
                          retained = store.file("recoverable")->current().draft;
                          CHECK(await(store.replaceDraft("recoverable", store.file("recoverable")->current().id,
                                                         unsavedDraft()))
                                == Transition::Applied);
                          const auto revision = store.file("recoverable")->current().id;
                          CHECK((checkpoint == 2 ? await(store.discardToStaged("recoverable", revision))
                                                 : await(store.discardToCheckpoint("recoverable", revision)))
                                == Transition::Applied);
                          CHECK(store.file("recoverable")->current().draft == retained && !store.hasPendingRecovery());
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
                CHECK(problem_text::text(replay.problem).contains(expected));
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
        auto [store, rows] = test_support::TestSession(directory.path() + "/recovery", faults);
        CHECK(store.appendFile(recoverableFile()));
        test_support::awaitRecovery(store);
        const auto original = store.file("recoverable")->current().draft;
        const test_support::ArmedFault fault(*faults, test_support::SyncFaults::Point::JournalSync);
        CHECK(await(store.replaceDraft("recoverable", 1, unsavedDraft())) == Transition::Blocked);
        CHECK(faults->failures == 1 && store.hasPendingRecovery());
        CHECK(store.file("recoverable")->current().id == 1 && store.file("recoverable")->current().draft == original);
        CHECK(store.pendingDraft("recoverable") && *store.pendingDraft("recoverable") == unsavedDraft());
        CHECK(role(rows, SessionModel::PendingInputRole).toBool());
        CHECK(role(rows, SessionModel::PendingTextRole).toString().toUtf8()
              == QByteArray::fromStdString(unsavedDraft().text));
        CHECK(store.pendingDraft("recoverable")->decisions.size() == 2);
        CHECK(role(rows, SessionModel::RecoveryStateRole).value<FileStates::RecoveryState>()
              == FileStates::RecoveryState::Failed);
        CHECK(role(rows, SessionModel::RecoveryErrorRole).toString().contains("retry"));
        CHECK(!role(rows, SessionModel::CanUndoRole).toBool() && !SessionHandoff::build(store, {}).clean);
        CHECK(await(store.replaceDraft("recoverable", 1, draft("must not overwrite pending"))) == Transition::Blocked);
        CHECK(await(store.recordCheckpoint("recoverable", 1, Outcome::Succeeded)) == Transition::Blocked);
        CHECK(await(store.undo("recoverable", 1)) == Transition::Blocked);
        storage::RecoveryStore journal(store.recoveryDirectory());
        const auto before = journal.read();
        CHECK(before.succeeded && before.transactions.size() == 2);
        const auto request = before.transactions.back().id;
        CHECK(await(store.retryRecovery("recoverable")) == Transition::Applied);
        CHECK(!store.hasPendingRecovery() && !store.pendingDraft("recoverable"));
        CHECK(store.file("recoverable")->current().id == 2
              && store.file("recoverable")->current().draft == unsavedDraft());
        CHECK(role(rows, SessionModel::RecoveryStateRole).value<FileStates::RecoveryState>()
              == FileStates::RecoveryState::Durable);
        CHECK(role(rows, SessionModel::RecoveryErrorRole).toString().isEmpty());
        CHECK(store.file("recoverable")->facts().unsaved && !store.file("recoverable")->publication().checkpoint());
        const auto after = journal.read();
        CHECK(after.succeeded && after.transactions.size() == 2 && after.transactions.back().id == request);
        checkDraft(test_support::recordedDrafts(journal).back(), unsavedDraft());
        CHECK(await(store.undo("recoverable", 2)) == Transition::Applied);
        CHECK(store.file("recoverable")->current().draft == original);
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
            SessionStore written(stored);
            CHECK(written.appendFile(recoverableFile()));
            test_support::awaitRecovery(written);
            CHECK(await(written.replaceDraft("recoverable", 1, unsavedDraft())) == Transition::Applied);
        }
        QCoreApplication::setOrganizationName("Example Organization");
        CHECK(recoveryRoot() == root);
        SessionStore store(newRecoveryDirectory());
        CHECK(QFileInfo(store.recoveryDirectory()).absolutePath() == root);
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
        SessionStore store(directory.path() + "/recovery");
        CHECK(store.appendFile(test_support::markedFile(true)));
        test_support::awaitRecovery(store);
        const auto choose = [&](SourceSide side, ChoiceAction action) {
            const auto &file = *store.file("choices");
            const auto &target = test_support::choiceTarget(file, side, ChoiceScope::Section);
            return await(store.applyChoice(
                "choices", previewChoice(file, file.current().id, target.source.contributionId, action), true));
        };
        const auto check = [&] {
            const auto &live = *store.file("choices");
            const auto replay = replayedFile(store.recoveryDirectory());
            CHECK(replay.current().draft == live.current().draft && replay.current().id == live.current().id);
            CHECK(replay.markers() == live.markers() && replay.markerSections() == live.markerSections());
            CHECK(!store.hasPendingRecovery());
        };
        CHECK(choose(SourceSide::Stage2, ChoiceAction::Accept) == Transition::Applied);
        CHECK(choose(SourceSide::Stage3, ChoiceAction::Ignore) == Transition::Applied);
        const auto chosen = store.file("choices")->current().draft;
        CHECK(chosen.text.starts_with("header\nleft first\nleft second\nbridge\n<<<<<<< ours\n"));
        CHECK(chosen.unresolvedMarkers == 1);
        check();
        CHECK(await(store.undo("choices", store.file("choices")->current().id)) == Transition::Applied);
        CHECK(store.file("choices")->current().draft.text == test_support::markedText(true));
        check();
        CHECK(await(store.redo("choices", store.file("choices")->current().id)) == Transition::Applied);
        CHECK(store.file("choices")->current().draft == chosen);
        check();
        auto reopened = replayedFile(store.recoveryDirectory());
        CHECK(reopened.undo(reopened.current().id) == Transition::Applied);
        CHECK(reopened.current().draft.text == test_support::markedText(true)
              && reopened.current().draft.unresolvedMarkers == 2);
    });
    suite.run("replay derives the marker count against the captured sources", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        SessionStore store(directory.path() + "/recovery");
        CHECK(store.appendFile(test_support::headingFile()));
        test_support::awaitRecovery(store);
        CHECK(store.file("choices")->current().draft.unresolvedMarkers == 1);
        auto launch = replayedFile(store.recoveryDirectory());
        CHECK(launch.current().draft.unresolvedMarkers == 1 && launch.markers() == store.file("choices")->markers());
        auto resolved = store.file("choices")->current().draft;
        resolved.text = "Changes\n=======\nintro\nleft line\ntail\n";
        CHECK(await(store.replaceDraft("choices", 1, resolved)) == Transition::Applied);
        const auto &live = *store.file("choices");
        CHECK(live.current().draft.unresolvedMarkers == 0);
        auto replay = replayedFile(store.recoveryDirectory());
        CHECK(replay.current().draft == live.current().draft && replay.markers() == live.markers());
        CHECK(replay.current().draft.unresolvedMarkers == 0);
        CHECK(await(replay.beginMarkResolved(replay.current().id, true)) == Transition::Applied);
    });
    suite.run("manual Save and discard stay distinct from recovery and publication", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        const auto faults = std::make_shared<test_support::SyncFaults>();
        auto [store, rows] = test_support::TestSession(directory.path() + "/recovery", faults);
        CHECK(store.appendFile(recoverableFile()));
        CHECK(await(store.replaceDraft("recoverable", 1, savedDraft())) == Transition::Applied);
        CHECK(store.file("recoverable")->facts().unsaved && !store.file("recoverable")->publication().checkpoint());
        const auto savedRevision = store.file("recoverable")->current().id;
        CHECK(await(store.recordCheckpoint("recoverable", savedRevision, Outcome::Succeeded)) == Transition::Applied);
        CHECK(store.file("recoverable")->publication().checkpoint()
              && store.file("recoverable")->revision(*store.file("recoverable")->publication().checkpoint())->draft
                  == savedDraft());
        CHECK(!store.file("recoverable")->facts().unsaved && !store.file("recoverable")->publication().written()
              && !store.file("recoverable")->publication().staged()
              && !store.file("recoverable")->publication().reviewed());
        CHECK(role(rows, SessionModel::StatusRole).toString()
              == "Draft saved in Branch Truce; repository file unchanged");
        CHECK(await(store.replaceDraft("recoverable", savedRevision, unsavedDraft())) == Transition::Applied);
        const auto discardedRevision = store.file("recoverable")->current().id;
        CHECK(store.file("recoverable")->facts().unsaved
              && store.file("recoverable")->revision(*store.file("recoverable")->publication().checkpoint())->draft
                  == savedDraft());
        {
            const test_support::ArmedFault fault(*faults, test_support::SyncFaults::Point::JournalSync);
            CHECK(await(store.discardToCheckpoint("recoverable", discardedRevision)) == Transition::Blocked);
            CHECK(faults->failures == 1);
        }
        CHECK(store.hasPendingRecovery() && store.file("recoverable")->current().draft == unsavedDraft());
        CHECK(role(rows, SessionModel::RecoveryStateRole).value<FileStates::RecoveryState>()
              == FileStates::RecoveryState::Failed);
        CHECK(await(store.retryRecovery("recoverable")) == Transition::Applied);
        CHECK(store.file("recoverable")->current().draft == savedDraft());
        CHECK(!store.file("recoverable")->facts().unsaved && !store.file("recoverable")->publication().written()
              && !store.file("recoverable")->publication().staged());
        storage::RecoveryStore journal(store.recoveryDirectory());
        auto read = journal.read();
        CHECK(read.succeeded && read.transactions.back().state.value("action") == "discard-to-checkpoint");
        const auto drafts = test_support::recordedDrafts(journal);
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
        const auto restored = replayedFile(store.recoveryDirectory());
        CHECK(restored.current().draft == savedDraft() && restored.current().draft != unsavedDraft());
        CHECK(restored.publication().checkpoint()
              && restored.revision(*restored.publication().checkpoint())->draft == savedDraft());
        CHECK(!restored.facts().unsaved && !restored.publication().written() && !restored.publication().staged());

        CHECK(await(store.replaceDraft("recoverable", store.file("recoverable")->current().id, unsavedDraft()))
              == Transition::Applied);
        const auto edited = *store.file("recoverable");
        {
            const test_support::ArmedFault fault(*faults, test_support::SyncFaults::Point::JournalSync);
            CHECK(await(store.recordCheckpoint("recoverable", edited.current().id, Outcome::Succeeded))
                  == Transition::Blocked);
            CHECK(faults->failures == 1);
        }
        CHECK(store.file("recoverable")->current().id == edited.current().id);
        CHECK(store.file("recoverable")->current().draft == unsavedDraft());
        CHECK(store.file("recoverable")->revision(*store.file("recoverable")->publication().checkpoint())->draft
              == savedDraft());
        CHECK(store.file("recoverable")->facts().unsaved);
        CHECK(role(rows, SessionModel::StatusRole).toString() == "Draft save failed");
        CHECK(role(rows, SessionModel::UnsavedRole).toBool());
        CHECK(role(rows, SessionModel::RecoveryStateRole).value<FileStates::RecoveryState>()
              == FileStates::RecoveryState::SaveFailed);
        CHECK(store.file("recoverable")->current().draft.text == unsavedDraft().text);
        CHECK(role(rows, SessionModel::FinishBlockersRole)
                  .toStringList()
                  .contains("Draft save failed; the edited result is still available"));
        CHECK(!store.file("recoverable")->publication().written()
              && !store.file("recoverable")->publication().staged());
        CHECK(await(store.discardToCheckpoint("recoverable", edited.current().id)) == Transition::Blocked);
        CHECK(await(store.retryRecovery("recoverable")) == Transition::Applied);
        CHECK(!store.hasPendingRecovery()
              && store.file("recoverable")->revision(*store.file("recoverable")->publication().checkpoint())->draft
                  == unsavedDraft());
        CHECK(!store.file("recoverable")->facts().unsaved && !store.file("recoverable")->publication().written()
              && !store.file("recoverable")->publication().staged());
        CHECK(role(rows, SessionModel::StatusRole).toString()
              == "Draft saved in Branch Truce; repository file unchanged");
        read = storage::RecoveryStore(store.recoveryDirectory()).read();
        CHECK(read.succeeded && read.transactions.back().state.value("action") == "manual-save");
        CHECK(read.transactions.back().state.value("written").isNull()
              && read.transactions.back().state.value("staged").isNull());
        const auto retried = replayedFile(store.recoveryDirectory());
        CHECK(retried.current().draft == unsavedDraft() && retried.publication().checkpoint()
              && retried.revision(*retried.publication().checkpoint())->draft == unsavedDraft()
              && !retried.publication().written() && !retried.publication().staged());
    });
    suite.run("failed Save and discard reuse one transaction and block the session until retry", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        const auto faults = std::make_shared<test_support::SyncFaults>();
        auto [store, rows] = test_support::TestSession(directory.path() + "/recovery", faults);
        CHECK(store.appendFile(recoverableFile()));
        CHECK(store.appendFile(FileSession("other", "other.txt", draft())));
        test_support::awaitRecovery(store);
        CHECK(!store.hasPendingRecovery());
        CHECK(await(store.replaceDraft("recoverable", 1, savedDraft())) == Transition::Applied);
        const auto savedRevision = store.file("recoverable")->current().id;
        CHECK(await(store.recordCheckpoint("recoverable", savedRevision, Outcome::Succeeded)) == Transition::Applied);
        CHECK(await(store.replaceDraft("recoverable", savedRevision, unsavedDraft())) == Transition::Applied);
        const auto discardedRevision = store.file("recoverable")->current().id;
        const auto otherRevision = store.file("other")->current().id;
        const auto otherDraft = store.file("other")->current().draft;
        storage::RecoveryStore journal(store.recoveryDirectory());
        qsizetype records = 0;
        QString request;
        {
            const test_support::ArmedFault fault(*faults, test_support::SyncFaults::Point::JournalSync);
            CHECK(await(store.discardToCheckpoint("recoverable", discardedRevision)) == Transition::Blocked);
            CHECK(faults->failures == 1);
        }
        CHECK(store.hasPendingRecovery() && store.file("recoverable")->current().draft == unsavedDraft());
        CHECK(store.pendingDraft("recoverable") && *store.pendingDraft("recoverable") == savedDraft());
        CHECK(store.file("recoverable")->revision(*store.file("recoverable")->publication().checkpoint())->draft
              == savedDraft());
        CHECK(role(rows, SessionModel::RecoveryStateRole).value<FileStates::RecoveryState>()
              == FileStates::RecoveryState::Failed);
        CHECK(await(store.replaceDraft("other", otherRevision, draft("blocked other"))) == Transition::Blocked);
        CHECK(await(store.undo("recoverable", discardedRevision)) == Transition::Blocked);
        CHECK(await(store.recordCheckpoint("recoverable", discardedRevision, Outcome::Succeeded))
              == Transition::Blocked);
        CHECK(store.file("other")->current().id == otherRevision && store.file("other")->current().draft == otherDraft);
        CHECK(store.file("recoverable")->current().draft == unsavedDraft());
        {
            const auto pending = journal.read();
            CHECK(pending.succeeded && pending.transactions.back().state.value("action") == "discard-to-checkpoint");
            records = pending.transactions.size();
            request = pending.transactions.back().id;
            checkDraft(test_support::recordedDrafts(journal).back(), savedDraft());
        }
        CHECK(await(store.retryRecovery("recoverable")) == Transition::Applied);
        CHECK(!store.hasPendingRecovery() && store.file("recoverable")->current().draft == savedDraft());
        CHECK(store.file("recoverable")->current().draft.decisions.at("right").value == DecisionValue::Ignored);
        CHECK(!store.file("recoverable")->publication().written()
              && !store.file("recoverable")->publication().staged());
        {
            const auto committed = journal.read();
            CHECK(committed.succeeded && committed.transactions.size() == records);
            CHECK(committed.transactions.back().id == request);
            CHECK(committed.transactions.back().state.value("action") == "discard-to-checkpoint");
            CHECK(committed.transactions.back().state.value("written").isNull()
                  && committed.transactions.back().state.value("staged").isNull());
            checkDraft(test_support::recordedDrafts(journal).back(), savedDraft());
        }
        CHECK(await(store.replaceDraft("other", otherRevision, draft("other stayed editable"))) == Transition::Applied);

        CHECK(await(store.replaceDraft("recoverable", store.file("recoverable")->current().id, unsavedDraft()))
              == Transition::Applied);
        const auto editedRevision = store.file("recoverable")->current().id;
        const auto otherEdited = store.file("other")->current();
        {
            const test_support::ArmedFault fault(*faults, test_support::SyncFaults::Point::JournalSync);
            CHECK(await(store.recordCheckpoint("recoverable", editedRevision, Outcome::Succeeded))
                  == Transition::Blocked);
            CHECK(faults->failures == 1);
        }
        CHECK(store.file("recoverable")->current().id == editedRevision);
        CHECK(store.file("recoverable")->current().draft == unsavedDraft());
        CHECK(store.pendingDraft("recoverable") && *store.pendingDraft("recoverable") == unsavedDraft());
        CHECK(store.file("recoverable")->revision(*store.file("recoverable")->publication().checkpoint())->draft
              == savedDraft());
        CHECK(store.file("recoverable")->facts().unsaved && !store.file("recoverable")->publication().written()
              && !store.file("recoverable")->publication().staged());
        CHECK(role(rows, SessionModel::StatusRole).toString() == "Draft save failed");
        CHECK(role(rows, SessionModel::RecoveryStateRole).value<FileStates::RecoveryState>()
              == FileStates::RecoveryState::SaveFailed);
        CHECK(store.file("recoverable")->current().draft.text == unsavedDraft().text);
        CHECK(await(store.replaceDraft("other", otherEdited.id, draft("must stay blocked"))) == Transition::Blocked);
        CHECK(await(store.undo("recoverable", editedRevision)) == Transition::Blocked);
        CHECK(await(store.discardToCheckpoint("recoverable", editedRevision)) == Transition::Blocked);
        CHECK(store.file("other")->current().id == otherEdited.id
              && store.file("other")->current().draft == otherEdited.draft);
        CHECK(store.file("recoverable")->current().draft == unsavedDraft());
        {
            const auto pending = journal.read();
            CHECK(pending.succeeded && pending.transactions.back().state.value("action") == "manual-save");
            records = pending.transactions.size();
            request = pending.transactions.back().id;
            CHECK(pending.transactions.back().state.value("written").isNull()
                  && pending.transactions.back().state.value("staged").isNull());
            checkDraft(test_support::recordedDrafts(journal).back(), unsavedDraft());
        }
        CHECK(await(store.retryRecovery("recoverable")) == Transition::Applied);
        CHECK(!store.hasPendingRecovery() && store.file("recoverable")->current().draft == unsavedDraft());
        CHECK(store.file("recoverable")->publication().checkpoint()
              && store.file("recoverable")->revision(*store.file("recoverable")->publication().checkpoint())->draft
                  == unsavedDraft());
        CHECK(store.file("recoverable")->current().draft.decisions.at("right").value == DecisionValue::Accepted);
        CHECK(!store.file("recoverable")->facts().unsaved && !store.file("recoverable")->publication().written()
              && !store.file("recoverable")->publication().staged());
        CHECK(role(rows, SessionModel::StatusRole).toString()
              == "Draft saved in Branch Truce; repository file unchanged");
        {
            const auto committed = journal.read();
            CHECK(committed.succeeded && committed.transactions.size() == records);
            CHECK(committed.transactions.back().id == request);
            CHECK(committed.transactions.back().state.value("action") == "manual-save");
            CHECK(committed.transactions.back().state.value("written").isNull()
                  && committed.transactions.back().state.value("staged").isNull());
            checkDraft(test_support::recordedDrafts(journal).back(), unsavedDraft());
        }
        CHECK(await(store.replaceDraft("other", otherEdited.id, draft("unblocked after retry")))
              == Transition::Applied);
        const auto replay = replayRecovery(storage::RecoveryStore(store.recoveryDirectory()));
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
        auto [store, rows] = test_support::TestSession(directory.path() + "/recovery");
        CHECK(store.appendFile(test_support::choiceFile()));
        test_support::awaitRecovery(store);
        const auto launch = store.file("choices")->current();
        const auto before = records(store);
        typeInto(store, "choices", 0, "abc");
        const auto typed = store.file("choices")->current();
        CHECK(typed.id == launch.id + 3 && typed.draft.text == "abc" + launch.draft.text);
        // Applied, aligned later and not yet acknowledged.
        CHECK(store.file("choices")->pendingAlignment() != nullptr);
        CHECK(store.recording("choices") && store.holdsUserWork() && records(store) == before);
        CHECK(role(rows, SessionModel::RecoveryStateRole).value<FileStates::RecoveryState>()
              == FileStates::RecoveryState::Recording);
        // The burst ends after a pause, and its record is written behind the editor.
        test_support::waitUntil([&] { return !store.recording(); }, "the typed burst was not recorded");
        CHECK(records(store) == before + 1 && !store.hasPendingRecovery());
        CHECK(role(rows, SessionModel::RecoveryStateRole).value<FileStates::RecoveryState>()
              == FileStates::RecoveryState::Durable);
        const auto replayed = replayedFile(store.recoveryDirectory());
        CHECK(replayed.current().id == typed.id && replayed.current().draft.text == typed.draft.text);
        CHECK(await(store.undo("choices", typed.id)) == Transition::Applied);
        CHECK(store.file("choices")->current().draft.text == launch.draft.text && !store.file("choices")->canUndo());
        CHECK(replayedFile(store.recoveryDirectory()).current().draft.text == launch.draft.text);
    });
    suite.run("a typed line break starts a new undo step", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        SessionStore store(directory.path() + "/recovery");
        CHECK(store.appendFile(test_support::choiceFile()));
        test_support::awaitRecovery(store);
        const auto launch = store.file("choices")->current().draft.text;
        typeInto(store, "choices", 0, "a\nb");
        store.recordTyping();
        test_support::awaitRecovery(store);
        CHECK(!store.hasPendingRecovery() && !store.recording());
        CHECK(await(store.undo("choices", store.file("choices")->current().id)) == Transition::Applied);
        CHECK(store.file("choices")->current().draft.text == "a" + launch);
        CHECK(await(store.undo("choices", store.file("choices")->current().id)) == Transition::Applied);
        CHECK(store.file("choices")->current().draft.text == launch);
        const auto drafts = test_support::recordedDrafts(storage::RecoveryStore(store.recoveryDirectory()));
        CHECK(drafts.size() == 5);
        CHECK(drafts[1].text == "a" + launch && drafts[2].text == "a\nb" + launch);
    });
    suite.run("a command records the typing before it, so the journal keeps the order of edits", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        SessionStore store(directory.path() + "/recovery");
        CHECK(store.appendFile(test_support::choiceFile()));
        test_support::awaitRecovery(store);
        const auto before = records(store);
        typeInto(store, "choices", 0, "saved ");
        const auto typed = store.file("choices")->current();
        CHECK(await(store.recordCheckpoint("choices", typed.id, Outcome::Succeeded)) == Transition::Applied);
        CHECK(!store.recording() && records(store) == before + 2);
        const auto replayed = replayedFile(store.recoveryDirectory());
        CHECK(replayed.publication().checkpoint() == typed.id);
        CHECK(replayed.revision(typed.id)->draft.text == typed.draft.text);
        // The next key starts a new burst rather than joining the one the Save recorded.
        typeInto(store, "choices", 0, "x");
        CHECK(await(store.undo("choices", store.file("choices")->current().id)) == Transition::Applied);
        CHECK(store.file("choices")->current().draft.text == typed.draft.text);
    });
    suite.run("typing that puts every recorded field back leaves no record and no undo step", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        SessionStore store(directory.path() + "/recovery");
        CHECK(store.appendFile(test_support::choiceFile()));
        CHECK(awaitAligned(store, store.editResult("choices", 1, {{0, 0}, "hand "})) == Transition::Applied);
        const auto edited = store.file("choices")->current();
        const auto before = records(store);
        typeInto(store, "choices", 2, "q");
        const auto revision = store.file("choices")->current().id;
        CHECK(store.typeResult("choices", revision, {{2, 3}, ""}) == Transition::Applied);
        // Inside the hand-edited range, so text, decisions, hand-edit flags and ranges are as they
        // were: the file is back at the revision before the burst at once.
        CHECK(store.file("choices")->current().id == edited.id && !store.recording());
        CHECK(store.file("choices")->correspondence()->handEditedRanges() == edited.correspondence->handEditedRanges());
        CHECK(store.recordTyping() && records(store) == before);
        const auto replayed = replayedFile(store.recoveryDirectory());
        CHECK(replayed.current().id == edited.id && replayed.current().draft.text == edited.draft.text);
        CHECK(await(store.undo("choices", edited.id)) == Transition::Applied && !store.file("choices")->canUndo());
    });
    suite.run("a failed typed record keeps every later typed edit pending in order until one retry", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        const auto faults = std::make_shared<test_support::SyncFaults>();
        auto [store, rows] = test_support::TestSession(directory.path() + "/recovery", faults);
        CHECK(store.appendFile(test_support::choiceFile()));
        test_support::awaitRecovery(store);
        const auto launch = store.file("choices")->current();
        const auto before = records(store);
        {
            const test_support::ArmedFault fault(*faults, test_support::SyncFaults::Point::JournalSync);
            // The line break ends the first burst, whose record goes to the worker; the rest of
            // the typing is applied before its outcome arrives.
            typeInto(store, "choices", 0, "a\nb");
            CHECK(store.file("choices")->current().draft.text == "a\nb" + launch.draft.text);
            store.recordTyping();
            test_support::awaitRecovery(store);
            CHECK(store.hasPendingRecovery());
            CHECK_EQ(faults->failures.load(), 1);
        }
        // The file shows its acknowledged state and the editor the newest pending draft.
        CHECK(store.hasPendingRecovery() && !store.recording());
        CHECK(store.file("choices")->current().id == launch.id
              && store.file("choices")->current().draft.text == launch.draft.text);
        CHECK(store.pendingDraft("choices") && store.pendingDraft("choices")->text == "a\nb" + launch.draft.text);
        CHECK(store.pendingAction("choices") == SessionStore::Action::ReplaceDraft);
        CHECK(!store.pendingError("choices").isEmpty());
        CHECK(role(rows, SessionModel::RecoveryStateRole).value<FileStates::RecoveryState>()
              == FileStates::RecoveryState::Failed);
        CHECK(store.typeResult("choices", launch.id, {{0, 0}, "z"}) == Transition::Blocked);
        CHECK(await(store.retryRecovery("choices")) == Transition::Applied);
        CHECK(!store.hasPendingRecovery() && !store.pendingDraft("choices"));
        const auto typed = store.file("choices")->current();
        CHECK(typed.draft.text == "a\nb" + launch.draft.text);
        // Each record once, in the order typed.
        const auto drafts = test_support::recordedDrafts(storage::RecoveryStore(store.recoveryDirectory()));
        CHECK(records(store) == before + 2);
        CHECK(drafts[drafts.size() - 2].text == "a" + launch.draft.text && drafts.back().text == typed.draft.text);
        const auto replayed = replayedFile(store.recoveryDirectory());
        CHECK(replayed.current().id == typed.id && replayed.current().draft.text == typed.draft.text);
        CHECK(await(store.undo("choices", typed.id)) == Transition::Applied);
        CHECK(store.file("choices")->current().draft.text == "a" + launch.draft.text);
    });
    suite.run("Save and undo queue behind delayed typing without acknowledging unfinished work", [] {
        QTemporaryDir directory;
        auto files = std::make_shared<DelayedFiles>();
        SessionStore store(directory.path() + "/recovery", files);
        CHECK(store.appendFile(test_support::choiceFile()));
        test_support::awaitRecovery(store);
        const auto initial = store.file("choices")->current().draft;
        ReleaseFiles release{files};
        files->armed = true;
        typeInto(store, "choices", 0, "typed ");
        const auto typed = store.file("choices")->current();
        auto saved = store.recordCheckpoint("choices", typed.id, Outcome::Succeeded);
        auto undone = store.undo("choices", typed.id);
        test_support::waitUntil([&] { return files->entered.load(); }, "typing did not reach persistence");
        CHECK(!saved.isFinished() && !undone.isFinished());
        CHECK(store.file("choices")->current().id == typed.id && !store.file("choices")->publication().checkpoint());
        CHECK(!SessionHandoff::build(store, {}).clean && !SessionHandoff::build(store, {}).canLeave);
        files->release.release();
        CHECK(await(saved) == Transition::Applied);
        CHECK(await(undone) == Transition::Applied);
        CHECK(store.file("choices")->current().draft == initial);
        const auto read = storage::RecoveryStore(store.recoveryDirectory()).read();
        CHECK(read.succeeded && read.transactions.size() == 4);
        CHECK(read.transactions[1].state.value("action") == "replace-draft");
        CHECK(read.transactions[2].state.value("action") == "manual-save");
        CHECK(read.transactions[3].state.value("action") == "undo");
        CHECK(replayedFile(store.recoveryDirectory()).current().draft == initial);
    });
    suite.run("a failed command holds another file and retry commits both original transactions in order", [] {
        QTemporaryDir directory;
        auto files = std::make_shared<DelayedFiles>();
        SessionStore store(directory.path() + "/recovery", files);
        CHECK(store.appendFile(sessionFile("first")) && store.appendFile(sessionFile("second")));
        test_support::awaitRecovery(store);
        ReleaseFiles release{files};
        files->armed = true;
        files->arm(test_support::SyncFaults::Point::JournalSync);
        auto first = store.editResult("first", 1, {{0, 0}, "one "});
        auto second = store.editResult("second", 1, {{0, 0}, "two "});
        test_support::waitUntil([&] { return files->entered.load(); }, "command did not reach persistence");
        CHECK(!first.isFinished() && !second.isFinished());
        CHECK(store.file("first")->current().id == 1 && store.file("second")->current().id == 1);
        files->release.release();
        CHECK(await(first) == Transition::Blocked && await(second) == Transition::Blocked);
        CHECK(files->failures == 1 && store.hasPendingRecovery());
        CHECK(store.pendingDraft("first")->text == "one result\n");
        CHECK(store.pendingDraft("second")->text == "two result\n");
        const auto beforeRetry = storage::RecoveryStore(store.recoveryDirectory()).read();
        CHECK(beforeRetry.succeeded && beforeRetry.transactions.size() == 3);
        const auto original = beforeRetry.transactions.back().id;
        files->entered = false;
        files->armed = true;
        auto retry = store.retryRecovery("first");
        test_support::waitUntil([&] { return files->entered.load(); }, "retry did not reach persistence");
        CHECK(store.pendingDraft("first")->text == "one result\n");
        CHECK(store.pendingDraft("second")->text == "two result\n");
        files->release.release();
        CHECK(await(retry) == Transition::Applied);
        const auto committed = storage::RecoveryStore(store.recoveryDirectory()).read();
        CHECK(committed.succeeded && committed.transactions.size() == 4);
        CHECK(committed.transactions[2].id == original);
        CHECK(RecoveryRecordReader(storage::RecoveryStore(store.recoveryDirectory()), committed.transactions[2].state)
                  .fileId()
              == "first");
        CHECK(RecoveryRecordReader(storage::RecoveryStore(store.recoveryDirectory()), committed.transactions[3].state)
                  .fileId()
              == "second");
        CHECK(store.file("first")->current().draft.text == "one result\n");
        CHECK(store.file("second")->current().draft.text == "two result\n");
        CHECK(await(store.undo("first", 2)) == Transition::Applied);
        CHECK(await(store.undo("second", 2)) == Transition::Applied);
        CHECK(store.file("first")->current().draft.text == "result\n");
        CHECK(store.file("second")->current().draft.text == "result\n");
    });
    suite.run("a durable mutation receipt keeps its exact store lease across replacement", [] {
        QTemporaryDir directory;
        SessionStore store(directory.path() + "/recovery");
        CHECK(store.appendFile(recoverableFile()));
        test_support::awaitRecovery(store);
        const auto original = store.recoveryDirectory();
        const auto transaction = mutationRecoveryTransaction(
            *store.file("recoverable"), {QUuid::createUuid().toString(), MutationKind::Publication, {}});
        const auto receipt = await(store.commitRecord(transaction));
        CHECK(receipt.commit.succeeded && receipt.lease == store.recoveryLease());
        CHECK(store.startNewRecoveryStore());
        store.replaceFiles({recoverableFile()});
        test_support::awaitRecovery(store);
        CHECK(store.recoveryLease() && store.recoveryLease() != receipt.lease);
        CHECK(receipt.lease->directory() == original && store.recoveryDirectory() != original);
        const auto read = storage::RecoveryStore(original).read();
        CHECK(read.succeeded && read.transactions.back().id == transaction.id);
    });
    suite.run("closing drains an accepted command and finishes its durability future", [] {
        QTemporaryDir directory;
        const auto location = directory.path() + "/recovery";
        auto store = std::make_unique<SessionStore>(location);
        CHECK(store->appendFile(test_support::choiceFile()));
        test_support::awaitRecovery(*store);
        auto command = store->editResult("choices", 1, {{0, 0}, "kept "});
        CHECK(!command.isFinished());
        store.reset();
        CHECK(command.isFinished() && command.result() == Transition::Applied);
        CHECK(replayedFile(location).current().draft.text.starts_with("kept "));
    });
    suite.run("closing the store records the typing it holds", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        const auto location = directory.path() + "/recovery";
        std::string typed;
        {
            SessionStore store(location);
            CHECK(store.appendFile(test_support::choiceFile()));
            test_support::awaitRecovery(store);
            typeInto(store, "choices", 0, "kept ");
            typed = store.file("choices")->current().draft.text;
            CHECK(store.recording());
        }
        CHECK(replayedFile(location).current().draft.text == typed);
    });
    return suite.report();
}
