// The recovery store persists app-owned journals and text payloads; a lease is the only way to append.
#pragma once

#include "storage/FileSystem.h"
#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sys/types.h>

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
// state inside them nor a repository; the recovery layer owns both. Reads may
// run on any thread; commits may too, and run one at a time.
class RecoveryStore {
public:
    explicit RecoveryStore(QString directory, std::shared_ptr<const FileSystem> files = systemFileSystem());
    [[nodiscard]] const QString &directory() const { return directory_; }
    [[nodiscard]] RecoveryRead read() const;
    [[nodiscard]] QByteArray payload(const QByteArray &digest, QString *error = nullptr) const;
    [[nodiscard]] static QByteArray digest(const QByteArray &bytes);

private:
    friend class RecoveryLease;
    // What fstat says about the journal file. A write, a truncation or a
    // replaced file changes it; a rewrite of the same size within the file
    // system's timestamp granularity may not.
    struct FileStamp {
        dev_t device = 0;
        ino_t inode = 0;
        off_t size = 0;
        timespec modified{};
        timespec changed{};
        [[nodiscard]] bool operator==(const FileStamp &other) const;
    };
    // The journal through its last complete record, as this store last
    // verified or wrote it, and what the next append needs from it.
    struct JournalEnd {
        // The file as fstat saw it then, if it could, and the bytes of its
        // complete records. Without a stamp the next commit verifies it again.
        std::optional<FileStamp> stamp;
        qint64 size = 0;
        qsizetype records = 0;
        QByteArray lastDigest;
        // Every committed transaction ID and the digest of its state.
        std::map<QString, QByteArray> committed;
    };
    // Verifies the journal as read() does unless fstat still describes it as
    // this store last verified or wrote it, then appends one record. A
    // transaction ID already in the journal appends nothing: the same state is
    // synced again and succeeds, different state is refused.
    [[nodiscard]] RecoveryCommit commit(const RecoveryTransaction &transaction);
    [[nodiscard]] RecoveryCommit append(const RecoveryTransaction &transaction);
    [[nodiscard]] bool openJournal(QFile &file, RecoveryRead &result) const;
    [[nodiscard]] RecoveryRead parse(QIODevice &journal, std::set<QByteArray> *payloads = nullptr) const;
    [[nodiscard]] const JournalEnd *refreshEnd(QString *error);
    [[nodiscard]] std::optional<FileStamp> stampOf(const QFile &file) const;

    QString directory_;
    std::shared_ptr<const FileSystem> files_;
    // Held for each commit, so commits from several threads run one at a time.
    std::mutex commits_;
    std::optional<JournalEnd> end_;
    // Payloads this store wrote or verified; they are not read again.
    std::set<QByteArray> verified_;
    // Directories with an entry this store created that no sync covers yet.
    std::set<QString> unsynced_;
};

// This process's exclusive use of one recovery store, through session.lock in
// its directory. Appending to a store requires it, so two windows can never
// write one journal. Its commits may run on any thread, one at a time; the
// other members are used on one thread at a time.
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
