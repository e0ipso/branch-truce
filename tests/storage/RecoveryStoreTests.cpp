#include "TestHarness.h"
#include "core/ResourceLimits.h"
#include "fixtures/Files.h"
#include "fixtures/SyncFaults.h"
#include "storage/RecoveryStore.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QProcess>
#include <QTemporaryDir>
#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>

using namespace branch_truce::storage;
using test_support::ArmedFault;
using test_support::readFile;
using test_support::SyncFaults;
using test_support::writeFile;

namespace {
RecoveryTransaction transaction(const QString &id, const QByteArray &bytes)
{
    const auto hash = RecoveryStore::digest(bytes);
    return {{{"schema", 1}, {"transactionId", id}, {"text", QString::fromLatin1(hash)}}, {{hash, bytes}}};
}
void overwritePrivate(const QString &path, const QByteArray &bytes)
{
    writeFile(path, bytes);
    CHECK(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
}

// Rewrites the journal's last record with `mutate` and gives it a matching checksum, so only
// the record's own validation can refuse it.
void rewriteLastRecord(const QString &directory, void (*mutate)(QJsonObject &))
{
    const auto path = directory + "/journal";
    auto lines = readFile(path).split('\n');
    CHECK(lines.size() >= 3 && lines.back().isEmpty());
    auto &line = lines[lines.size() - 2];
    const auto frame = QJsonDocument::fromJson(line).object();
    auto record = QJsonDocument::fromJson(QByteArray::fromBase64(frame.value("record").toString().toLatin1())).object();
    CHECK(!record.isEmpty());
    mutate(record);
    const auto body = QJsonDocument(record).toJson(QJsonDocument::Compact);
    line = QJsonDocument(QJsonObject{{"record", QString::fromLatin1(body.toBase64())},
                                     {"sha256", QString::fromLatin1(RecoveryStore::digest(body))}})
               .toJson(QJsonDocument::Compact);
    writeFile(path, lines.join('\n'));
}

// Records that keep a valid checksum but break the journal's schema or order.
const std::pair<const char *, void (*)(QJsonObject &)> invalidRecords[] = {
    {"a sequence out of order", [](QJsonObject &record) { record.insert("sequence", "3"); }},
    {"a broken chain to the previous record",
     [](QJsonObject &record) { record.insert("previous", QString(64, QLatin1Char('0'))); }},
    {"another record version", [](QJsonObject &record) { record.insert("version", 2); }},
    {"another state schema",
     [](QJsonObject &record) {
         auto state = record.value("state").toObject();
         state.insert("schema", 2);
         record.insert("state", state);
     }},
    {"an empty transaction identity", [](QJsonObject &record) {
         auto state = record.value("state").toObject();
         state.insert("transactionId", "");
         record.insert("state", state);
     }}};

// The largest recovery payload the store accepts.
constexpr auto limit = static_cast<qsizetype>(branch_truce::kMaxRecoveryPayloadBytes);
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    test_support::configure(argc, argv);
    test_support::Suite suite("RecoveryStoreTests");
    suite.run("interrupted transaction tails preserve the acknowledged prefix and retry once", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        RecoveryStore store(directory.path() + "/recovery");
        const auto first = transaction("acknowledged", QByteArray("first\0exact bytes", 17));
        const auto second = transaction("interrupted", "second complete edit and decisions");
        CHECK(store.commit(first).succeeded);
        const auto journalPath = store.directory() + "/journal";
        const auto acknowledged = readFile(journalPath);
        CHECK(store.commit(second).succeeded);
        const auto complete = readFile(journalPath);
        const auto frame = complete.mid(acknowledged.size());
        CHECK(frame.endsWith('\n') && frame.size() > 6);
        // Payloads have landed but the record may stop anywhere before its newline.
        for (const qsizetype cut :
             std::array<qsizetype, 6>{0, 1, frame.size() / 3, frame.size() / 2, frame.size() - 2, frame.size() - 1}) {
            writeFile(journalPath, acknowledged + frame.left(cut));
            const auto recovered = RecoveryStore(store.directory()).read();
            CHECK(recovered.succeeded && recovered.incompleteTail == (cut != 0));
            CHECK(recovered.transactions == QList<QJsonObject>{first.state});
            CHECK(recovered.validBytes == acknowledged.size());
            CHECK(store.commit(second).succeeded);
            CHECK(store.commit(second).succeeded);
            const auto retried = RecoveryStore(store.directory()).read();
            CHECK(retried.succeeded && !retried.incompleteTail);
            CHECK(retried.transactions == QList<QJsonObject>{first.state, second.state});
            CHECK(readFile(journalPath) == complete);
        }
        QString error;
        CHECK(store.payload(RecoveryStore::digest(QByteArray("first\0exact bytes", 17)), &error)
              == QByteArray("first\0exact bytes", 17));
        CHECK(error.isEmpty());
    });
    for (const auto &[point, name] : {std::pair{SyncFaults::Point::PayloadSync, "payload sync"},
                                      std::pair{SyncFaults::Point::DirectorySync, "directory sync"},
                                      std::pair{SyncFaults::Point::JournalSync, "journal sync"}}) {
        suite.run(std::string("recovery write failure retains the prior commit and idempotent retry: ") + name,
                  [point] {
                      QTemporaryDir directory;
                      CHECK(directory.isValid());
                      const auto files = std::make_shared<SyncFaults>();
                      RecoveryStore store(directory.path() + "/recovery", files);
                      const auto first = transaction("first", "already acknowledged");
                      const auto pending = transaction("stable-retry-identity", "new pending bytes");
                      CHECK(store.commit(first).succeeded);
                      {
                          ArmedFault fault(*files, point);
                          const auto failed = store.commit(pending);
                          CHECK(files->failures == 1 && !failed.succeeded && !failed.error.isEmpty());
                      }
                      const auto interrupted = store.read();
                      CHECK(interrupted.succeeded && interrupted.transactions.front() == first.state);
                      if (point == SyncFaults::Point::JournalSync)
                          CHECK(interrupted.transactions == QList<QJsonObject>{first.state, pending.state});
                      else
                          CHECK(interrupted.transactions == QList<QJsonObject>{first.state});
                      CHECK(store.commit(pending).succeeded && store.commit(pending).succeeded);
                      const auto recovered = RecoveryStore(store.directory()).read();
                      CHECK(recovered.succeeded && !recovered.incompleteTail);
                      CHECK(recovered.transactions == QList<QJsonObject>{first.state, pending.state});
                      QString error;
                      CHECK(store.payload(RecoveryStore::digest("new pending bytes"), &error) == "new pending bytes");
                      CHECK(error.isEmpty());
                  });
    }
    suite.run("a record is appended only after the directories of its new entries are synced", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        const auto files = std::make_shared<SyncFaults>();
        const auto inventory = QFileInfo(directory.path()).canonicalFilePath() + "/inventory";
        RecoveryStore store(inventory + "/store", files);
        CHECK(store.commit(transaction("first", "one")).succeeded);
        const auto synced = files->syncedDirectories();
        for (const auto &expected : {inventory, inventory + "/store", inventory + "/store/payloads"}) {
            SCOPE(expected.toStdString());
            CHECK(std::find(synced.begin(), synced.end(), expected.toStdString()) != synced.end());
        }
        // A payload that already exists adds no directory entry, so nothing needs a sync.
        const auto before = files->syncedDirectories().size();
        auto same = transaction("second", "one");
        CHECK(store.commit(same).succeeded);
        CHECK_EQ(files->syncedDirectories().size(), before);
    });
    suite.run("recovery payload envelope keeps the boundary and the acknowledged journal", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        RecoveryStore store(directory.path() + "/recovery");
        const auto acknowledged = transaction("acknowledged", "kept");
        CHECK(store.commit(acknowledged).succeeded);
        const auto below = transaction("below", QByteArray(limit - 1, 'b'));
        const auto at = transaction("at", QByteArray(limit, 'a'));
        const auto above = transaction("above", QByteArray(limit + 1, 'Z'));
        CHECK(store.commit(below).succeeded);
        CHECK(store.commit(at).succeeded);
        const auto journalBefore = readFile(store.directory() + "/journal");
        const auto failed = store.commit(above);
        CHECK(!failed.succeeded);
        CHECK(failed.error.contains(QString::number(limit + 1)));
        CHECK(failed.error.contains("pending input is retained"));
        CHECK(readFile(store.directory() + "/journal") == journalBefore);
        const auto reloaded = RecoveryStore(store.directory()).read();
        CHECK(reloaded.succeeded && !reloaded.incompleteTail);
        CHECK(reloaded.transactions.size() == 3);
        QString error;
        CHECK_EQ(store.payload(RecoveryStore::digest(QByteArray(limit - 1, 'b')), &error).size(), limit - 1);
        CHECK(error.isEmpty());
        CHECK_EQ(store.payload(RecoveryStore::digest(QByteArray(limit, 'a')), &error).size(), limit);
        CHECK(error.isEmpty());
        const auto aboveHash = RecoveryStore::digest(QByteArray(limit + 1, 'Z'));
        CHECK(!QFileInfo::exists(store.directory() + "/payloads/" + QString::fromLatin1(aboveHash)));
        CHECK(store.payload(aboveHash, &error).isEmpty());
        CHECK(error.contains("missing or unsafe"));
    });
    suite.run("a long-lived store writes the bytes of a store that rereads every commit", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        RecoveryStore kept(directory.path() + "/kept");
        const auto rereading = directory.path() + "/rereading";
        for (int index = 1; index <= 12; ++index) {
            const auto next = transaction(QString("edit-%1").arg(index), "draft " + QByteArray::number(index));
            CHECK(kept.commit(next).succeeded);
            CHECK(RecoveryStore(rereading).commit(next).succeeded);
            if (index % 4 == 0) {
                // A retry of the flushed transaction appends nothing on either path.
                CHECK(kept.commit(next).succeeded);
                CHECK(RecoveryStore(rereading).commit(next).succeeded);
            }
        }
        const auto bytes = readFile(kept.directory() + "/journal");
        CHECK(bytes == readFile(rereading + "/journal"));
        const auto reloaded = RecoveryStore(kept.directory()).read();
        CHECK(reloaded.succeeded && reloaded.transactions.size() == 12 && reloaded.validBytes == bytes.size());
    });
    suite.run("another writer's records between two commits are read before the next append", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        RecoveryStore store(directory.path() + "/recovery");
        const auto mine = transaction("mine", "first");
        const auto theirs = transaction("theirs", "appended by another store");
        CHECK(store.commit(mine).succeeded);
        CHECK(RecoveryStore(store.directory()).commit(theirs).succeeded);
        // The retried transaction is no longer the last record; it is still in the journal, so the
        // retry appends nothing.
        CHECK(store.commit(mine).succeeded);
        const auto next = transaction("next", "second");
        CHECK(store.commit(next).succeeded);
        const auto reloaded = RecoveryStore(store.directory()).read();
        CHECK(reloaded.succeeded && !reloaded.incompleteTail);
        CHECK(reloaded.transactions == (QList<QJsonObject>{mine.state, theirs.state, next.state}));
    });
    suite.run("a retry after a failed journal sync appends nothing even when other records followed", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        const auto files = std::make_shared<SyncFaults>();
        RecoveryStore store(directory.path() + "/recovery", files);
        const auto first = transaction("first", "open of a");
        const auto other = transaction("other", "open of b");
        {
            ArmedFault fault(*files, SyncFaults::Point::JournalSync);
            CHECK(!store.commit(first).succeeded);
            CHECK_EQ(files->failures.load(), 1);
        }
        CHECK(store.commit(other).succeeded);
        CHECK(store.commit(first).succeeded);
        const auto reloaded = RecoveryStore(store.directory()).read();
        CHECK(reloaded.succeeded);
        CHECK(reloaded.transactions == (QList<QJsonObject>{first.state, other.state}));
        auto reused = transaction("first", "different work");
        CHECK(store.commit(reused).error.contains("reused for different work"));
    });
    suite.run("a journal that repeats a transaction identity is refused", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        RecoveryStore store(directory.path() + "/recovery");
        CHECK(store.commit(transaction("first", "one")).succeeded);
        CHECK(store.commit(transaction("second", "two")).succeeded);
        rewriteLastRecord(store.directory(), [](QJsonObject &record) {
            auto state = record.value("state").toObject();
            state.insert("transactionId", "first");
            record.insert("state", state);
        });
        const auto reread = RecoveryStore(store.directory()).read();
        CHECK(!reread.succeeded);
        CHECK(reread.error.contains("repeats a transaction"));
    });
    suite.run("a same-size change to an earlier record fails the next commit", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        RecoveryStore store(directory.path() + "/recovery");
        CHECK(store.commit(transaction("first", "one")).succeeded);
        CHECK(store.commit(transaction("second", "two")).succeeded);
        const auto journalPath = store.directory() + "/journal";
        auto changed = readFile(journalPath);
        const auto firstFrame = changed.indexOf('\n') + 1;
        CHECK(firstFrame > 0 && changed.size() > firstFrame + 12);
        // Rewritten at once, with the same size, so no timestamp or size can tell.
        changed[firstFrame + 11] = changed.at(firstFrame + 11) == 'A' ? 'B' : 'A';
        writeFile(journalPath, changed);
        const auto failed = store.commit(transaction("third", "three"));
        CHECK(!failed.succeeded && failed.error.contains("integrity"));
        CHECK(readFile(journalPath) == changed);
    });
    suite.run("a foreign, truncated or unsafe journal is verified before the next append", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        RecoveryStore store(directory.path() + "/recovery");
        RecoveryStore other(directory.path() + "/other");
        const auto first = transaction("first", "one");
        const auto second = transaction("second", "two");
        const auto foreignA = transaction("foreign-a", "elsewhere a");
        const auto foreignB = transaction("foreign-b", "elsewhere b");
        CHECK(store.commit(first).succeeded && store.commit(second).succeeded);
        CHECK(other.commit(foreignA).succeeded && other.commit(foreignB).succeeded);
        const auto journalPath = store.directory() + "/journal";
        const auto acknowledged = readFile(journalPath);

        // Another store's complete journal: the next record chains after its last one.
        for (const auto &hash : {RecoveryStore::digest("elsewhere a"), RecoveryStore::digest("elsewhere b")}) {
            const auto name = "/payloads/" + QString::fromLatin1(hash);
            overwritePrivate(store.directory() + name, readFile(other.directory() + name));
        }
        writeFile(journalPath, readFile(other.directory() + "/journal"));
        const auto after = transaction("after-foreign", "three");
        CHECK(store.commit(after).succeeded);
        auto reloaded = RecoveryStore(store.directory()).read();
        CHECK(reloaded.succeeded
              && reloaded.transactions == (QList<QJsonObject>{foreignA.state, foreignB.state, after.state}));

        // A journal whose payloads are missing is refused.
        writeFile(journalPath, acknowledged);
        CHECK(QFile::remove(store.directory() + "/payloads/" + QString::fromLatin1(RecoveryStore::digest("one"))));
        auto failed = store.commit(transaction("missing", "four"));
        CHECK(!failed.succeeded && failed.error.contains("missing or unsafe"));
        CHECK(readFile(journalPath) == acknowledged);
        overwritePrivate(store.directory() + "/payloads/" + QString::fromLatin1(RecoveryStore::digest("one")), "one");

        // An unknown header is refused.
        auto unknown = acknowledged;
        unknown.replace(0, 33, "Branch Truce recovery journal v9\n");
        writeFile(journalPath, unknown);
        failed = store.commit(transaction("unknown", "five"));
        CHECK(!failed.succeeded && failed.error.contains("Unknown or corrupt"));

        // A journal cut back to its first record continues from that record.
        writeFile(journalPath, acknowledged.left(acknowledged.indexOf('\n', 33) + 1));
        const auto resumed = transaction("resumed", "six");
        CHECK(store.commit(resumed).succeeded);
        reloaded = RecoveryStore(store.directory()).read();
        CHECK(reloaded.succeeded && reloaded.transactions == (QList<QJsonObject>{first.state, resumed.state}));

        // A journal other users can read is refused even when its bytes are unchanged.
        const auto before = readFile(journalPath);
        CHECK(QFile::setPermissions(journalPath,
                                    QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadOther));
        failed = store.commit(transaction("exposed", "seven"));
        CHECK(!failed.succeeded && failed.error.contains("unavailable or unsafe"));
        CHECK(readFile(journalPath) == before);
    });
    suite.run("a commit while another holder has the journal lock reports busy and writes nothing", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        RecoveryStore store(directory.path() + "/recovery");
        CHECK(store.commit(transaction("first", "one")).succeeded);
        const auto journalBefore = readFile(store.directory() + "/journal");
        QLockFile holder(store.directory() + "/journal.lock");
        CHECK(holder.tryLock(0));
        const auto busy = store.commit(transaction("second", "two"));
        CHECK(!busy.succeeded);
        CHECK(busy.error.contains("Recovery storage is busy"));
        CHECK_EQ(readFile(store.directory() + "/journal"), journalBefore);
        holder.unlock();
        CHECK(store.commit(transaction("second", "two")).succeeded);
    });
    suite.run("a lock left by a process that has exited does not block the next commit", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        RecoveryStore store(directory.path() + "/recovery");
        CHECK(store.commit(transaction("first", "one")).succeeded);
        // A lock in Qt's own format, naming this host and a process that is gone.
        const auto lockPath = store.directory() + "/journal.lock";
        QByteArray content;
        {
            QLockFile probe(lockPath);
            CHECK(probe.tryLock(0));
            content = readFile(lockPath);
            probe.unlock();
        }
        QProcess exited;
        exited.start(QStringLiteral("/bin/true"), QStringList{});
        CHECK(exited.waitForStarted(5000));
        const auto deadPid = QByteArray::number(exited.processId());
        CHECK(exited.waitForFinished(5000));
        content = deadPid + content.mid(content.indexOf('\n'));
        overwritePrivate(lockPath, content);
        const auto next = store.commit(transaction("second", "two"));
        CHECK(next.succeeded);
        CHECK_EQ(RecoveryStore(store.directory()).read().transactions.size(), 2);
    });
    suite.run("a retried transaction identity that carries different work is refused", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        RecoveryStore store(directory.path() + "/recovery");
        const auto original = transaction("stable-identity", "the acknowledged draft");
        CHECK(store.commit(original).succeeded);
        const auto journalBefore = readFile(store.directory() + "/journal");
        const auto reused = store.commit(transaction("stable-identity", "a newer draft"));
        CHECK(!reused.succeeded);
        CHECK(reused.error.contains("identity was reused for different work"));
        CHECK_EQ(readFile(store.directory() + "/journal"), journalBefore);
        const auto reloaded = RecoveryStore(store.directory()).read();
        CHECK(reloaded.succeeded && reloaded.transactions == QList<QJsonObject>{original.state});
    });
    suite.run("a payload whose bytes changed after the commit fails its integrity check", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        RecoveryStore store(directory.path() + "/recovery");
        CHECK(store.commit(transaction("first", "exact payload bytes")).succeeded);
        const auto hash = RecoveryStore::digest("exact payload bytes");
        overwritePrivate(store.directory() + "/payloads/" + QString::fromLatin1(hash), "exact payload bytez");
        QString error;
        CHECK(store.payload(hash, &error).isEmpty());
        CHECK(error.contains("payload integrity check failed"));
        const auto reread = RecoveryStore(store.directory()).read();
        CHECK(!reread.succeeded && reread.error.contains("payload integrity check failed"));
    });
    for (const auto &[name, mutate] : invalidRecords) {
        suite.run(std::string("a checksummed record with ") + name + " is refused by reads and commits", [mutate] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            RecoveryStore store(directory.path() + "/recovery");
            CHECK(store.commit(transaction("first", "one")).succeeded);
            CHECK(store.commit(transaction("second", "two")).succeeded);
            rewriteLastRecord(store.directory(), mutate);
            const auto journal = readFile(store.directory() + "/journal");
            const auto reread = RecoveryStore(store.directory()).read();
            CHECK(!reread.succeeded);
            CHECK(reread.error.contains("schema or ordering is invalid"));
            const auto failed = store.commit(transaction("third", "three"));
            CHECK(!failed.succeeded && failed.error.contains("schema or ordering is invalid"));
            CHECK_EQ(readFile(store.directory() + "/journal"), journal);
        });
    }
    suite.run("commits refuse a relative location, another schema, an empty identity and a misnamed payload", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        const QString relative = QStringLiteral("branch-truce-relative-recovery");
        CHECK(!QFileInfo::exists(relative));
        const auto refusedRelative = RecoveryStore(relative).commit(transaction("first", "one"));
        CHECK(!refusedRelative.succeeded && refusedRelative.error.contains("absolute app-owned location"));
        CHECK(!QFileInfo::exists(relative));
        RecoveryStore store(directory.path() + "/recovery");
        auto otherSchema = transaction("schema", "two");
        otherSchema.state.insert("schema", 2);
        auto noIdentity = transaction("", "three");
        for (const auto &invalid : {otherSchema, noIdentity}) {
            const auto refused = store.commit(invalid);
            CHECK(!refused.succeeded && refused.error.contains("Invalid recovery transaction"));
        }
        CHECK(!QFileInfo::exists(store.directory()));
        auto misnamed = transaction("misnamed", "four");
        misnamed.payloads = {{RecoveryStore::digest("not these bytes"), "four"}};
        const auto refused = store.commit(misnamed);
        CHECK(!refused.succeeded && refused.error.contains("Invalid recovery payload identity"));
        CHECK(RecoveryStore(store.directory()).read().transactions.isEmpty());
    });
    return suite.report();
}
