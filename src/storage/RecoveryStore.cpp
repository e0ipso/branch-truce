// The recovery store persists app-owned journals and text payloads; a lease is the only way to append.
#include "RecoveryStore.h"
#include "core/ResourceLimits.h"
#include "storage/Problem.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <utility>

namespace branch_truce::storage {
namespace {
constexpr int journalVersion = 2;
const QByteArray headerPrefix("Branch Truce recovery journal v");
const QByteArray header = headerPrefix + QByteArray::number(journalVersion) + '\n';
constexpr auto filePermissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
constexpr auto directoryPermissions = filePermissions | QFileDevice::ExeOwner;

QString payloadLimitError(qint64 size)
{
    return Problem{
        ProblemCode::RecoveryPayloadTooLarge,
        {QString::number(size), QString::number(static_cast<qulonglong>(branch_truce::kMaxRecoveryPayloadBytes))}}
        .encoded();
}

bool validDigest(const QByteArray &value)
{
    if (value.size() != 64) return false;
    for (const auto c : value)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

// Creates path when missing and adds its parent to unsynced, so a power loss
// cannot drop the new entry once a record that depends on it is durable.
bool privateDirectory(const FileSystem &files, const QString &path, std::set<QString> &unsynced)
{
    const QFileInfo before(path);
    if (before.isSymLink() || (before.exists() && !before.isDir())) return false;
    if (!before.exists()) {
        if (!QDir().mkpath(path, directoryPermissions)) return false;
        unsynced.insert(QFileInfo(path).absolutePath());
    }
    const QFileInfo existing(path);
    return existing.ownerId() == files.effectiveUser() && QFile::setPermissions(path, directoryPermissions);
}

bool regularPrivateFile(const FileSystem &files, const QString &path)
{
    const QFileInfo info(path);
    return !info.isSymLink() && info.isFile() && info.ownerId() == files.effectiveUser()
        && (info.permissions()
            & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther
               | QFileDevice::WriteOther | QFileDevice::ExeOther))
        == 0;
}

// Writes a new file through a synced temporary and a rename. The caller syncs
// the directory before anything refers to the file.
bool atomicFile(const FileSystem &files, const QString &path, const QByteArray &bytes)
{
    if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) return false;
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    return file.open(QIODevice::WriteOnly) && file.setPermissions(filePermissions) && file.write(bytes) == bytes.size()
        && file.flush() && files.sync(file.handle()) == 0 && file.commit();
}

bool synchronize(const FileSystem &files, QFile &file) { return file.flush() && files.sync(file.handle()) == 0; }

// The digest of a state as the journal stores it, so a state handed to commit
// and the same state read back compare equal.
QByteArray stateDigest(const QJsonObject &state)
{
    const auto stored = QJsonDocument(state).toJson(QJsonDocument::Compact);
    return RecoveryStore::digest(QJsonDocument::fromJson(stored).toJson(QJsonDocument::Compact));
}

// The format version a header line names, or 0 when it names none.
int headerVersion(const QByteArray &line)
{
    if (!line.startsWith(headerPrefix) || !line.endsWith('\n')) return 0;
    bool ok = false;
    const auto version = line.mid(headerPrefix.size()).chopped(1).toInt(&ok);
    return ok ? version : 0;
}
}

RecoveryStore::RecoveryStore(QString directory, std::shared_ptr<const FileSystem> files)
    : directory_(std::move(directory)), files_(std::move(files))
{}

QByteArray RecoveryStore::digest(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

QByteArray RecoveryStore::payload(const QByteArray &hash, QString *error) const
{
    const auto path = directory_ + QStringLiteral("/payloads/") + QString::fromLatin1(hash);
    QFile file(path);
    if (!validDigest(hash) || !regularPrivateFile(*files_, path) || !file.open(QIODevice::ReadOnly)) {
        if (error) *error = Problem{ProblemCode::RecoveryPayloadUnsafe}.encoded();
        return {};
    }
    if (file.size() > static_cast<qint64>(branch_truce::kMaxRecoveryPayloadBytes)) {
        if (error) *error = payloadLimitError(file.size());
        return {};
    }
    const auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError || digest(bytes) != hash) {
        if (error) *error = Problem{ProblemCode::RecoveryPayloadIntegrity}.encoded();
        return {};
    }
    if (error) error->clear();
    return bytes;
}

