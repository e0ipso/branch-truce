#include "storage/RecoveryStore.h"
#include "TestHarness.h"
#include "fixtures/RecoveryFaults.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryDir>
#include <array>

using namespace branch_truce::storage;

namespace {
RecoveryTransaction transaction(const QString &id, const QByteArray &bytes)
{
    const auto hash = RecoveryStore::digest(bytes);
    return {{{"schema", 1}, {"transactionId", id}, {"text", QString::fromLatin1(hash)}}, {{hash, bytes}}};
}
QByteArray readFile(const QString &path)
{
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly));
    const auto bytes = file.readAll();
    CHECK(file.error() == QFileDevice::NoError);
    return bytes;
}
void overwrite(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    CHECK(file.write(bytes) == bytes.size());
}
void overwritePrivate(const QString &path, const QByteArray &bytes)
{
    overwrite(path, bytes);
    CHECK(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
}
}

class RecoveryStoreTests {
public:
    int run()
    {
        test_support::Suite suite("RecoveryStoreTests");
        suite.run("TRUCE-0013 interrupted transaction tails preserve the acknowledged prefix and retry once", [] {
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
            for (const qsizetype cut : std::array<qsizetype, 6>{0, 1, frame.size() / 3,
                    frame.size() / 2, frame.size() - 2, frame.size() - 1}) {
                overwrite(journalPath, acknowledged + frame.left(cut));
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
        for (const auto point : {recovery_faults::Point::PayloadSync, recovery_faults::Point::JournalSync}) {
            suite.run("TRUCE-0013 recovery write failure retains the prior commit and idempotent retry "
                      + std::to_string(static_cast<int>(point)), [=] {
                QTemporaryDir directory;
                CHECK(directory.isValid());
                RecoveryStore store(directory.path() + "/recovery");
                const auto first = transaction("first", "already acknowledged");
                const auto pending = transaction("stable-retry-identity", "new pending bytes");
                CHECK(store.commit(first).succeeded);
                recovery_faults::Failure fault(point);
                const auto failed = store.commit(pending);
                CHECK(recovery_faults::failures == 1 && !failed.succeeded && !failed.error.isEmpty());
                const auto interrupted = store.read();
                CHECK(interrupted.succeeded && interrupted.transactions.front() == first.state);
                if (point == recovery_faults::Point::PayloadSync)
                    CHECK(interrupted.transactions == QList<QJsonObject>{first.state});
                else
                    CHECK(interrupted.transactions == QList<QJsonObject>{first.state, pending.state});
                CHECK(store.commit(pending).succeeded && store.commit(pending).succeeded);
                const auto recovered = RecoveryStore(store.directory()).read();
                CHECK(recovered.succeeded && !recovered.incompleteTail);
                CHECK(recovered.transactions == QList<QJsonObject>{first.state, pending.state});
                QString error;
                CHECK(store.payload(RecoveryStore::digest("new pending bytes"), &error) == "new pending bytes");
                CHECK(error.isEmpty());
            });
        }
        suite.run("TRUCE-0029 recovery payload envelope keeps the boundary and the acknowledged journal", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            RecoveryStore store(directory.path() + "/recovery");
            const auto acknowledged = transaction("acknowledged", "kept");
            CHECK(store.commit(acknowledged).succeeded);
            const auto below = transaction("below", QByteArray(8388607, 'b'));
            const auto at = transaction("at", QByteArray(8388608, 'a'));
            const auto above = transaction("above", QByteArray(8388609, 'Z'));
            CHECK(store.commit(below).succeeded);
            CHECK(store.commit(at).succeeded);
            const auto journalBefore = readFile(store.directory() + "/journal");
            const auto failed = store.commit(above);
            CHECK(!failed.succeeded);
            CHECK(failed.error.contains("8388609"));
            CHECK(failed.error.contains("pending input is retained"));
            CHECK(readFile(store.directory() + "/journal") == journalBefore);
            const auto reloaded = RecoveryStore(store.directory()).read();
            CHECK(reloaded.succeeded && !reloaded.incompleteTail);
            CHECK(reloaded.transactions.size() == 3);
            QString error;
            CHECK(store.payload(RecoveryStore::digest(QByteArray(8388607, 'b')), &error).size() == 8388607);
            CHECK(error.isEmpty());
            CHECK(store.payload(RecoveryStore::digest(QByteArray(8388608, 'a')), &error).size() == 8388608);
            CHECK(error.isEmpty());
            const auto aboveHash = RecoveryStore::digest(QByteArray(8388609, 'Z'));
            CHECK(!QFileInfo::exists(store.directory() + "/payloads/" + QString::fromLatin1(aboveHash)));
            CHECK(store.payload(aboveHash, &error).isEmpty());
            CHECK(error.contains("missing or unsafe"));
        });
        suite.run("TRUCE-0249 a long-lived store writes the bytes of a store that rereads every commit", [] {
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
        suite.run("TRUCE-0249 another writer's records between two commits are read before the next append", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            RecoveryStore store(directory.path() + "/recovery");
            const auto mine = transaction("mine", "first");
            const auto theirs = transaction("theirs", "appended by another store");
            CHECK(store.commit(mine).succeeded);
            CHECK(RecoveryStore(store.directory()).commit(theirs).succeeded);
            // The last record is no longer this store's, so its retry appends again as before.
            CHECK(store.commit(mine).succeeded);
            const auto next = transaction("next", "second");
            CHECK(store.commit(next).succeeded);
            const auto reloaded = RecoveryStore(store.directory()).read();
            CHECK(reloaded.succeeded && !reloaded.incompleteTail);
            CHECK(reloaded.transactions == (QList<QJsonObject>{mine.state, theirs.state, mine.state, next.state}));
        });
        suite.run("TRUCE-0249 a same-size change to an earlier record fails the next commit", [] {
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
            overwrite(journalPath, changed);
            const auto failed = store.commit(transaction("third", "three"));
            CHECK(!failed.succeeded && failed.error.contains("integrity"));
            CHECK(readFile(journalPath) == changed);
        });
        suite.run("TRUCE-0249 a foreign, truncated or unsafe journal is verified before the next append", [] {
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
            overwrite(journalPath, readFile(other.directory() + "/journal"));
            const auto after = transaction("after-foreign", "three");
            CHECK(store.commit(after).succeeded);
            auto reloaded = RecoveryStore(store.directory()).read();
            CHECK(reloaded.succeeded
                && reloaded.transactions == (QList<QJsonObject>{foreignA.state, foreignB.state, after.state}));

            // A journal whose payloads are missing is refused.
            overwrite(journalPath, acknowledged);
            CHECK(QFile::remove(store.directory() + "/payloads/" + QString::fromLatin1(RecoveryStore::digest("one"))));
            auto failed = store.commit(transaction("missing", "four"));
            CHECK(!failed.succeeded && failed.error.contains("missing or unsafe"));
            CHECK(readFile(journalPath) == acknowledged);
            overwritePrivate(store.directory() + "/payloads/" + QString::fromLatin1(RecoveryStore::digest("one")), "one");

            // An unknown header is refused.
            auto unknown = acknowledged;
            unknown.replace(0, 33, "Branch Truce recovery journal v9\n");
            overwrite(journalPath, unknown);
            failed = store.commit(transaction("unknown", "five"));
            CHECK(!failed.succeeded && failed.error.contains("Unknown or corrupt"));

            // A journal cut back to its first record continues from that record.
            overwrite(journalPath, acknowledged.left(acknowledged.indexOf('\n', 33) + 1));
            const auto resumed = transaction("resumed", "six");
            CHECK(store.commit(resumed).succeeded);
            reloaded = RecoveryStore(store.directory()).read();
            CHECK(reloaded.succeeded && reloaded.transactions == (QList<QJsonObject>{first.state, resumed.state}));

            // A journal other users can read is refused even when its bytes are unchanged.
            const auto before = readFile(journalPath);
            CHECK(QFile::setPermissions(journalPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadOther));
            failed = store.commit(transaction("exposed", "seven"));
            CHECK(!failed.succeeded && failed.error.contains("unavailable or unsafe"));
            CHECK(readFile(journalPath) == before);
        });
        return suite.report();
    }
};

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    return RecoveryStoreTests{}.run();
}
