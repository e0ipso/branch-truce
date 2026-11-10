#include "TestHarness.h"
#include "core/EditingCommands.h"
#include "core/ExactLineCorrespondence.h"
#include "fixtures/ChoiceFile.h"
#include "fixtures/RecoveryJournal.h"
#include "recovery/RecoveryRecord.h"
#include "recovery/RecoveryRestore.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>
#include <atomic>
#include <set>

using namespace branch_truce;

namespace {
// Every source slot filled, a rebase provenance, a conversion policy, a
// launch identity and a path that is not valid UTF-8.
FileSession capturedFile()
{
    auto captured = *test_support::choiceFile().snapshots();
    captured.path = std::string("choices\t") + static_cast<char>(0xff) + ".txt";
    const std::string base = "header\nbase first\nbase second\nbridge\nbase later\ntail\n";
    captured.earlier =
        SourceSnapshot{"base-object", "100644", base, test_support::choiceText(base), {}, "Common ancestor"};
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
    return FileSession(
        "choices", path, {text, {}, 0},
        {.supportDetail = "captured at launch", .snapshots = std::move(captured), .initializeChoices = true});
}

// Appends records the way SessionStore does: each one encodes the file after its command, with
// its current text as a change from the text before.
class Recorder {
public:
    explicit Recorder(const QString &directory, FileSession launch)
        : lease(storage::RecoveryLease::acquire(directory)), file(std::move(launch))
    {
        CHECK(lease != nullptr);
        commit(RecoveryAction::Open, {});
    }
    template <typename Command> void step(RecoveryAction action, Command command)
    {
        const auto before = file.current().draft.text;
        CHECK(command(file) == Transition::Applied);
        commit(action, before);
    }
    storage::RecoveryTransaction commit(RecoveryAction action, const std::string &before)
    {
        auto transaction =
            recoveryTransaction(file, action, QUuid::createUuid().toString(QUuid::WithoutBraces), before);
        CHECK(lease->commit(transaction).succeeded);
        return transaction;
    }
    std::shared_ptr<storage::RecoveryLease> lease;
    FileSession file;
};

// The states of the journal's records.
QList<QJsonObject> states(const storage::RecoveryStore &store)
{
    const auto read = store.read();
    CHECK(read.succeeded && !read.incompleteTail);
    QList<QJsonObject> result;
    for (const auto &stored : read.transactions) result.push_back(stored.state);
    return result;
}

// Counts the comparisons a file makes, so a case can tell which algorithm aligned it.
class CountingAlgorithm final : public CorrespondenceAlgorithm {
public:
    mutable std::atomic<int> calls = 0;
    std::vector<Passage> compare(std::string_view source, std::string_view result) const override
    {
        ++calls;
        return ExactLineCorrespondence().compare(source, result);
    }
    [[nodiscard]] std::string_view identity() const noexcept override { return ExactLineCorrespondence{}.identity(); }
};

// The default alignment under another name, as a later version of it would be.
class RenamedAlgorithm final : public CorrespondenceAlgorithm {
public:
    std::vector<Passage> compare(std::string_view source, std::string_view result) const override
    {
        return ExactLineCorrespondence().compare(source, result);
    }
    [[nodiscard]] std::string_view identity() const noexcept override { return "exact-line/renamed"; }
};

// Rewrites the journal's last state and replays it; the replay must refuse it with error.
template <typename Mutate> void rejected(const QString &directory, Mutate mutate, const QString &error)
{
    QTemporaryDir copy;
    CHECK(copy.isValid());
    const auto store = copy.path() + "/store";
    CHECK(QDir().mkpath(store + "/payloads"));
    const auto owner = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    for (const auto *folder : {"", "/payloads"}) {
        CHECK(QFile::setPermissions(store + QLatin1String(folder), owner | QFileDevice::ExeOwner));
        for (const auto &name : QDir(directory + QLatin1String(folder)).entryList(QDir::Files)) {
            if (name == QLatin1String("session.lock")) continue;
            const auto target = store + QLatin1String(folder) + QLatin1Char('/') + name;
            CHECK(QFile::copy(directory + QLatin1String(folder) + QLatin1Char('/') + name, target));
            CHECK(QFile::setPermissions(target, owner));
        }
    }
    test_support::rewriteLastRecoveryState(store, mutate);
    const auto replay = replayRecovery(storage::RecoveryStore(store));
    CHECK(!replay.succeeded && replay.files.empty());
    if (!replay.error.contains(error)) throw test_support::Failure(replay.error.toStdString());
}
}

