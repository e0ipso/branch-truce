// The recovery store persists app-owned journals and text payloads; a lease is the only way to append.
#include "RecoveryStore.h"
#include "core/ResourceLimits.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <algorithm>
#include <utility>

namespace branch_truce::storage {
namespace {
constexpr int journalVersion = 3;
const QByteArray headerPrefix("Branch Truce recovery journal v");
const QByteArray header = headerPrefix + QByteArray::number(journalVersion) + '\n';
constexpr auto filePermissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
constexpr auto directoryPermissions = filePermissions | QFileDevice::ExeOwner;

Problem payloadLimitError(qint64 size)
{
    return {ProblemCode::RecoveryPayloadTooLarge,
            {QString::number(size), QString::number(static_cast<qulonglong>(branch_truce::kMaxRecoveryPayloadBytes))}};
}

Problem resourceLimit() { return ProblemCode::RecoveryResourceLimit; }

// Subtraction keeps both admission and read accounting safe at integer limits.
bool charge(qint64 &used, qint64 amount, qint64 limit)
{
    if (amount < 0 || used < 0 || used > limit || amount > limit - used) return false;
    used += amount;
    return true;
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
QByteArray stateDigest(const QJsonObject &state, qint64 retained)
{
    const auto stored = QJsonDocument(QJsonObject{{QStringLiteral("state"), state},
                                                  {QStringLiteral("retained"), QString::number(retained)}})
                            .toJson(QJsonDocument::Compact);
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

RecoveryStore::RecoveryStore(QString directory, std::shared_ptr<const FileSystem> files, RecoveryLimits limits)
    : directory_(std::move(directory)), files_(std::move(files)), limits_(limits)
{
    const RecoveryLimits supported;
    for (auto member : {&RecoveryLimits::frameBytes, &RecoveryLimits::journalBytes, &RecoveryLimits::stateBytes,
                        &RecoveryLimits::expandedBytes, &RecoveryLimits::transactions, &RecoveryLimits::payloadFiles,
                        &RecoveryLimits::storeBytes})
        limits_.*member = std::clamp(limits_.*member, qint64{0}, supported.*member);
}

QByteArray RecoveryStore::digest(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

QByteArray RecoveryStore::payload(const QByteArray &hash, Problem *error, qint64 maxBytes) const
{
    const auto path = directory_ + QStringLiteral("/payloads/") + QString::fromLatin1(hash);
    QFile file(path);
    if (!validDigest(hash) || !regularPrivateFile(*files_, path) || !file.open(QIODevice::ReadOnly)) {
        if (error) *error = Problem{ProblemCode::RecoveryPayloadUnsafe};
        return {};
    }
    maxBytes = std::clamp(maxBytes, qint64{0}, static_cast<qint64>(branch_truce::kMaxRecoveryPayloadBytes));
    if (file.size() > maxBytes) {
        if (error) *error = resourceLimit();
        return {};
    }
    const auto bytes = file.read(maxBytes + 1);
    if (bytes.size() > maxBytes || !file.atEnd()) {
        if (error) *error = resourceLimit();
        return {};
    }
    if (file.error() != QFileDevice::NoError || digest(bytes) != hash) {
        if (error) *error = Problem{ProblemCode::RecoveryPayloadIntegrity};
        return {};
    }
    if (error) *error = {};
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
        result.problem = Problem{ProblemCode::RecoveryJournalUnsafe};
        return false;
    }
    return true;
}

RecoveryRead RecoveryStore::read(bool firstRecordOnly) const
{
    RecoveryRead result;
    QFile file;
    if (!openJournal(file, result)) return result;
    std::set<QByteArray> payloads;
    result = parse(file, payloads, firstRecordOnly);
    if (result.succeeded && file.error() != QFileDevice::NoError) {
        result.succeeded = false;
        result.problem = Problem{ProblemCode::RecoveryJournalIncomplete};
    }
    return result;
}

Problem RecoveryStore::retention(qint64 journalBytes, const QMap<QByteArray, QByteArray> &additional) const
{
    qint64 used = 0, count = 0;
    if (!charge(used, journalBytes, limits_.storeBytes)) return resourceLimit();
    const auto directory = directory_ + QStringLiteral("/payloads");
    QDirIterator entries(directory, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    while (entries.hasNext()) {
        entries.next();
        const auto info = entries.fileInfo();
        if (!charge(count, 1, limits_.payloadFiles) || !charge(used, info.size(), limits_.storeBytes))
            return resourceLimit();
        if (!validDigest(info.fileName().toLatin1()) || !regularPrivateFile(*files_, info.absoluteFilePath()))
            return ProblemCode::RecoveryPayloadUnsafe;
    }
    for (auto entry = additional.cbegin(); entry != additional.cend(); ++entry) {
        if (QFileInfo::exists(directory + QLatin1Char('/') + QString::fromLatin1(entry.key()))) continue;
        if (!charge(count, 1, limits_.payloadFiles) || !charge(used, entry.value().size(), limits_.storeBytes))
            return resourceLimit();
    }
    return {};
}

RecoveryRead RecoveryStore::parse(QIODevice &file, std::set<QByteArray> &payloads, bool firstRecordOnly,
                                  qint64 *stateBytes) const
{
    RecoveryRead result;
    const auto limited = [&result] {
        result.problem = resourceLimit();
        return result;
    };
    const auto newer = [&result] {
        result.newerFormat = true;
        result.problem = Problem{ProblemCode::RecoveryJournalNewer};
        return result;
    };
    // Qt reads at most maxSize - 1 bytes. Keep one extra byte to detect overflow.
    const auto lineHeader = file.readLine(130);
    if (lineHeader.size() > 128 || (!lineHeader.endsWith('\n') && !file.atEnd())) return limited();
    const auto version = headerVersion(lineHeader);
    if (version > journalVersion) return newer();
    if (version != journalVersion) {
        result.problem = Problem{ProblemCode::RecoveryJournalVersion};
        return result;
    }
    if (!firstRecordOnly) {
        if (file.size() > limits_.journalBytes) return limited();
        if (const auto error = retention(file.size()); !error.isEmpty()) {
            result.problem = error;
            return result;
        }
    }
    result.validBytes = file.pos();
    quint64 sequence = 1;
    qint64 retained = 0;
    std::set<QString> identities;
    while (!file.atEnd()) {
        const auto bound = firstRecordOnly ? std::min(limits_.frameBytes, qint64{65536}) : limits_.frameBytes;
        const auto line = file.readLine(bound + 2);
        if (line.size() > bound || (!line.endsWith('\n') && !file.atEnd())) return limited();
        if (!line.endsWith('\n')) {
            result.incompleteTail = true;
            break;
        }
        if (result.transactions.size() >= limits_.transactions) return limited();
        QJsonParseError parse;
        const auto frame = QJsonDocument::fromJson(line, &parse);
        const auto object = frame.object();
        const auto encoded = object.value(QStringLiteral("record")).toString().toLatin1();
        const auto decodedSize = encoded.size() / 4 * 3 - (encoded.endsWith("==") ? 2 : encoded.endsWith('=') ? 1 : 0);
        if (!charge(retained, decodedSize, limits_.stateBytes)) return limited();
        const auto body = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
        const auto hash = object.value(QStringLiteral("sha256")).toString().toLatin1();
        if (parse.error != QJsonParseError::NoError || !frame.isObject() || body.toBase64() != encoded
            || !validDigest(hash) || hash != digest(body)) {
            result.problem = Problem{ProblemCode::RecoveryRecordIntegrity};
            return result;
        }
        const auto recordDocument = QJsonDocument::fromJson(body, &parse);
        const auto record = recordDocument.object();
        if (record.value(QStringLiteral("version")).toInt() > journalVersion) return newer();
        const auto id = record.value(QStringLiteral("transaction")).toString();
        bool retainedOk = false;
        const auto expanded = record.value(QStringLiteral("retained")).toString().toLongLong(&retainedOk);
        if (id.size() > 256 || !charge(retained, id.size() * 2 + 256, limits_.stateBytes)) return limited();
        if (parse.error != QJsonParseError::NoError || !recordDocument.isObject()
            || record.value(QStringLiteral("version")).toInt() != journalVersion
            || record.value(QStringLiteral("sequence")).toString() != QString::number(sequence)
            || record.value(QStringLiteral("previous")).toString().toLatin1() != result.lastDigest
            || !record.value(QStringLiteral("payloads")).isArray() || !record.value(QStringLiteral("state")).isObject()
            || id.isEmpty() || !retainedOk
            || QString::number(expanded) != record.value(QStringLiteral("retained")).toString()) {
            result.problem = Problem{ProblemCode::RecoveryRecordSchema};
            return result;
        }
        if (!charge(result.retainedBytes, expanded, limits_.expandedBytes)) return limited();
        if (!identities.insert(id).second) {
            result.problem = Problem{ProblemCode::RecoveryTransactionRepeated};
            return result;
        }
        for (const auto &reference :
             firstRecordOnly ? QJsonArray{} : record.value(QStringLiteral("payloads")).toArray()) {
            const auto name = reference.toString().toLatin1();
            if (payloads.contains(name)) continue;
            Problem error;
            (void)payload(name, &error);
            if (!error.isEmpty()) {
                result.problem = error;
                return result;
            }
            payloads.insert(name);
        }
        result.transactions.push_back({id, record.value(QStringLiteral("state")).toObject(), expanded});
        result.validBytes = file.pos();
        result.lastDigest = hash;
        ++sequence;
        if (firstRecordOnly) break;
    }
    if (stateBytes) *stateBytes = retained;
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
const RecoveryStore::JournalEnd *RecoveryStore::refreshEnd(Problem *error)
{
    static const JournalEnd missing;
    RecoveryRead opened;
    QFile file;
    if (!openJournal(file, opened)) {
        end_.reset();
        verified_.clear();
        *error = opened.problem;
        return opened.succeeded ? &missing : nullptr;
    }
    const auto stamp = stampOf(file);
    if (end_ && stamp && end_->stamp == stamp) return &*end_;
    end_.reset();
    // Another writer, a changed byte or a cut tail: verify every payload again.
    verified_.clear();
    qint64 retained = 0;
    const auto current = parse(file, verified_, false, &retained);
    if (!current.succeeded || file.error() != QFileDevice::NoError) {
        verified_.clear();
        *error = current.succeeded ? Problem{ProblemCode::RecoveryJournalIncomplete} : current.problem;
        return nullptr;
    }
    end_ = JournalEnd{stamp,    current.validBytes,    current.transactions.size(),
                      retained, current.retainedBytes, current.lastDigest,
                      {}};
    for (const auto &stored : current.transactions)
        end_->committed.emplace(stored.id, stateDigest(stored.state, stored.retainedBytes));
    return &*end_;
}

RecoveryCommit RecoveryStore::commit(const RecoveryTransaction &transaction)
{
    const std::lock_guard lock(commits_);
    return append(transaction);
}

RecoveryCommit RecoveryStore::append(const RecoveryTransaction &transaction)
{
    if (!QDir::isAbsolutePath(directory_)) return {false, Problem{ProblemCode::RecoveryStorageLocation}};
    if (transaction.id.isEmpty() || transaction.state.isEmpty())
        return {false, Problem{ProblemCode::RecoveryTransactionInvalid}};
    if (transaction.id.size() > 256) return {false, resourceLimit()};
    Problem readError;
    const auto *current = refreshEnd(&readError);
    if (!current) return {false, readError};
    const auto journalPath = directory_ + QStringLiteral("/journal");
    // A complete record left by a failed sync keeps its ID, even at capacity.
    // A retry flushes it without admitting new history or touching payloads.
    if (const auto committed = current->committed.find(transaction.id); committed != current->committed.end()) {
        if (committed->second != stateDigest(transaction.state, transaction.retainedBytes))
            return {false, Problem{ProblemCode::RecoveryTransactionReused}};
        QFile journal(journalPath);
        if (!journal.open(QIODevice::ReadWrite) || !synchronize(*files_, journal))
            return {false, Problem{ProblemCode::RecoveryPendingFlush}};
        if (end_) end_->stamp = stampOf(journal);
        return {true, {}};
    }
    if (current->records >= limits_.transactions) return {false, resourceLimit()};
    auto expanded = current->retainedBytes;
    if (!charge(expanded, transaction.retainedBytes, limits_.expandedBytes)) return {false, resourceLimit()};
    if (transaction.payloads.size() > limits_.payloadFiles) return {false, resourceLimit()};
    QJsonArray references;
    for (auto entry = transaction.payloads.cbegin(); entry != transaction.payloads.cend(); ++entry) {
        if (entry.value().size() > static_cast<qsizetype>(branch_truce::kMaxRecoveryPayloadBytes))
            return {false, payloadLimitError(entry.value().size())};
        if (!validDigest(entry.key()) || digest(entry.value()) != entry.key())
            return {false, Problem{ProblemCode::RecoveryPayloadIdentity}};
        references.push_back(QString::fromLatin1(entry.key()));
    }
    const QJsonObject record{{QStringLiteral("version"), journalVersion},
                             {QStringLiteral("sequence"), QString::number(current->records + 1)},
                             {QStringLiteral("previous"), QString::fromLatin1(current->lastDigest)},
                             {QStringLiteral("transaction"), transaction.id},
                             {QStringLiteral("retained"), QString::number(transaction.retainedBytes)},
                             {QStringLiteral("payloads"), references},
                             {QStringLiteral("state"), transaction.state}};
    const auto body = QJsonDocument(record).toJson(QJsonDocument::Compact);
    auto retained = current->stateBytes;
    if (!charge(retained, body.size(), limits_.stateBytes)
        || !charge(retained, transaction.id.size() * 2 + 256, limits_.stateBytes))
        return {false, resourceLimit()};
    const auto bodyDigest = digest(body);
    const auto frame = QJsonDocument(QJsonObject{{QStringLiteral("record"), QString::fromLatin1(body.toBase64())},
                                                 {QStringLiteral("sha256"), QString::fromLatin1(bodyDigest)}})
                           .toJson(QJsonDocument::Compact)
        + '\n';
    auto size = current->size ? current->size : header.size();
    if (frame.size() > limits_.frameBytes || !charge(size, frame.size(), limits_.journalBytes))
        return {false, resourceLimit()};
    if (const auto error = retention(size, transaction.payloads); !error.isEmpty()) return {false, error};

    // Admission is complete before creating payloads, initializing the journal
    // or cutting an interrupted tail. Rejections leave the store unchanged.
    const auto payloadDirectory = directory_ + QStringLiteral("/payloads");
    if (!privateDirectory(*files_, directory_, unsynced_) || !privateDirectory(*files_, payloadDirectory, unsynced_))
        return {false, Problem{ProblemCode::RecoveryStorageCreate}};
    if (!QFileInfo::exists(journalPath)) {
        if (!atomicFile(*files_, journalPath, header)) return {false, Problem{ProblemCode::RecoveryJournalInitialize}};
        unsynced_.insert(directory_);
    }
    for (auto entry = transaction.payloads.cbegin(); entry != transaction.payloads.cend(); ++entry) {
        if (verified_.contains(entry.key())) continue;
        const auto path = payloadDirectory + QLatin1Char('/') + QString::fromLatin1(entry.key());
        if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) {
            Problem error;
            (void)payload(entry.key(), &error);
            if (!error.isEmpty()) return {false, error};
        } else {
            if (!atomicFile(*files_, path, entry.value())) return {false, Problem{ProblemCode::RecoveryPayloadCommit}};
            unsynced_.insert(payloadDirectory);
        }
        verified_.insert(entry.key());
    }
    for (auto pending = unsynced_.begin(); pending != unsynced_.end(); pending = unsynced_.erase(pending)) {
        if (!syncDirectory(*files_, *pending)) return {false, Problem{ProblemCode::RecoveryStorageSync}};
    }
    QFile journal(journalPath);
    const auto validBytes = current->size ? current->size : header.size();
    if (!journal.open(QIODevice::ReadWrite) || (journal.size() != validBytes && !journal.resize(validBytes))
        || !journal.seek(validBytes))
        return {false, Problem{ProblemCode::RecoveryCommitBoundary}};
    if (journal.write(frame) != frame.size() || !synchronize(*files_, journal)) {
        end_.reset();
        return {false, Problem{ProblemCode::RecoveryCommitFailed}};
    }
    if (!end_) end_ = *current;
    end_->size = size;
    ++end_->records;
    end_->stateBytes = retained;
    end_->retainedBytes = expanded;
    end_->lastDigest = bodyDigest;
    end_->committed.emplace(transaction.id, stateDigest(transaction.state, transaction.retainedBytes));
    end_->stamp = stampOf(journal);
    return {true, {}};
}

RecoveryLease::RecoveryLease(QString directory, std::shared_ptr<const FileSystem> files, RecoveryLimits limits)
    : lock_(std::make_unique<QLockFile>(directory + QStringLiteral("/session.lock"))),
      store_(std::move(directory), std::move(files), limits)
{}

RecoveryLease::~RecoveryLease() = default;

std::shared_ptr<RecoveryLease> RecoveryLease::acquire(const QString &directory, std::shared_ptr<const FileSystem> files,
                                                      Failure *failure, RecoveryLimits limits)
{
    const auto fail = [failure](Failure reason) {
        if (failure) *failure = reason;
        return std::shared_ptr<RecoveryLease>{};
    };
    if (!files || !QDir::isAbsolutePath(directory)) return fail(Failure::Unsafe);
    std::shared_ptr<RecoveryLease> lease(new RecoveryLease(directory, files, limits));
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
