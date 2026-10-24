#include "RecoveryStore.h"
#include "core/ResourceLimits.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <utility>
#include <unistd.h>

namespace branch_truce::storage {
namespace {
const QByteArray header("Branch Truce recovery journal v1\n");
constexpr auto filePermissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
constexpr auto directoryPermissions = filePermissions | QFileDevice::ExeOwner;

bool validDigest(const QByteArray &value)
{
    if (value.size() != 64) return false;
    for (const auto c : value) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

bool privateDirectory(const QString &path)
{
    const QFileInfo before(path);
    if (before.isSymLink() || (before.exists() && !before.isDir())) return false;
    if (!QDir().mkpath(path, directoryPermissions)) return false;
    const QFileInfo created(path);
    return created.ownerId() == ::geteuid() && QFile::setPermissions(path, directoryPermissions);
}

bool regularPrivateFile(const QString &path)
{
    const QFileInfo info(path);
    return !info.isSymLink() && info.isFile() && info.ownerId() == ::geteuid()
        && (info.permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
            | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther)) == 0;
}

bool atomicFile(const QString &path, const QByteArray &bytes)
{
    if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) return false;
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    return file.open(QIODevice::WriteOnly) && file.setPermissions(filePermissions)
        && file.write(bytes) == bytes.size() && file.flush() && ::fsync(file.handle()) == 0 && file.commit();
}

bool synchronize(QFile &file)
{
    return file.flush() && ::fsync(file.handle()) == 0;
}
}

RecoveryStore::RecoveryStore(QString directory) : directory_(std::move(directory)) {}

QByteArray RecoveryStore::digest(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

QByteArray RecoveryStore::payload(const QByteArray &hash, QString *error) const
{
    const auto path = directory_ + QStringLiteral("/payloads/") + QString::fromLatin1(hash);
    QFile file(path);
    if (!validDigest(hash) || !regularPrivateFile(path) || !file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Recovery payload is missing or unsafe.");
        return {};
    }
    if (file.size() > static_cast<qint64>(branch_truce::kMaxRecoveryPayloadBytes)) {
        if (error) *error = QStringLiteral("Recovery payload of %1 bytes exceeds the 8388608 byte limit; pending input is retained.").arg(file.size());
        return {};
    }
    const auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError || digest(bytes) != hash) {
        if (error) *error = QStringLiteral("Recovery payload integrity check failed.");
        return {};
    }
    if (error) error->clear();
    return bytes;
}

RecoveryRead RecoveryStore::read() const
{
    RecoveryRead result;
    const auto path = directory_ + QStringLiteral("/journal");
    if (!QFileInfo::exists(path) && !QFileInfo(path).isSymLink()) {
        result.succeeded = true;
        return result;
    }
    const QFileInfo root(directory_);
    const QFileInfo payloads(directory_ + QStringLiteral("/payloads"));
    QFile file(path);
    if (root.isSymLink() || !root.isDir() || payloads.isSymLink() || !payloads.isDir()
        || !regularPrivateFile(path) || !file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("Recovery journal is unavailable or unsafe.");
        return result;
    }
    if (file.readLine() != header) {
        result.error = QStringLiteral("Unknown or corrupt recovery journal version.");
        return result;
    }
    result.validBytes = file.pos();
    quint64 sequence = 1;
    while (!file.atEnd()) {
        const auto line = file.readLine();
        if (!line.endsWith('\n')) {
            result.incompleteTail = true;
            break;
        }
        QJsonParseError parse;
        const auto frame = QJsonDocument::fromJson(line, &parse);
        const auto object = frame.object();
        const auto encoded = object.value(QStringLiteral("record")).toString().toLatin1();
        const auto body = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
        const auto hash = object.value(QStringLiteral("sha256")).toString().toLatin1();
        if (parse.error != QJsonParseError::NoError || !frame.isObject() || body.toBase64() != encoded
            || !validDigest(hash) || hash != digest(body)) {
            result.error = QStringLiteral("Recovery record integrity check failed.");
            return result;
        }
        const auto recordDocument = QJsonDocument::fromJson(body, &parse);
        const auto record = recordDocument.object();
        const auto state = record.value(QStringLiteral("state")).toObject();
        if (parse.error != QJsonParseError::NoError || !recordDocument.isObject()
            || record.value(QStringLiteral("version")).toInt() != 1
            || record.value(QStringLiteral("sequence")).toString() != QString::number(sequence)
            || record.value(QStringLiteral("previous")).toString().toLatin1() != result.lastDigest
            || !record.value(QStringLiteral("payloads")).isArray()
            || state.value(QStringLiteral("schema")).toInt() != 1
            || state.value(QStringLiteral("transactionId")).toString().isEmpty()) {
            result.error = QStringLiteral("Recovery record schema or ordering is invalid.");
            return result;
        }
        for (const auto &reference : record.value(QStringLiteral("payloads")).toArray()) {
            QString error;
            (void)payload(reference.toString().toLatin1(), &error);
            if (!error.isEmpty()) {
                result.error = error;
                return result;
            }
        }
        result.transactions.push_back(state);
        result.validBytes = file.pos();
        result.lastDigest = hash;
        ++sequence;
    }
    if (file.error() != QFileDevice::NoError) {
        result.error = QStringLiteral("Could not read the complete recovery journal.");
        return result;
    }
    result.succeeded = true;
    return result;
}