int main()
{
    test_support::Suite suite("RecoveryRecordTests");
    suite.run("an edited file session replays to the state its records were encoded from", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        const auto counting = std::make_shared<CountingAlgorithm>();
        Recorder recorder(directory.path() + "/store", capturedFile());
        const auto choose = [&](SourceSide side, ChoiceScope scope, std::size_t occurrence, ChoiceAction action) {
            recorder.step(RecoveryAction::ReplaceDraft, [&](FileSession &file) {
                const auto target = test_support::choiceTarget(file, side, scope, occurrence).source.contributionId;
                return applyChoice(file, previewChoice(file, file.current().id, target, action), true);
            });
        };
        choose(SourceSide::Stage2, ChoiceScope::Line, 0, ChoiceAction::Accept);
        choose(SourceSide::Stage3, ChoiceScope::Line, 0, ChoiceAction::Ignore);
        choose(SourceSide::Stage2, ChoiceScope::Section, 1, ChoiceAction::CombineLeftFirst);
        recorder.step(RecoveryAction::ReplaceDraft, [](FileSession &file) {
            const auto at = file.current().draft.text.find("left first") + 5;
            return file.editResult(file.current().id, {{at, at}, "hand "});
        });
        recorder.step(RecoveryAction::ManualSave, [](FileSession &file) {
            return file.recordCheckpoint(file.current().id, Outcome::Succeeded, "saved");
        });
        recorder.step(RecoveryAction::ReplaceDraft, [](FileSession &file) {
            const auto end = file.current().draft.text.size();
            return file.editResult(file.current().id, {{end, end}, "after save\n"});
        });
        recorder.step(RecoveryAction::Undo, [](FileSession &file) { return file.undo(file.current().id); });
        recorder.step(RecoveryAction::Redo, [](FileSession &file) { return file.redo(file.current().id); });
        const auto &expected = recorder.file;
        CHECK(expected.publication().checkpoint() && *expected.publication().checkpoint() != expected.current().id);
        CHECK(!expected.correspondence()->handEditedAnchors().empty());

        const auto replay = replayRecovery(recorder.lease->store(), counting);
        CHECK(replay.succeeded && replay.error.isEmpty() && replay.userWork && replay.files.size() == 1);
        const auto &file = replay.files.front();
        CHECK(file.snapshots() && *file.snapshots() == *expected.snapshots());
        CHECK(file.current().id == expected.current().id && file.current().draft == expected.current().draft);
        CHECK(*file.publication().checkpoint() == *expected.publication().checkpoint()
              && file.revision(*file.publication().checkpoint())->draft
                  == expected.revision(*expected.publication().checkpoint())->draft);
        CHECK(file.correspondence()->handEditedAnchors() == expected.correspondence()->handEditedAnchors());
        CHECK(file.correspondence()->handEditedRanges() == expected.correspondence()->handEditedRanges());
        CHECK(file.publication().checkpointOutcome() && file.publication().checkpointOutcome()->detail == "saved");
        // The restored file aligns its sources with the algorithm the window uses.
        CHECK(counting->calls > 0);

        // The typed reader gives back what the last record wrote.
        const auto last = states(recorder.lease->store()).back();
        const RecoveryRecordReader record(recorder.lease->store(), last);
        CHECK_EQ(record.action(), RecoveryAction::Redo);
        CHECK(record.fileId() == expected.id() && record.path() == expected.path());
        CHECK_EQ(record.support(), Support::Supported);
        CHECK(record.supportDetail() == "captured at launch");
        CHECK(record.checkpointOutcome().detail == "saved");
        CHECK(!record.mutation());
        // Only the launch record carries the captures.
        const auto all = states(recorder.lease->store());
        CHECK(all.front().contains("snapshots"));
        CHECK(std::none_of(all.begin() + 1, all.end(), [](const auto &state) { return state.contains("snapshots"); }));
    });
    suite.run("one record replays a burst of edits", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        Recorder recorder(directory.path() + "/store", capturedFile());
        const auto before = recorder.file.current().draft.text;
        bool join = false;
        for (const auto *typed : {"one\n", "two\n", "three\n"}) {
            CHECK(recorder.file.editResult(recorder.file.current().id, {{0, 0}, typed}, {.join = join})
                  == Transition::Applied);
            join = true;
        }
        recorder.commit(RecoveryAction::ReplaceDraft, before);
        recorder.step(RecoveryAction::Undo, [](FileSession &file) { return file.undo(file.current().id); });
        const auto replay = replayRecovery(recorder.lease->store());
        CHECK(replay.succeeded && replay.files.size() == 1);
        const auto &file = replay.files.front();
        CHECK(file.current().id == recorder.file.current().id && file.current().draft == recorder.file.current().draft);
    });
    suite.run("replay refuses a journal another correspondence algorithm wrote", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        Recorder recorder(directory.path() + "/store", capturedFile());
        recorder.step(RecoveryAction::ReplaceDraft, [](FileSession &file) {
            const auto end = file.current().draft.text.size();
            return file.editResult(file.current().id, {{end, end}, "typed\n"});
        });
        const auto launch = states(recorder.lease->store()).front();
        CHECK(RecoveryRecordReader(recorder.lease->store(), launch).alignment()
              == ExactLineCorrespondence().identity());
        CHECK(replayRecovery(recorder.lease->store()).succeeded);
        const auto replay = replayRecovery(recorder.lease->store(), std::make_shared<RenamedAlgorithm>());
        CHECK(!replay.succeeded && replay.files.empty());
        CHECK(replay.error.contains(QStringLiteral("another correspondence algorithm")));
    });
    suite.run("a keystroke records the typed bytes, not the whole text", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        std::string large;
        for (int line = 0; large.size() < 512 * 1024; ++line) large += "unchanged line " + std::to_string(line) + '\n';
        auto launch = test_support::choiceFile();
        Recorder recorder(directory.path() + "/store", launch);
        recorder.step(RecoveryAction::ReplaceDraft, [&](FileSession &file) {
            auto draft = file.current().draft;
            draft.text = large;
            return file.replaceDraft(file.current().id, draft);
        });
        const auto before = recorder.file.current().draft.text;
        const auto at = before.size() / 2;
        CHECK(recorder.file.editResult(recorder.file.current().id, {{at, at}, "x"}) == Transition::Applied);
        const auto keystroke = recorder.commit(RecoveryAction::ReplaceDraft, before);
        qsizetype bytes = 0;
        for (const auto &payload : keystroke.payloads) bytes += payload.size();
        CHECK_LT(bytes, 1024);
        const auto replay = replayRecovery(recorder.lease->store());
        CHECK(replay.succeeded && replay.files.size() == 1);
        CHECK(replay.files.front().current().draft.text == recorder.file.current().draft.text);
    });
    suite.run("every recovery action, mutation kind and observation has one name", [] {
        std::set<QString> names;
        for (const auto &[action, name] : recoveryActionNames) {
            CHECK(valueNamed(recoveryActionNames, nameOf(recoveryActionNames, action)) == action);
            CHECK(names.insert(nameOf(recoveryActionNames, action)).second);
        }
        for (const auto &[kind, name] : mutationKindNames)
            CHECK(valueNamed(mutationKindNames, nameOf(mutationKindNames, kind)) == kind);
        for (const auto &[observation, name] : mutationObservationNames)
            CHECK(valueNamed(mutationObservationNames, nameOf(mutationObservationNames, observation)) == observation);
        CHECK(!valueNamed(recoveryActionNames, QStringLiteral("rename")));
    });
    suite.run("a mutation observation repeats its intent's target", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        Recorder recorder(directory.path() + "/store", capturedFile());
        const MutationFact intent{QUuid::createUuid().toString(QUuid::WithoutBraces), MutationKind::Staging, {}};
        const auto target = recorder.file;
        CHECK(recorder.lease->commit(mutationRecoveryTransaction(target, intent)).succeeded);
        recorder.step(RecoveryAction::ReplaceDraft, [](FileSession &file) {
            const auto end = file.current().draft.text.size();
            return file.editResult(file.current().id, {{end, end}, "later\n"});
        });
        auto observed = intent;
        observed.observation = MutationObservation::ObservedChanged;
        CHECK(recorder.lease->commit(mutationRecoveryTransaction(target, observed)).succeeded);
        const auto replay = replayRecovery(recorder.lease->store());
        CHECK(replay.succeeded && replay.mutations.size() == 1);
        const auto &mutation = replay.mutations.front();
        CHECK(mutation.id == intent.id && mutation.kind == MutationKind::Staging);
        CHECK(mutation.observation == MutationObservation::ObservedChanged);
        // The observation never replaces the later draft.
        CHECK(replay.files.front().current().draft == recorder.file.current().draft);
    });
    suite.run("replay rejects records whose values cannot be right", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        const auto path = directory.path() + "/store";
        {
            Recorder recorder(path, capturedFile());
            recorder.step(RecoveryAction::ReplaceDraft, [](FileSession &file) {
                const auto end = file.current().draft.text.size();
                return file.editResult(file.current().id, {{end, end}, "typed\n"});
            });
        }
        const auto withCurrent = [](const auto &change) {
            return [change](QJsonObject &state) {
                auto current = state.value("current").toObject();
                change(current);
                state.insert("current", current);
            };
        };
        const QString mismatch = "does not match the acknowledged record";
        rejected(path, withCurrent([](QJsonObject &current) { current.insert("id", "9"); }), mismatch);
        rejected(path, withCurrent([](QJsonObject &current) {
                     auto text = current.value("text").toObject();
                     text.insert("suffix", "999999");
                     current.insert("text", text);
                 }),
                 "Invalid recovered text change");
        rejected(path, withCurrent([](QJsonObject &current) {
                     auto text = current.value("text").toObject();
                     text.insert("prefix", "01");
                     current.insert("text", text);
                 }),
                 "Invalid recovery integer");
        rejected(path, withCurrent([](QJsonObject &current) { current.remove("handEdited"); }),
                 "Invalid recovered hand edits");
        rejected(path, withCurrent([](QJsonObject &current) {
                     current.insert("handEditedRanges", QJsonArray{QJsonObject{{"begin", "6"}, {"end", "0"}}});
                 }),
                 "Invalid recovered hand-edited range");
        rejected(path, withCurrent([](QJsonObject &current) {
                     current.insert("decisions", QJsonArray{QJsonObject{{"id", "x"}, {"value", "maybe"}}});
                 }),
                 "Unsupported recovery value");
        rejected(path, [](QJsonObject &state) { state.insert("action", "rename"); }, "Unsupported recovery value");
        rejected(path, [](QJsonObject &state) { state.insert("support", "outside-app"); }, mismatch);
        rejected(path, [](QJsonObject &state) { state.insert("schema", 1); }, "Unsupported recovery state schema");
        rejected(
            path, [](QJsonObject &state) { state.insert("action", "open"); },
            "Recovery attempted to replace an acknowledged session");
        // The record's inserted text is a payload it holds, so its reference names a real payload.
        const auto inserted = [](const QJsonObject &current) {
            return current.value("text").toObject().value("insert");
        };
        rejected(path, withCurrent([&](QJsonObject &current) {
                     const auto id = inserted(current);
                     current.insert("decisions",
                                    QJsonArray{QJsonObject{{"id", id}, {"value", "accepted"}},
                                               QJsonObject{{"id", id}, {"value", "ignored"}}});
                 }),
                 "Duplicate recovered decision");
        rejected(path, withCurrent([](QJsonObject &current) { current.insert("decisions", "none"); }),
                 "Invalid recovered draft");
        rejected(path, withCurrent([&](QJsonObject &current) {
                     const auto id = inserted(current);
                     current.insert("handEdited", QJsonArray{id, id});
                 }),
                 "Duplicate recovered hand edit");
        rejected(path, withCurrent([](QJsonObject &current) {
                     auto text = current.value("text").toObject();
                     text.insert("insert", 7);
                     current.insert("text", text);
                 }),
                 "Missing recovery payload reference");
        rejected(path, withCurrent([](QJsonObject &current) {
                     auto text = current.value("text").toObject();
                     text.insert("insert", QString(64, QLatin1Char('0')));
                     current.insert("text", text);
                 }),
                 "Recovery payload is missing, unsafe or corrupt");
    });
    suite.run("replay rejects a launch record whose captures cannot be right", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        // Only the launch record, so the last record is the one with the captures.
        const auto path = directory.path() + "/store";
        {
            Recorder recorder(path, capturedFile());
        }
        const auto withSnapshots = [](const auto &change) {
            return [change](QJsonObject &state) {
                auto snapshots = state.value("snapshots").toObject();
                change(snapshots);
                state.insert("snapshots", snapshots);
            };
        };
        // Another payload of the record, whose bytes are neither a UUID nor a file mode.
        const auto pathReference = [](const QJsonObject &snapshots) { return snapshots.value("path"); };
        rejected(path, [](QJsonObject &state) { state.insert("snapshots", 5); }, "Invalid recovery capture");
        rejected(path, withSnapshots([&](QJsonObject &snapshots) {
                     auto provenance = snapshots.value("provenance").toObject();
                     provenance.insert("sessionId", pathReference(snapshots));
                     snapshots.insert("provenance", provenance);
                 }),
                 "Invalid recovery session identity");
        rejected(path, withSnapshots([&](QJsonObject &snapshots) {
                     auto left = snapshots.value("stage2").toObject();
                     left.insert("mode", pathReference(snapshots));
                     snapshots.insert("stage2", left);
                 }),
                 "Invalid recovered source identity");
        rejected(path, withSnapshots([](QJsonObject &snapshots) {
                     auto launch = snapshots.value("launchResult").toObject();
                     auto identity = launch.value("identity").toObject();
                     identity.insert("mode", "4294967296");
                     launch.insert("identity", identity);
                     snapshots.insert("launchResult", launch);
                 }),
                 "Invalid recovery file mode");
        rejected(path, withSnapshots([](QJsonObject &snapshots) {
                     auto conversion = snapshots.value("conversion").toObject();
                     conversion.insert("attributes", "text");
                     snapshots.insert("conversion", conversion);
                 }),
                 "Invalid recovery conversion policy");
        rejected(path, withSnapshots([](QJsonObject &snapshots) {
                     auto conversion = snapshots.value("conversion").toObject();
                     const auto attributes = conversion.value("attributes").toArray();
                     CHECK(!attributes.isEmpty());
                     conversion.insert("attributes", QJsonArray{attributes.first(), attributes.first()});
                     snapshots.insert("conversion", conversion);
                 }),
                 "Duplicate recovery conversion policy entry");
    });
    return suite.report();
}
