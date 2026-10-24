#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>

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
    [[nodiscard]] RecoveryCommit commit(const RecoveryTransaction &transaction);
    [[nodiscard]] QByteArray payload(const QByteArray &digest, QString *error = nullptr) const;
    [[nodiscard]] static QByteArray digest(const QByteArray &bytes);
private:
    QString directory_;
};

}