bool RecoveryStore::openJournal(QFile &file, RecoveryRead &result) const
{
    const auto path = directory_ + QStringLiteral("/journal");
    if (!QFileInfo::exists(path) && !QFileInfo(path).isSymLink()) {
        result.succeeded = true;
        return false;
    }
    const QFileInfo root(directory_);
    const QFileInfo payloads(directory_ + QStringLiteral("/payloads"));
    file.setFileName(path);
    if (root.isSymLink() || !root.isDir() || payloads.isSymLink() || !payloads.isDir()
        || !regularPrivateFile(*files_, path) || !file.open(QIODevice::ReadOnly)) {
        result.error = Problem{ProblemCode::RecoveryJournalUnsafe}.encoded();
        return false;
    }
    return true;
}

RecoveryRead RecoveryStore::read() const
{
    RecoveryRead result;
    QFile file;
    if (!openJournal(file, result)) return result;
    result = parse(file);
    if (result.succeeded && file.error() != QFileDevice::NoError) {
        result.succeeded = false;
        result.error = Problem{ProblemCode::RecoveryJournalIncomplete}.encoded();
    }
    return result;
}

RecoveryRead RecoveryStore::parse(QIODevice &file, std::set<QByteArray> *payloads) const
{
    RecoveryRead result;
    const auto newer = [&result] {
        result.newerFormat = true;
        result.error = Problem{ProblemCode::RecoveryJournalNewer}.encoded();
        return result;
    };
    const auto version = headerVersion(file.readLine());
    if (version > journalVersion) return newer();
    if (version != journalVersion) {
        result.error = Problem{ProblemCode::RecoveryJournalVersion}.encoded();
        return result;
    }
    result.validBytes = file.pos();
    quint64 sequence = 1;
    std::set<QString> identities;
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
            result.error = Problem{ProblemCode::RecoveryRecordIntegrity}.encoded();
            return result;
        }
        const auto recordDocument = QJsonDocument::fromJson(body, &parse);
        const auto record = recordDocument.object();
        if (record.value(QStringLiteral("version")).toInt() > journalVersion) return newer();
        const auto id = record.value(QStringLiteral("transaction")).toString();
        if (parse.error != QJsonParseError::NoError || !recordDocument.isObject()
            || record.value(QStringLiteral("version")).toInt() != journalVersion
            || record.value(QStringLiteral("sequence")).toString() != QString::number(sequence)
            || record.value(QStringLiteral("previous")).toString().toLatin1() != result.lastDigest
            || !record.value(QStringLiteral("payloads")).isArray() || !record.value(QStringLiteral("state")).isObject()
            || id.isEmpty()) {
            result.error = Problem{ProblemCode::RecoveryRecordSchema}.encoded();
            return result;
        }
        if (!identities.insert(id).second) {
            result.error = Problem{ProblemCode::RecoveryTransactionRepeated}.encoded();
            return result;
        }
        for (const auto &reference : record.value(QStringLiteral("payloads")).toArray()) {
            const auto name = reference.toString().toLatin1();
            if (payloads && payloads->contains(name)) continue;
            QString error;
            (void)payload(name, &error);
            if (!error.isEmpty()) {
                result.error = error;
                return result;
            }
            if (payloads) payloads->insert(name);
        }
        result.transactions.push_back({id, record.value(QStringLiteral("state")).toObject()});
        result.validBytes = file.pos();
        result.lastDigest = hash;
        ++sequence;
    }
    result.succeeded = true;
    return result;
}

bool RecoveryStore::FileStamp::operator==(const FileStamp &other) const
{
    return device == other.device && inode == other.inode && size == other.size
        && modified.tv_sec == other.modified.tv_sec && modified.tv_nsec == other.modified.tv_nsec
        && changed.tv_sec == other.changed.tv_sec && changed.tv_nsec == other.changed.tv_nsec;
}

