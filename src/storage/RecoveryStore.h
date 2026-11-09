// The recovery store persists app-owned journals and text payloads; a lease is the only way to append.
#pragma once

#include "storage/FileSystem.h"
#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <map>
#include <memory>
#include <optional>
#include <set>

class QFile;
class QIODevice;
class QLockFile;

namespace branch_truce::storage {

// One record: a stable ID that makes a retry idempotent, a state the
// recovery layer owns, and the payloads the state refers to.
struct RecoveryTransaction {
    QString id;
    QJsonObject state;
    QMap<QByteArray, QByteArray> payloads; // SHA-256 hex -> exact bytes.
};

// A record as read back from the journal.
struct StoredTransaction {
    QString id;
    QJsonObject state;
    bool operator==(const StoredTransaction &) const = default;
};

struct RecoveryRead {
    bool succeeded = false;
    QString error;
    // The journal or one of its records has a format later than this build reads.
    bool newerFormat = false;
    QList<StoredTransaction> transactions;
    bool incompleteTail = false;
    qint64 validBytes = 0;
    QByteArray lastDigest;
};

struct RecoveryCommit {
    bool succeeded = false;
    QString error;
};

// App-owned data only. Reading validates frames and payloads, but neither the
// state inside them nor a repository; the recovery layer owns both.
class RecoveryStore {
public:
    explicit RecoveryStore(QString directory, std::shared_ptr<const FileSystem> files = systemFileSystem());
    [[nodiscard]] const QString &directory() const { return directory_; }
    [[nodiscard]] RecoveryRead read() const;
    [[nodiscard]] QByteArray payload(const QByteArray &digest, QString *error = nullptr) const;
    [[nodiscard]] static QByteArray digest(const QByteArray &bytes);

private:
    friend class RecoveryLease;
    // The journal through its last complete record, as this store last
    // verified or wrote it, and what the next append needs from it.
    struct JournalEnd {
        QByteArray bytes;
        qsizetype records = 0;
        QByteArray lastDigest;
        // Every committed transaction ID and the digest of its state.
        std::map<QString, QByteArray> committed;
    };
    // Verifies the journal as read() does unless its bytes are exactly those
    // this store last verified or wrote, then appends one record. A transaction
    // ID already in the journal appends nothing: the same state is synced again
    // and succeeds, different state is refused.
    [[nodiscard]] RecoveryCommit commit(const RecoveryTransaction &transaction);
    [[nodiscard]] bool openJournal(QFile &file, RecoveryRead &result) const;
    [[nodiscard]] RecoveryRead parse(QIODevice &journal, std::set<QByteArray> *payloads = nullptr) const;
    [[nodiscard]] const JournalEnd *refreshEnd(QString *error);

    QString directory_;
    std::shared_ptr<const FileSystem> files_;
    std::optional<JournalEnd> end_;
    // Payloads this store wrote or verified; they are not read again.
    std::set<QByteArray> verified_;
    // Directories with an entry this store created that no sync covers yet.
    std::set<QString> unsynced_;
};

// This process's exclusive use of one recovery store, through session.lock in
// its directory. Appending to a store requires it, so two windows can never
// write one journal. Used on one thread at a time.
class RecoveryLease {
public:
    enum class Failure { None, Unsafe, Held };
    // Creates the directory owner-only when it is missing. Null when the
    // directory is not an absolute, private directory of this user, or when
    // another window or process holds it.
    [[nodiscard]] static std::shared_ptr<RecoveryLease>
    acquire(const QString &directory, std::shared_ptr<const FileSystem> files = systemFileSystem(),
            Failure *failure = nullptr);
    ~RecoveryLease();
    RecoveryLease(const RecoveryLease &) = delete;
    RecoveryLease &operator=(const RecoveryLease &) = delete;

    [[nodiscard]] const QString &directory() const { return store_.directory(); }
    [[nodiscard]] const RecoveryStore &store() const { return store_; }
    [[nodiscard]] RecoveryCommit commit(const RecoveryTransaction &transaction) { return store_.commit(transaction); }
    // Deletes the store and everything in it, then releases the lease.
    // Afterwards nothing can be committed.
    bool remove();

private:
    RecoveryLease(QString directory, std::shared_ptr<const FileSystem> files);
    std::unique_ptr<QLockFile> lock_;
    RecoveryStore store_;
    bool removed_ = false;
};

}