RecoveryCommit RecoveryStore::commit(const RecoveryTransaction &transaction)
{
    if (!QDir::isAbsolutePath(directory_))
        return {false, QStringLiteral("Recovery storage needs an absolute app-owned location.")};
    if (transaction.state.value(QStringLiteral("schema")).toInt() != 1
        || transaction.state.value(QStringLiteral("transactionId")).toString().isEmpty())
        return {false, QStringLiteral("Invalid recovery transaction.")};
    if (!privateDirectory(directory_) || !privateDirectory(directory_ + QStringLiteral("/payloads")))
        return {false, QStringLiteral("Could not create private recovery storage; retry after repairing its location.")};
    QLockFile lock(directory_ + QStringLiteral("/journal.lock"));
    if (!lock.tryLock(0)) return {false, QStringLiteral("Recovery storage is busy; retry the pending action.")};
    const auto journalPath = directory_ + QStringLiteral("/journal");
    if (!QFileInfo::exists(journalPath) && !atomicFile(journalPath, header))
        return {false, QStringLiteral("Could not initialize the recovery journal.")};
    const auto current = read();
    if (!current.succeeded) return {false, current.error};
    QJsonArray references;
    for (auto entry = transaction.payloads.cbegin(); entry != transaction.payloads.cend(); ++entry) {
        if (!validDigest(entry.key()) || digest(entry.value()) != entry.key())
            return {false, QStringLiteral("Invalid recovery payload identity.")};
        if (entry.value().size() > static_cast<qsizetype>(branch_truce::kMaxRecoveryPayloadBytes))
            return {false, QStringLiteral("Recovery payload of %1 bytes exceeds the 8388608 byte limit; pending input is retained.").arg(entry.value().size())};
        const auto path = directory_ + QStringLiteral("/payloads/") + QString::fromLatin1(entry.key());
        if (!QFileInfo::exists(path) && !atomicFile(path, entry.value()))
            return {false, QStringLiteral("Could not commit a recovery payload; pending input is retained.")};
        QString error;
        (void)payload(entry.key(), &error);
        if (!error.isEmpty()) return {false, error};
        references.push_back(QString::fromLatin1(entry.key()));
    }
    QFile journal(journalPath);
    if (!journal.open(QIODevice::ReadWrite) || !journal.resize(current.validBytes)
        || !journal.seek(current.validBytes))
        return {false, QStringLiteral("Could not open the recovery commit boundary.")};
    // A flush failure may leave a complete record. Retrying its stable request
    // identity makes that record durable without appending the action twice.
    if (!current.transactions.isEmpty()
        && current.transactions.back().value(QStringLiteral("transactionId"))
            == transaction.state.value(QStringLiteral("transactionId"))) {
        if (current.transactions.back() != transaction.state)
            return {false, QStringLiteral("Recovery transaction identity was reused for different work.")};
        return synchronize(journal) ? RecoveryCommit{true, {}}
            : RecoveryCommit{false, QStringLiteral("Could not flush the pending recovery transaction; retry.")};
    }
    const QJsonObject record{{QStringLiteral("version"), 1},
        {QStringLiteral("sequence"), QString::number(current.transactions.size() + 1)},
        {QStringLiteral("previous"), QString::fromLatin1(current.lastDigest)},
        {QStringLiteral("payloads"), references}, {QStringLiteral("state"), transaction.state}};
    const auto body = QJsonDocument(record).toJson(QJsonDocument::Compact);
    const auto frame = QJsonDocument(QJsonObject{{QStringLiteral("record"), QString::fromLatin1(body.toBase64())},
        {QStringLiteral("sha256"), QString::fromLatin1(digest(body))}}).toJson(QJsonDocument::Compact) + '\n';
    if (journal.write(frame) != frame.size() || !synchronize(journal))
        return {false, QStringLiteral("Recovery commit failed; pending input is retained for retry.")};
    return {true, {}};
}
}