std::optional<RecoveryStore::FileStamp> RecoveryStore::stampOf(const QFile &file) const
{
    struct stat status{};
    if (files_->status(file.handle(), &status) != 0) return std::nullopt;
    return FileStamp{status.st_dev, status.st_ino, status.st_size, status.st_mtim, status.st_ctim};
}

// Reads and verifies the whole journal unless fstat still describes it as
// this store last verified or wrote it. Payloads of verified records are not
// read again. A missing journal is never remembered.
const RecoveryStore::JournalEnd *RecoveryStore::refreshEnd(QString *error)
{
    static const JournalEnd missing;
    RecoveryRead opened;
    QFile file;
    if (!openJournal(file, opened)) {
        end_.reset();
        *error = opened.error;
        return opened.succeeded ? &missing : nullptr;
    }
    const auto stamp = stampOf(file);
    if (end_ && stamp && end_->stamp == stamp) return &*end_;
    end_.reset();
    // Another writer, a changed byte or a cut tail: verify every payload again.
    verified_.clear();
    const auto current = parse(file, &verified_);
    if (!current.succeeded || file.error() != QFileDevice::NoError) {
        verified_.clear();
        *error = current.succeeded ? Problem{ProblemCode::RecoveryJournalIncomplete}.encoded() : current.error;
        return nullptr;
    }
    end_ = JournalEnd{stamp, current.validBytes, current.transactions.size(), current.lastDigest, {}};
    for (const auto &stored : current.transactions) end_->committed.emplace(stored.id, stateDigest(stored.state));
    return &*end_;
}

RecoveryCommit RecoveryStore::commit(const RecoveryTransaction &transaction)
{
    const std::lock_guard lock(commits_);
    return append(transaction);
}

