#include "TestHarness.h"
#include "core/ExactLineCorrespondence.h"
#include "fixtures/ChoiceFile.h"
#include "fixtures/Files.h"
#include "fixtures/SyncFaults.h"
#include "recovery/RecoveryRestore.h"
#include "storage/RecoveryStore.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <atomic>
#include <limits>

using namespace branch_truce;
using namespace branch_truce::storage;
using test_support::readFile;
using test_support::writeFile;

namespace {
RecoveryTransaction transaction(const QString &id, const QByteArray &bytes)
{
    const auto hash = RecoveryStore::digest(bytes);
    return {id, {{"text", QString::fromLatin1(hash)}}, {{hash, bytes}}};
}

std::shared_ptr<RecoveryLease> leased(const QString &path, RecoveryLimits limits = {},
                                      std::shared_ptr<const FileSystem> files = systemFileSystem())
{
    auto result = RecoveryLease::acquire(path, std::move(files), nullptr, limits);
    CHECK(result != nullptr);
    return result;
}

void privateFile(const QString &path, const QByteArray &bytes)
{
    writeFile(path, bytes);
    CHECK(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
}

void limited(const RecoveryCommit &result)
{
    CHECK(!result.succeeded && result.problem.code == ProblemCode::RecoveryResourceLimit);
}
void limited(const RecoveryRead &result)
{
    CHECK(!result.succeeded && result.problem.code == ProblemCode::RecoveryResourceLimit);
}

// Cost uses the wire bytes, independently of the admission implementation.
qint64 stateCost(const QByteArray &frame)
{
    const auto body =
        QByteArray::fromBase64(QJsonDocument::fromJson(frame).object().value("record").toString().toLatin1());
    const auto id = QJsonDocument::fromJson(body).object().value("transaction").toString();
    return body.size() + id.size() * 2 + 256;
}

class CountingAlgorithm final : public CorrespondenceAlgorithm {
public:
    mutable std::atomic<int> calls = 0;
    std::vector<Passage> compare(std::string_view source, std::string_view result) const override
    {
        ++calls;
        return ExactLineCorrespondence().compare(source, result);
    }
    std::string_view identity() const noexcept override { return ExactLineCorrespondence{}.identity(); }
};
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    test_support::configure(argc, argv);
    test_support::Suite suite("RecoveryResourceTests");
    suite.run("header and frame reads reject oversized and unterminated input within small bounds", [] {
        QTemporaryDir directory;
        const auto lease = leased(directory.path() + "/store");
        CHECK(lease->commit(transaction("first", "kept")).succeeded);
        const auto path = lease->directory() + "/journal";
        const auto original = readFile(path);
        const auto header = original.left(original.indexOf('\n') + 1);
        RecoveryLimits limits;
        limits.frameBytes = 64;
        for (const auto &bytes : {QByteArray(129, 'h'), QByteArray(128, 'h') + '\n', header + QByteArray(65, 'f'),
                                  header + QByteArray(64, 'f') + '\n'}) {
            privateFile(path, bytes);
            limited(RecoveryStore(lease->directory(), systemFileSystem(), limits).read());
            CHECK(readFile(path) == bytes);
        }
        privateFile(path, header.chopped(1));
        CHECK(RecoveryStore(lease->directory()).read().problem.code == ProblemCode::RecoveryJournalVersion);
        privateFile(path, header + QByteArray(64, 'f'));
        const auto tail = RecoveryStore(lease->directory(), systemFileSystem(), limits).read();
        CHECK(tail.succeeded && tail.incompleteTail && tail.transactions.empty());
        privateFile(path, original + "partial");
        const auto prefix = RecoveryStore(lease->directory()).read();
        CHECK(prefix.succeeded && prefix.incompleteTail && prefix.transactions.size() == 1);
    });
    suite.run("each admission budget accepts its exact boundary and refuses before store mutation", [] {
        QTemporaryDir directory;
        auto first = transaction("a", "one");
        first.retainedBytes = 3;
        auto second = transaction("a-longer-identity", "second payload");
        second.retainedBytes = 3;
        const auto probe = leased(directory.path() + "/probe");
        CHECK(probe->commit(first).succeeded);
        const auto journal = readFile(probe->directory() + "/journal");
        const auto frame = journal.mid(journal.indexOf('\n') + 1);
        const std::pair<qint64 RecoveryLimits::*, qint64> budgets[] = {
            {&RecoveryLimits::frameBytes, frame.size()},
            {&RecoveryLimits::journalBytes, journal.size()},
            {&RecoveryLimits::stateBytes, stateCost(frame)},
            {&RecoveryLimits::expandedBytes, first.retainedBytes},
            {&RecoveryLimits::transactions, 1},
            {&RecoveryLimits::payloadFiles, 1},
            {&RecoveryLimits::storeBytes, journal.size() + 3}};
        int index = 0;
        for (const auto &[member, boundary] : budgets) {
            RecoveryLimits limits;
            limits.*member = boundary;
            const auto lease = leased(directory.path() + QString("/at-%1").arg(index), limits);
            CHECK(lease->commit(first).succeeded);
            auto changed = first;
            ++changed.retainedBytes;
            CHECK(lease->commit(changed).problem.code == ProblemCode::RecoveryTransactionReused);
            const auto before = readFile(lease->directory() + "/journal");
            const auto payloads = QDir(lease->directory() + "/payloads").entryList();
            limited(lease->commit(second));
            CHECK(readFile(lease->directory() + "/journal") == before);
            CHECK(QDir(lease->directory() + "/payloads").entryList() == payloads);
            CHECK(lease->commit(first).succeeded);
            CHECK(RecoveryStore(lease->directory(), systemFileSystem(), limits).read().succeeded);
            limits.*member = boundary - 1;
            const auto below = leased(directory.path() + QString("/below-%1").arg(index++), limits);
            limited(below->commit(first));
            CHECK(!QFileInfo::exists(below->directory() + "/journal"));
            CHECK(!QFileInfo::exists(below->directory() + "/payloads"));
            limited(RecoveryStore(lease->directory(), systemFileSystem(), limits).read());
        }
    });
    suite.run("aggregate state accounting stops before the next record and clamps huge limits", [] {
        QTemporaryDir directory;
        const auto probe = leased(directory.path() + "/probe");
        CHECK(probe->commit(transaction("a", "one")).succeeded);
        CHECK(probe->commit(transaction("b", "two")).succeeded);
        const auto lines = readFile(probe->directory() + "/journal").split('\n');
        RecoveryLimits limits;
        limits.stateBytes = stateCost(lines[1]) + stateCost(lines[2]) - 1;
        limited(RecoveryStore(probe->directory(), systemFileSystem(), limits).read());
        const auto lease = leased(directory.path() + "/bounded", limits);
        CHECK(lease->commit(transaction("a", "one")).succeeded);
        limited(lease->commit(transaction("b", "two")));
        limits.frameBytes = std::numeric_limits<qint64>::max();
        limits.journalBytes = std::numeric_limits<qint64>::max();
        limits.stateBytes = std::numeric_limits<qint64>::max();
        limits.storeBytes = std::numeric_limits<qint64>::max();
        CHECK(RecoveryStore(probe->directory(), systemFileSystem(), limits).read().succeeded);
        limits.storeBytes = -1;
        limited(RecoveryStore(probe->directory(), systemFileSystem(), limits).read());
        auto oversized = transaction("overflow", "kept");
        oversized.retainedBytes = std::numeric_limits<qint64>::max();
        limited(lease->commit(oversized));
        oversized.retainedBytes = -1;
        limited(lease->commit(oversized));
    });
    suite.run("payload and transaction identity limits reject without growing the journal", [] {
        QTemporaryDir directory;
        const auto lease = leased(directory.path() + "/store");
        CHECK(lease->commit(transaction(QString(256, 'i'), "kept")).succeeded);
        const auto before = readFile(lease->directory() + "/journal");
        limited(lease->commit(transaction(QString(257, 'i'), "rejected")));
        CHECK(readFile(lease->directory() + "/journal") == before);
        const auto hash = RecoveryStore::digest("sparse");
        const auto path = lease->directory() + "/payloads/" + QString::fromLatin1(hash);
        privateFile(path, {});
        QFile sparse(path);
        CHECK(sparse.open(QIODevice::WriteOnly) && sparse.resize(8388609));
        sparse.close();
        Problem error;
        CHECK(lease->store().payload(hash, &error).isEmpty());
        CHECK(error.code == ProblemCode::RecoveryResourceLimit);
    });
    for (const auto point :
         {test_support::SyncFaults::Point::PayloadSync, test_support::SyncFaults::Point::DirectorySync,
          test_support::SyncFaults::Point::JournalSync}) {
        suite.run("failed writes preserve history and retry at the payload and transaction capacity", [point] {
            QTemporaryDir directory;
            RecoveryLimits limits;
            limits.transactions = 2;
            limits.payloadFiles = 2;
            const auto files = std::make_shared<test_support::SyncFaults>();
            auto lease = leased(directory.path() + "/store", limits, files);
            const auto first = transaction("a", "kept");
            const auto pending = transaction("b", "pending");
            CHECK(lease->commit(first).succeeded);
            {
                test_support::ArmedFault fault(*files, point);
                CHECK(!lease->commit(pending).succeeded);
                CHECK(files->failures == 1);
            }
            lease.reset(); // Reopen as after a process exit, without cache or pending directory state.
            lease = leased(directory.path() + "/store", limits, files);
            CHECK(lease->commit(pending).succeeded && lease->commit(pending).succeeded);
            const auto read = lease->store().read();
            CHECK(read.succeeded && read.transactions.size() == 2);
            CHECK(read.transactions.front().state == first.state && read.transactions.back().state == pending.state);
            const auto before = readFile(lease->directory() + "/journal");
            limited(lease->commit(transaction("c", "rejected")));
            CHECK(readFile(lease->directory() + "/journal") == before);
        });
    }
    suite.run("orphan payloads count toward admission and a rejected append keeps an interrupted tail", [] {
        QTemporaryDir directory;
        RecoveryLimits limits;
        limits.payloadFiles = 2;
        const auto lease = leased(directory.path() + "/store", limits);
        const auto first = transaction("a", "kept");
        CHECK(lease->commit(first).succeeded);
        const auto orphan = RecoveryStore::digest("orphan");
        privateFile(lease->directory() + "/payloads/" + QString::fromLatin1(orphan), "orphan");
        const auto path = lease->directory() + "/journal";
        const auto bytes = readFile(path) + "unfinished";
        privateFile(path, bytes);
        limited(lease->commit(transaction("b", "new")));
        CHECK(readFile(path) == bytes);
        CHECK(lease->commit(first).succeeded);
        CHECK(readFile(path) == bytes);
        const auto read = lease->store().read();
        CHECK(read.succeeded && read.incompleteTail && read.transactions.size() == 1);
    });
    suite.run("capacity preserves captures, checkpoints, decisions and unobserved Git intents on replay", [] {
        QTemporaryDir directory;
        RecoveryLimits limits;
        limits.transactions = 4;
        const auto lease = leased(directory.path() + "/store", limits);
        auto file = test_support::choiceFile();
        const auto captures = *file.snapshots();
        CHECK(lease->commit(recoveryTransaction(file, RecoveryAction::Open, "open", {})).succeeded);
        const auto before = file.current().draft.text;
        auto draft = file.current().draft;
        draft.text += "pending review\n";
        CHECK(file.replaceDraft(file.current().id, draft) == Transition::Applied);
        CHECK(lease->commit(recoveryTransaction(file, RecoveryAction::ReplaceDraft, "edit", before)).succeeded);
        CHECK(file.recordCheckpoint(file.current().id, Outcome::Succeeded) == Transition::Applied);
        CHECK(lease->commit(recoveryTransaction(file, RecoveryAction::ManualSave, "save", file.current().draft.text))
                  .succeeded);
        const auto intent =
            mutationRecoveryTransaction(file, {"123bd2be-f830-477a-8077-1e15c4725d09", MutationKind::Publication, {}});
        CHECK(lease->commit(intent).succeeded);
        limited(lease->commit(transaction("exhausted", "new input")));
        const auto replay = replayRecovery(lease->store());
        CHECK(replay.succeeded && replay.files.size() == 1 && replay.mutations.size() == 1);
        CHECK(*replay.files.front().snapshots() == captures);
        CHECK(replay.files.front().current().draft == file.current().draft);
        CHECK(replay.files.front().publication().checkpoint() == file.publication().checkpoint());
        CHECK(!replay.mutations.front().observation && !replay.mutations.front().completed);
        CHECK(lease->commit(intent).succeeded);
    });
    suite.run("short deltas still consume the full reconstructed draft budget before admission", [] {
        QTemporaryDir directory;
        auto file = test_support::choiceFile();
        const auto open = recoveryTransaction(file, RecoveryAction::Open, "open", {});
        const auto before = file.current().draft.text;
        auto draft = file.current().draft;
        draft.text += "x";
        CHECK(file.replaceDraft(file.current().id, draft) == Transition::Applied);
        const auto edit = recoveryTransaction(file, RecoveryAction::ReplaceDraft, "edit", before);
        CHECK(edit.retainedBytes >= static_cast<qint64>(draft.text.size()));
        RecoveryLimits limits;
        limits.expandedBytes = open.retainedBytes + edit.retainedBytes - 1;
        const auto lease = leased(directory.path() + "/store", limits);
        CHECK(lease->commit(open).succeeded);
        const auto journal = readFile(lease->directory() + "/journal");
        limited(lease->commit(edit));
        CHECK(readFile(lease->directory() + "/journal") == journal);
        const auto replay = replayRecovery(lease->store());
        CHECK(replay.succeeded && replay.files.front().current().draft.text == before);
    });
    suite.run("a forged small expanded budget fails before rebuilding payload references", [] {
        QTemporaryDir directory;
        const auto lease = leased(directory.path() + "/store");
        const auto open = recoveryTransaction(test_support::choiceFile(), RecoveryAction::Open, "open", {});
        CHECK(open.retainedBytes > 0 && lease->commit(open).succeeded);
        const auto path = lease->directory() + "/journal";
        auto lines = readFile(path).split('\n');
        const auto frame = QJsonDocument::fromJson(lines[1]).object();
        auto record =
            QJsonDocument::fromJson(QByteArray::fromBase64(frame.value("record").toString().toLatin1())).object();
        record.insert("retained", "0");
        const auto body = QJsonDocument(record).toJson(QJsonDocument::Compact);
        lines[1] = QJsonDocument(QJsonObject{{"record", QString::fromLatin1(body.toBase64())},
                                             {"sha256", QString::fromLatin1(RecoveryStore::digest(body))}})
                       .toJson(QJsonDocument::Compact);
        privateFile(path, lines.join('\n'));
        const auto replay = replayRecovery(lease->store());
        CHECK(!replay.succeeded && replay.files.empty());
        CHECK(replay.problem.code == ProblemCode::RecoveryResourceLimit);
        auto exaggerated = open;
        exaggerated.retainedBytes += 1;
        const auto other = leased(directory.path() + "/other");
        CHECK(other->commit(exaggerated).succeeded);
        CHECK(!replayRecovery(other->store()).succeeded);
    });
    suite.run("inventory skips unrelated captures before payload validation and alignment", [] {
        QTemporaryDir directory;
        auto lease = leased(directory.path() + "/store");
        auto file = test_support::choiceFile();
        const auto open = recoveryTransaction(file, RecoveryAction::Open, "open", {});
        CHECK(lease->commit(open).succeeded);
        const auto root = file.snapshots()->provenance.worktreeRoot;
        const auto source = RecoveryStore::digest(QByteArray::fromStdString(file.snapshots()->stage2->rawBytes));
        CHECK(QFile::remove(lease->directory() + "/payloads/" + QString::fromLatin1(source)));
        CHECK(!replayRecovery(lease->store()).succeeded);
        lease.reset();
        auto algorithm = std::make_shared<CountingAlgorithm>();
        git::RepositoryContext repository;
        repository.worktreeRoot = QString::fromStdString(root) + "/unrelated";
        const RestoreOptions options{.correspondence = algorithm, .now = {}};
        const auto skipped = restoreRecovery(directory.path(), {}, repository, {}, {}, {}, options);
        CHECK(!skipped.found && skipped.diagnostics.empty() && algorithm->calls == 0);
        repository.worktreeRoot = QString::fromStdString(root);
        const auto matching = restoreRecovery(directory.path(), {}, repository, {}, {}, {}, options);
        CHECK(!matching.found && !matching.diagnostics.empty());
        const auto hash = RecoveryStore::digest(QByteArray::fromStdString(root));
        privateFile(directory.path() + "/store/payloads/" + QString::fromLatin1(hash), "/redirected");
        repository.worktreeRoot = "/another";
        const auto unknown = restoreRecovery(directory.path(), {}, repository, {}, {}, {}, options);
        CHECK(!unknown.found && !unknown.diagnostics.empty());
        CHECK(QFileInfo::exists(directory.path() + "/store/journal"));
    });
    return suite.report();
}
