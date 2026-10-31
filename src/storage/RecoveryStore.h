// The recovery store persists app-owned journals and text payloads under an exclusive session lease.
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <optional>

class QFile;
class QIODevice;

namespace branch_truce::storage {

struct RecoveryTransaction {
    QJsonObject state;
    QMap<QByteArray, QByteArray> payloads; // SHA-256 hex -> exact bytes.
};

struct RecoveryRead {
    bool succeeded = false;
    QString error;
    QList<QJsonObject> transactions;
    bool incompleteTail = false;
    qint64 validBytes = 0;
    QByteArray lastDigest;
};

struct RecoveryCommit {
    bool succeeded = false;
    QString error;
};

// App-owned data only. Reading validates records and payloads, but does not
// authorize restoration against a repository or perform any repository writes.
class RecoveryStore {
public:
    explicit RecoveryStore(QString directory);
    [[nodiscard]] const QString &directory() const { return directory_; }
    [[nodiscard]] RecoveryRead read() const;
    // Verifies the journal as read() does unless its bytes are exactly those
    // this store last verified or wrote, then appends one record.
    [[nodiscard]] RecoveryCommit commit(const RecoveryTransaction &transaction);
    [[nodiscard]] QByteArray payload(const QByteArray &digest, QString *error = nullptr) const;
    [[nodiscard]] static QByteArray digest(const QByteArray &bytes);
private:
    // The journal through its last complete record, as this store last
    // verified or wrote it, and what the next append needs from it.
    struct JournalEnd {
        QByteArray bytes;
        qsizetype records = 0;
        QByteArray lastDigest;
        QJsonObject lastState;
    };
    [[nodiscard]] bool openJournal(QFile &file, RecoveryRead &result) const;
    [[nodiscard]] RecoveryRead parse(QIODevice &journal) const;
    [[nodiscard]] const JournalEnd *refreshEnd(QString *error);

    QString directory_;
    std::optional<JournalEnd> end_;
};

}