RecoveryCommit RecoveryStore::append(const RecoveryTransaction &transaction)
{
    if (!QDir::isAbsolutePath(directory_)) return {false, Problem{ProblemCode::RecoveryStorageLocation}.encoded()};
    if (transaction.id.isEmpty() || transaction.state.isEmpty())
        return {false, Problem{ProblemCode::RecoveryTransactionInvalid}.encoded()};
    const auto payloadDirectory = directory_ + QStringLiteral("/payloads");
    if (!privateDirectory(*files_, directory_, unsynced_) || !privateDirectory(*files_, payloadDirectory, unsynced_))
        return {false, Problem{ProblemCode::RecoveryStorageCreate}.encoded()};
    const auto journalPath = directory_ + QStringLiteral("/journal");
    if (!QFileInfo::exists(journalPath)) {
        if (!atomicFile(*files_, journalPath, header))
            return {false, Problem{ProblemCode::RecoveryJournalInitialize}.encoded()};
        unsynced_.insert(directory_);
    }
    QString readError;
    const auto *current = refreshEnd(&readError);
    if (!current) return {false, readError};
    QFile journal(journalPath);
    const auto validBytes = current->size;
    // Cutting an interrupted tail changes the file, so its stamp is taken again.
    if (!journal.open(QIODevice::ReadWrite) || (journal.size() != validBytes && !journal.resize(validBytes))
        || !journal.seek(validBytes))
        return {false, Problem{ProblemCode::RecoveryCommitBoundary}.encoded()};
    const auto restamp = [&] {
        if (end_) end_->stamp = stampOf(journal);
    };
    // A sync failure may leave a complete record, and other records may follow
    // it before the retry. Retrying its stable ID makes that record durable
    // without appending the action twice.
    if (const auto committed = current->committed.find(transaction.id); committed != current->committed.end()) {
        if (committed->second != stateDigest(transaction.state))
            return {false, Problem{ProblemCode::RecoveryTransactionReused}.encoded()};
        if (!synchronize(*files_, journal)) return {false, Problem{ProblemCode::RecoveryPendingFlush}.encoded()};
        restamp();
        return {true, {}};
    }
    QJsonArray references;
    for (auto entry = transaction.payloads.cbegin(); entry != transaction.payloads.cend(); ++entry) {
        references.push_back(QString::fromLatin1(entry.key()));
        if (verified_.contains(entry.key())) continue;
        if (!validDigest(entry.key()) || digest(entry.value()) != entry.key())
            return {false, Problem{ProblemCode::RecoveryPayloadIdentity}.encoded()};
        if (entry.value().size() > static_cast<qsizetype>(branch_truce::kMaxRecoveryPayloadBytes))
            return {false, payloadLimitError(entry.value().size())};
        const auto path = payloadDirectory + QLatin1Char('/') + QString::fromLatin1(entry.key());
        if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) {
            QString error;
            (void)payload(entry.key(), &error);
            if (!error.isEmpty()) return {false, error};
        } else {
            if (!atomicFile(*files_, path, entry.value()))
                return {false, Problem{ProblemCode::RecoveryPayloadCommit}.encoded()};
            unsynced_.insert(payloadDirectory);
        }
        verified_.insert(entry.key());
    }
    // The record must not outlive the directory entries it depends on: the
    // store, the journal and its payloads.
    for (auto pending = unsynced_.begin(); pending != unsynced_.end(); pending = unsynced_.erase(pending)) {
        if (!syncDirectory(*files_, *pending)) return {false, Problem{ProblemCode::RecoveryStorageSync}.encoded()};
    }
    const QJsonObject record{{QStringLiteral("version"), journalVersion},
                             {QStringLiteral("sequence"), QString::number(current->records + 1)},
                             {QStringLiteral("previous"), QString::fromLatin1(current->lastDigest)},
                             {QStringLiteral("transaction"), transaction.id},
                             {QStringLiteral("payloads"), references},
                             {QStringLiteral("state"), transaction.state}};
    const auto body = QJsonDocument(record).toJson(QJsonDocument::Compact);
    const auto bodyDigest = digest(body);
    const auto frame = QJsonDocument(QJsonObject{{QStringLiteral("record"), QString::fromLatin1(body.toBase64())},
                                                 {QStringLiteral("sha256"), QString::fromLatin1(bodyDigest)}})
                           .toJson(QJsonDocument::Compact)
        + '\n';
    if (journal.write(frame) != frame.size() || !synchronize(*files_, journal))
        return {false, Problem{ProblemCode::RecoveryCommitFailed}.encoded()};
    if (end_) {
        end_->size += frame.size();
        ++end_->records;
        end_->lastDigest = bodyDigest;
        end_->committed.emplace(transaction.id, stateDigest(transaction.state));
        restamp();
    }
    return {true, {}};
}

RecoveryLease::RecoveryLease(QString directory, std::shared_ptr<const FileSystem> files)
    : lock_(std::make_unique<QLockFile>(directory + QStringLiteral("/session.lock"))),
      store_(std::move(directory), std::move(files))
{}

RecoveryLease::~RecoveryLease() = default;

std::shared_ptr<RecoveryLease> RecoveryLease::acquire(const QString &directory, std::shared_ptr<const FileSystem> files,
                                                      Failure *failure)
{
    const auto fail = [failure](Failure reason) {
        if (failure) *failure = reason;
        return std::shared_ptr<RecoveryLease>{};
    };
    if (!files || !QDir::isAbsolutePath(directory)) return fail(Failure::Unsafe);
    std::shared_ptr<RecoveryLease> lease(new RecoveryLease(directory, files));
    if (!privateDirectory(*files, directory, lease->store_.unsynced_)) return fail(Failure::Unsafe);
    if (!lease->lock_->tryLock(0)) return fail(Failure::Held);
    if (failure) *failure = Failure::None;
    return lease;
}

bool RecoveryLease::remove()
{
    if (removed_) return false;
    removed_ = true;
    QDir store(directory());
    bool removed = true;
    for (const auto &entry :
         store.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
        if (entry.fileName() == QLatin1String("session.lock")) continue;
        removed = (entry.isDir() && !entry.isSymLink() ? QDir(entry.absoluteFilePath()).removeRecursively()
                                                       : QFile::remove(entry.absoluteFilePath()))
            && removed;
    }
    lock_->unlock();
    return QDir().rmdir(directory()) && removed;
}

}
