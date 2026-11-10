// Continuation preserves domain histories while bounded discovery refuses incomplete inventories.
#include "TestHarness.h"
#include "core/ExactLineCorrespondence.h"
#include "fixtures/Await.h"
#include "fixtures/ChoiceFile.h"
#include "fixtures/Files.h"
#include "fixtures/SyncFaults.h"
#include "recovery/RecoveryRestore.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QUuid>
#include <cerrno>

using namespace branch_truce;
using namespace branch_truce::storage;
namespace {
class WholeText final : public CorrespondenceAlgorithm {
public:
    std::string_view identity() const noexcept override { return "whole-text/checkpoint"; }
    std::vector<Passage> compare(std::string_view source, std::string_view result) const override
    {
        return {
            {{0, source.size()}, {0, result.size()}, source == result ? PassageKind::Equal : PassageKind::Ambiguous}};
    }
};
FileSession choiceFile(const std::shared_ptr<const CorrespondenceAlgorithm> &algorithm)
{
    const auto original = test_support::choiceFile();
    return FileSession(original.id(), original.path(), {original.current().draft.text, {}, 0},
                       {.snapshots = *original.snapshots(), .initializeChoices = true, .algorithm = algorithm});
}
const std::vector<std::shared_ptr<const CorrespondenceAlgorithm>> algorithms{
    std::make_shared<const ExactLineCorrespondence>(), std::make_shared<const WholeText>()};
RecoveryLimits smallLimits()
{
    RecoveryLimits limits;
    limits.transactions = 4;
    return limits;
}
std::shared_ptr<RecoveryLease> leased(const QString &path, std::shared_ptr<const FileSystem> files = systemFileSystem())
{
    auto lease = RecoveryLease::acquire(path, std::move(files), nullptr, smallLimits());
    CHECK(lease);
    return lease;
}
void equivalent(const FileSession &actual, const FileSession &expected)
{
    CHECK(recoveryFileCheckpoint(actual).state == recoveryFileCheckpoint(expected).state);
    CHECK(actual.facts().blockers == expected.facts().blockers);
    CHECK(actual.canUndo() == expected.canUndo() && actual.canRedo() == expected.canRedo());
    for (const auto &revision : SessionCheckpoint::capture(expected).history.revisions) {
        auto a = actual;
        auto b = expected;
        CHECK(a.recordCheckpoint(revision->id, Outcome::Failed) == b.recordCheckpoint(revision->id, Outcome::Failed));
    }
    if (const auto &review = expected.publication().reviewed()) {
        auto a = actual;
        auto b = expected;
        CHECK(a.recordWrite(review->revision, Outcome::Failed) == b.recordWrite(review->revision, Outcome::Failed));
    }
}
void replayEquals(const RecoveryLease &lease, const FileSession &expected)
{
    const auto replay = replayRecovery(lease.store(), expected.algorithm());
    CHECK(replay.succeeded && replay.files.size() == 1);
    equivalent(replay.files.front(), expected);
}
void commit(const std::shared_ptr<RecoveryLease> &lease, const FileSession &file, RecoveryAction action,
            std::string_view previous)
{
    const auto transaction =
        recoveryTransaction(file, action, QUuid::createUuid().toString(QUuid::WithoutBraces), previous);
    CHECK(lease->commit(transaction).succeeded);
    CHECK(lease->commit(transaction).succeeded);
    replayEquals(*lease, file);
}
void edit(const std::shared_ptr<RecoveryLease> &lease, FileSession &file, const std::string &text)
{
    const auto previous = file.current().draft.text;
    auto draft = file.current().draft;
    draft.text += text;
    CHECK(file.replaceDraft(file.current().id, draft) == Transition::Applied);
    commit(lease, file, RecoveryAction::ReplaceDraft, previous);
}
class RolloverFault final : public test_support::SyncFaults {
public:
    enum class Fault { None, Rename, AfterRenameSync, Cleanup };
    mutable Fault fault = Fault::None;
    mutable bool published = false;
    int rename(int fromDirectory, const char *from, int toDirectory, const char *to) const override
    {
        const bool head = std::string_view(from).ends_with("/journal-next");
        if (head && fault == Fault::Rename) {
            fault = Fault::None;
            errno = EIO;
            return -1;
        }
        const auto result = FileSystem::rename(fromDirectory, from, toDirectory, to);
        if (head) published = result == 0;
        return result;
    }
    int sync(int fd) const override
    {
        struct stat status{};
        if (fault == Fault::AfterRenameSync && published && ::fstat(fd, &status) == 0 && S_ISDIR(status.st_mode)) {
            fault = Fault::None;
            errno = EIO;
            return -1;
        }
        return SyncFaults::sync(fd);
    }
    int unlink(int directory, const char *name, int flags) const override
    {
        if (fault == Fault::Cleanup && std::string_view(name).find("/payloads/") != std::string_view::npos) {
            fault = Fault::None;
            errno = EIO;
            return -1;
        }
        return FileSystem::unlink(directory, name, flags);
    }
};
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    test_support::configure(argc, argv);
    test_support::Suite suite("RecoveryContinuationTests");
    for (const auto &algorithm : algorithms) {
        suite.run("repeated continuations preserve decisions, hand edits, protected revisions and history cursor",
                  [algorithm] {
                      QTemporaryDir directory;
                      const auto lease = leased(directory.path() + "/store");
                      auto file = choiceFile(algorithm);
                      commit(lease, file, RecoveryAction::Open, {});
                      auto draft = file.current().draft;
                      draft.decisions.begin()->second.value = DecisionValue::Ignored;
                      const auto before = file.current().draft.text;
                      CHECK(file.replaceDraft(file.current().id, draft) == Transition::Applied);
                      commit(lease, file, RecoveryAction::ReplaceDraft, before);
                      CHECK(file.beginMarkResolved(file.current().id, true) == Transition::Applied);
                      commit(lease, file, RecoveryAction::MarkResolvedReview, file.current().draft.text);
                      CHECK(file.recordCheckpoint(file.current().id, Outcome::Succeeded) == Transition::Applied);
                      commit(lease, file, RecoveryAction::ManualSave, file.current().draft.text);
                      CHECK(file.recordWrite(file.current().id, Outcome::Succeeded) == Transition::Applied);
                      commit(lease, file, RecoveryAction::WriteOutcome, file.current().draft.text);
                      CHECK(file.recordStage(file.current().id, Outcome::Succeeded, "object", "100644")
                            == Transition::Applied);
                      commit(lease, file, RecoveryAction::StageOutcome, file.current().draft.text);
                      for (int i = 0; i < 18; ++i) {
                          edit(lease, file, "hand edit " + std::to_string(i) + "\n");
                          const auto previous = file.current().draft.text;
                          const auto action = i % 2 ? RecoveryAction::Undo : RecoveryAction::ManualSave;
                          if (i % 2)
                              CHECK(file.undo(file.current().id) == Transition::Applied);
                          else
                              CHECK(file.recordCheckpoint(file.current().id, Outcome::Succeeded)
                                    == Transition::Applied);
                          commit(lease, file, action, previous);
                          if (i % 2) {
                              const auto text = file.current().draft.text;
                              CHECK(file.redo(file.current().id) == Transition::Applied);
                              commit(lease, file, RecoveryAction::Redo, text);
                          }
                      }
                      CHECK(lease->store().read().transactions.size() <= 4);
                      const auto live = recoveryCheckpoint(lease->store(), algorithm);
                      CHECK(QDir(lease->directory() + "/payloads").entryList(QDir::Files).size()
                            <= live.payloads.size() + 16);
                      CHECK(!QFileInfo::exists(lease->directory() + "/journal-next"));
                      const auto snapshot = replayRecovery(lease->store(), algorithm).files.front();
                      CHECK(!replayRecovery(lease->store(), algorithms[algorithm == algorithms[0] ? 1 : 0]).succeeded);
                      auto replayUndo = snapshot;
                      auto liveUndo = file;
                      while (liveUndo.canUndo()) {
                          CHECK(liveUndo.undo(liveUndo.current().id) == Transition::Applied);
                          CHECK(replayUndo.undo(replayUndo.current().id) == Transition::Applied);
                          equivalent(replayUndo, liveUndo);
                      }
                      while (liveUndo.canRedo()) {
                          CHECK(liveUndo.redo(liveUndo.current().id) == Transition::Applied);
                          CHECK(replayUndo.redo(replayUndo.current().id) == Transition::Applied);
                          equivalent(replayUndo, liveUndo);
                      }
                      CHECK(file.publication().written() && file.publication().staged());
                      CHECK(!file.facts().resolved && file.facts().needsReview);
                      const auto previous = file.current().draft.text;
                      CHECK(file.discardToCheckpoint(file.current().id) == Transition::Applied);
                      commit(lease, file, RecoveryAction::DiscardToCheckpoint, previous);
                      edit(lease, file, "after discard\n");
                      const auto stagedPrevious = file.current().draft.text;
                      CHECK(file.discardToStaged(file.current().id) == Transition::Applied);
                      commit(lease, file, RecoveryAction::DiscardToStaged, stagedPrevious);
                      for (int i = 0; i < 4; ++i) edit(lease, file, "after staged discard\n");
                  });
    }
    for (const auto point :
         {RolloverFault::Fault::Rename, RolloverFault::Fault::AfterRenameSync, RolloverFault::Fault::Cleanup}) {
        for (const auto &algorithm : algorithms) {
            suite.run("failed head publication, directory sync and cleanup keep equivalent acknowledged history",
                      [point, algorithm] {
                          QTemporaryDir directory;
                          const auto files = std::make_shared<RolloverFault>();
                          const auto lease = leased(directory.path() + "/store", files);
                          auto file = choiceFile(algorithm);
                          commit(lease, file, RecoveryAction::Open, {});
                          for (int i = 0; i < 3; ++i) edit(lease, file, std::to_string(i));
                          const auto acknowledged = file;
                          auto draft = file.current().draft;
                          draft.text += "pending";
                          CHECK(file.replaceDraft(file.current().id, draft) == Transition::Applied);
                          const auto pending = recoveryTransaction(file, RecoveryAction::ReplaceDraft, "pending",
                                                                   acknowledged.current().draft.text);
                          files->fault = point;
                          CHECK(!lease->commit(pending).succeeded);
                          CHECK(files->fault == RolloverFault::Fault::None);
                          replayEquals(*lease, acknowledged);
                          CHECK(lease->commit(pending).succeeded && lease->commit(pending).succeeded);
                          replayEquals(*lease, file);
                      });
        }
    }
    for (const auto &algorithm : algorithms) {
        suite.run("uncertain flush IDs and exact unresolved mutation targets survive several continuations",
                  [algorithm] {
                      QTemporaryDir directory;
                      const auto files = std::make_shared<RolloverFault>();
                      const auto lease = leased(directory.path() + "/store", files);
                      auto file = choiceFile(algorithm);
                      commit(lease, file, RecoveryAction::Open, {});
                      const auto target = file;
                      const MutationFact fact{
                          QUuid::createUuid().toString(QUuid::WithoutBraces), MutationKind::Publication, {}};
                      const auto intent = mutationRecoveryTransaction(target, fact);
                      files->arm(test_support::SyncFaults::Point::JournalSync);
                      CHECK(!lease->commit(intent).succeeded);
                      for (int i = 0; i < 14; ++i) edit(lease, file, std::to_string(i));
                      CHECK(lease->commit(intent).succeeded);
                      auto observation = fact;
                      observation.observation = MutationObservation::Uncertain;
                      CHECK(lease->commit(mutationRecoveryTransaction(target, observation)).succeeded);
                      for (int i = 0; i < 4; ++i) edit(lease, file, "observed");
                      const auto replay = replayRecovery(lease->store(), algorithm);
                      CHECK(replay.succeeded && replay.mutations.size() == 1);
                      CHECK(replay.mutations.front().id == fact.id
                            && replay.mutations.front().observation == observation.observation);
                      equivalent(replay.mutations.front().target, target);
                      equivalent(replay.files.front(), file);
                  });
    }
    for (const auto point :
         {test_support::SyncFaults::Point::PayloadSync, test_support::SyncFaults::Point::DirectorySync}) {
        suite.run("checkpoint payload and prepublication sync failures preserve the original journal", [point] {
            QTemporaryDir directory;
            const auto files = std::make_shared<RolloverFault>();
            const auto lease = leased(directory.path() + "/store", files);
            auto file = test_support::choiceFile();
            commit(lease, file, RecoveryAction::Open, {});
            for (int i = 0; i < 3; ++i) edit(lease, file, std::to_string(i));
            const auto original = test_support::readFile(lease->directory() + "/journal");
            const auto previous = file.current().draft.text;
            auto draft = file.current().draft;
            draft.text += "pending";
            CHECK(file.replaceDraft(file.current().id, draft) == Transition::Applied);
            const auto pending = recoveryTransaction(file, RecoveryAction::ReplaceDraft, "pending", previous);
            files->arm(point);
            CHECK(!lease->commit(pending).succeeded && files->failures == 1);
            CHECK(test_support::readFile(lease->directory() + "/journal") == original);
            CHECK(lease->commit(pending).succeeded);
            replayEquals(*lease, file);
        });
    }
    suite.run("a pending transaction owns its configured algorithm until continuation completes", [] {
        auto algorithm = std::make_shared<const WholeText>();
        const std::weak_ptr<const CorrespondenceAlgorithm> lifetime = algorithm;
        auto transaction = recoveryTransaction(choiceFile(algorithm), RecoveryAction::Open, "owned", {});
        algorithm.reset();
        CHECK(!lifetime.expired());
        QTemporaryDir directory;
        const auto lease = leased(directory.path() + "/store");
        CHECK(lease->commit(transaction).succeeded);
        CHECK(transaction.continuation(lease->store()).state.contains("checkpointSchema"));
        transaction = {};
        CHECK(lifetime.expired());
    });
    suite.run("unsupported checkpoint schema and inconsistent history are rejected", [] {
        QTemporaryDir directory;
        const auto source = leased(directory.path() + "/source");
        auto file = test_support::choiceFile();
        commit(source, file, RecoveryAction::Open, {});
        edit(source, file, "unsaved");
        auto checkpoint = recoveryCheckpoint(source->store());
        for (int i = 0; i < 2; ++i) {
            auto invalid = checkpoint;
            if (i == 0)
                invalid.state.insert("checkpointSchema", 99);
            else {
                auto entries = invalid.state.value("files").toArray();
                auto entry = entries[0].toObject();
                auto state = entry.value("file").toObject();
                auto history = state.value("history").toObject();
                history.insert("next", "1");
                state.insert("history", history);
                entry.insert("file", state);
                entries[0] = entry;
                invalid.state.insert("files", entries);
            }
            const auto destination = leased(directory.path() + QString("/invalid-%1").arg(i));
            CHECK(destination->commit(invalid).succeeded);
            const auto replay = replayRecovery(destination->store());
            CHECK(!replay.succeeded && replay.files.empty());
        }
    });
    suite.run("undeclared checkpoint payloads are rejected before reclamation can use their inventory", [] {
        QTemporaryDir directory;
        const auto source = leased(directory.path() + "/source");
        commit(source, test_support::choiceFile(), RecoveryAction::Open, {});
        auto checkpoint = recoveryCheckpoint(source->store());
        const auto destination = leased(directory.path() + "/destination");
        CHECK(QDir().mkpath(destination->directory() + "/payloads"));
        for (auto it = checkpoint.payloads.cbegin(); it != checkpoint.payloads.cend(); ++it) {
            const auto path = destination->directory() + "/payloads/" + QString::fromLatin1(it.key());
            test_support::writeFile(path, it.value());
            CHECK(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        }
        checkpoint.payloads.clear();
        CHECK(destination->commit(checkpoint).succeeded);
        CHECK(!replayRecovery(destination->store()).succeeded);
    });
    suite.run("confirmed large-file admission survives continuation", [] {
        QTemporaryDir directory;
        const auto lease = leased(directory.path() + "/store");
        std::string text;
        for (int i = 0; i < 4001; ++i) text += "x\n";
        auto file = test_support::choiceFile(text, text);
        commit(lease, file, RecoveryAction::Open, {});
        CHECK(file.loadLargeFile() == Transition::Applied);
        commit(lease, file, RecoveryAction::LoadLarge, text);
        for (int i = 0; i < 6; ++i) edit(lease, file, "x");
    });
    suite.run("a live checkpoint over the injected envelope refuses without discarding input", [] {
        QTemporaryDir directory;
        RecoveryLimits limits = smallLimits();
        limits.transactions = 1;
        auto lease = RecoveryLease::acquire(directory.path() + "/store", systemFileSystem(), nullptr, limits);
        auto file = test_support::choiceFile();
        CHECK(lease->commit(recoveryTransaction(file, RecoveryAction::Open, "open", {})).succeeded);
        const auto original = test_support::readFile(lease->directory() + "/journal");
        auto draft = file.current().draft;
        draft.text += "retained input";
        CHECK(file.replaceDraft(file.current().id, draft) == Transition::Applied);
        const auto pending = recoveryTransaction(file, RecoveryAction::ReplaceDraft, "pending",
                                                 file.snapshots()->launchResult.text.utf8);
        CHECK(!lease->commit(pending).succeeded);
        CHECK(test_support::readFile(lease->directory() + "/journal") == original);
        CHECK(pending.payloads.contains(RecoveryStore::digest("retained input")));
    });
    suite.run("an oversized live checkpoint is refused before publishing its head", [] {
        QTemporaryDir directory;
        const auto probe = leased(directory.path() + "/probe");
        auto file = test_support::choiceFile();
        commit(probe, file, RecoveryAction::Open, {});
        const auto bytes = test_support::readFile(probe->directory() + "/journal");
        auto limits = smallLimits();
        limits.transactions = 2;
        limits.frameBytes = bytes.size() - bytes.indexOf('\n') - 1;
        const auto lease = RecoveryLease::acquire(directory.path() + "/bounded", systemFileSystem(), nullptr, limits);
        commit(lease, file, RecoveryAction::Open, {});
        edit(lease, file, "first");
        const auto original = test_support::readFile(lease->directory() + "/journal");
        const auto before = file.current().draft.text;
        auto draft = file.current().draft;
        draft.text += "second";
        CHECK(file.replaceDraft(file.current().id, draft) == Transition::Applied);
        const auto pending = recoveryTransaction(file, RecoveryAction::ReplaceDraft, "pending", before);
        CHECK(lease->commit(pending).problem.code == ProblemCode::RecoveryResourceLimit);
        CHECK(test_support::readFile(lease->directory() + "/journal") == original);
    });
    suite.run("cross-file asynchronous FIFO continues and shutdown drains it", [] {
        QTemporaryDir directory;
        const auto path = directory.path() + "/store";
        {
            SessionStore store(path, systemFileSystem(), nullptr, smallLimits());
            CHECK(store.appendFile(FileSession("a", "a.txt", {"a\n", {}, 0})));
            CHECK(store.appendFile(FileSession("b", "b.txt", {"b\n", {}, 0})));
            test_support::awaitRecovery(store);
            for (int i = 0; i < 8; ++i) {
                auto a = store.editResult("a", store.file("a")->current().id, {{0, 0}, "a"});
                auto b = store.editResult("b", store.file("b")->current().id, {{0, 0}, "b"});
                CHECK(test_support::await(a) == Transition::Applied && test_support::await(b) == Transition::Applied);
            }
            CHECK(store.typeResult("a", store.file("a")->current().id, {{0, 0}, "last"}) == Transition::Applied);
        }
        const auto replay = replayRecovery(RecoveryStore(path));
        CHECK(replay.succeeded && replay.files.size() == 2);
        CHECK(replay.files[0].current().draft.text == "lastaaaaaaaaa\n");
        CHECK(replay.files[1].current().draft.text == "bbbbbbbbb\n");
    });
    suite.run("many matching or malformed entries refuse discovery without a prefix adoption", [] {
        QTemporaryDir directory;
        for (int i = 0; i < 6; ++i) {
            const auto lease = leased(directory.path() + QString("/session-%1").arg(i));
            auto file = test_support::choiceFile();
            commit(lease, file, RecoveryAction::Open, {});
            edit(lease, file, "unsaved\n");
        }
        git::RepositoryContext repository;
        const auto captures = test_support::choiceFile().snapshots()->provenance;
        repository.worktreeRoot = QString::fromStdString(captures.worktreeRoot);
        repository.gitDirectory = QString::fromStdString(captures.gitDirectory);
        repository.supported = true;
        repository.operation = git::RepositoryOperation::Merge;
        repository.stage2.commitId = QByteArray::fromStdString(captures.stage2CommitId);
        repository.stage3.commitId = QByteArray::fromStdString(captures.stage3CommitId);
        RestoreOptions options;
        options.inventoryLimits.sessions = 2;
        const auto refused = restoreRecovery(directory.path(), {}, repository, {}, {}, "session-0", options);
        CHECK(!refused.found && refused.choices.empty() && refused.diagnostics.size() == 1);
        CHECK(refused.diagnostics.front().code == ProblemCode::RecoverySessionRejected);
        for (int i = 0; i < 6; ++i) CHECK(QFileInfo::exists(directory.path() + QString("/session-%1/journal").arg(i)));
        options.inventoryLimits.sessions = 32;
        options.inventoryLimits.candidates = 1;
        const auto capped = restoreRecovery(directory.path(), {}, repository, {}, {}, {}, options);
        CHECK(!capped.found && capped.choices.empty() && capped.diagnostics.size() == 1);
        options.inventoryLimits.candidates = 16;
        options.inventoryLimits.decodedBytes = 0;
        CHECK(!restoreRecovery(directory.path(), {}, repository, {}, {}, {}, options).found);
        QTemporaryDir malformed;
        for (int i = 0; i < 10; ++i) CHECK(QDir().mkpath(malformed.path() + QString("/bad-%1").arg(i)));
        options.inventoryLimits.entries = 3;
        CHECK(!restoreRecovery(malformed.path(), {}, repository, {}, {}, {}, options).diagnostics.empty());
    });
    return suite.report();
}
