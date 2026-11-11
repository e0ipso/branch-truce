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
#include <algorithm>
#include <fcntl.h>
#include <stdexcept>
#include <utility>

namespace branch_truce::storage {
namespace {
constexpr int journalVersion = 3;
constexpr auto headerPrefix = QByteArrayView("Branch Truce recovery journal v");
constexpr auto header = QByteArrayView("Branch Truce recovery journal v3\n");
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
        if ((c < '0' || c > '9') && (c < 'a' || c > 'f')) return false;
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
    const auto pending = path + QStringLiteral(".pending");
    if ((QFileInfo::exists(pending) || QFileInfo(pending).isSymLink())
        && (!regularPrivateFile(files, pending)
            || files.unlink(AT_FDCWD, QFile::encodeName(pending).constData(), 0) != 0))
        return false;
    QFile file(pending);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || !file.setPermissions(filePermissions)
        || file.write(bytes) != bytes.size() || !file.flush() || files.sync(file.handle()) != 0)
        return false;
    file.close();
    return files.rename(AT_FDCWD, QFile::encodeName(pending).constData(), AT_FDCWD, QFile::encodeName(path).constData())
        == 0;
}

bool payloadName(const QByteArray &name)
{
    return validDigest(name) || (name.endsWith(".pending") && validDigest(name.chopped(8)));
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

struct Frame {
    QByteArray bytes, digest;
    qint64 stateBytes;
};
Frame encodeFrame(const RecoveryTransaction &transaction, qint64 sequence, const QByteArray &previous)
{
    QJsonArray references;
    for (auto it = transaction.payloads.cbegin(); it != transaction.payloads.cend(); ++it)
        references.push_back(QString::fromLatin1(it.key()));
    const QJsonObject record{{"version", journalVersion},
                             {"sequence", QString::number(sequence)},
                             {"previous", QString::fromLatin1(previous)},
                             {"transaction", transaction.id},
                             {"retained", QString::number(transaction.retainedBytes)},
                             {"payloads", references},
                             {"state", transaction.state}};
    const auto body = QJsonDocument(record).toJson(QJsonDocument::Compact);
    const auto hash = RecoveryStore::digest(body);
    return {QJsonDocument(
                QJsonObject{{"record", QString::fromLatin1(body.toBase64())}, {"sha256", QString::fromLatin1(hash)}})
                    .toJson(QJsonDocument::Compact)
                + '\n',
            hash, body.size() + transaction.id.size() * 2 + 256};
}

struct DecodedFrame {
    QJsonObject record;
    QByteArray hash;
    QString id;
    qint64 expanded;
};
std::optional<DecodedFrame> decodeFrame(const QByteArray &line, quint64 sequence, const RecoveryLimits &limits,
                                        qint64 &retained, RecoveryRead &result)
{
    const auto refuse = [&](ProblemCode problem) -> std::optional<DecodedFrame> {
        result.problem = problem;
        return {};
    };
    QJsonParseError parse;
    const auto frame = QJsonDocument::fromJson(line, &parse);
    const auto object = frame.object();
    const auto encoded = object.value(QStringLiteral("record")).toString().toLatin1();
    const auto decodedSize = encoded.size() / 4 * 3 - (encoded.endsWith("==") ? 2 : encoded.endsWith('=') ? 1 : 0);
    if (!charge(retained, decodedSize, limits.stateBytes)) return refuse(ProblemCode::RecoveryResourceLimit);
    const auto body = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
    const auto hash = object.value(QStringLiteral("sha256")).toString().toLatin1();
    if (parse.error != QJsonParseError::NoError || !frame.isObject() || body.toBase64() != encoded || !validDigest(hash)
        || hash != RecoveryStore::digest(body)) {
        return refuse(ProblemCode::RecoveryRecordIntegrity);
    }
    const auto recordDocument = QJsonDocument::fromJson(body, &parse);
    const auto record = recordDocument.object();
    if (record.value(QStringLiteral("version")).toInt() > journalVersion) {
        result.newerFormat = true;
        return refuse(ProblemCode::RecoveryJournalNewer);
    }
    const auto id = record.value(QStringLiteral("transaction")).toString();
    bool retainedOk = false;
    const auto expanded = record.value(QStringLiteral("retained")).toString().toLongLong(&retainedOk);
    if (id.size() > 256 || !charge(retained, id.size() * 2 + 256, limits.stateBytes))
        return refuse(ProblemCode::RecoveryResourceLimit);
    if (parse.error != QJsonParseError::NoError || !recordDocument.isObject()
        || record.value(QStringLiteral("version")).toInt() != journalVersion
        || record.value(QStringLiteral("sequence")).toString() != QString::number(sequence)
        || record.value(QStringLiteral("previous")).toString().toLatin1() != result.lastDigest
        || !record.value(QStringLiteral("payloads")).isArray() || !record.value(QStringLiteral("state")).isObject()
        || id.isEmpty() || !retainedOk
        || QString::number(expanded) != record.value(QStringLiteral("retained")).toString()) {
        return refuse(ProblemCode::RecoveryRecordSchema);
    }
    if (!charge(result.retainedBytes, expanded, limits.expandedBytes))
        return refuse(ProblemCode::RecoveryResourceLimit);
    return DecodedFrame{record, hash, id, expanded};
}
bool decodeReceipts(const QJsonObject &record, quint64 sequence, const RecoveryLimits &limits,
                    std::set<QString> &identities, RecoveryRead &result)
{
    const auto state = record.value("state").toObject();
    if (state.contains("receipts")) {
        const auto value = state.value("receipts");
        if (sequence != 1 || !value.isArray() || value.toArray().size() > limits.transactions * 2) {
            result.problem = ProblemCode::RecoveryRecordSchema;
            return false;
        }
        for (const auto &item : value.toArray()) {
            const auto receipt = item.toArray();
            const auto identity = receipt.size() == 2 ? receipt[0].toString() : QString{};
            const auto hash = receipt.size() == 2 ? receipt[1].toString().toLatin1() : QByteArray{};
            if (identity.isEmpty() || identity.size() > 256 || !validDigest(hash)
                || !identities.insert(identity).second) {
                result.problem = ProblemCode::RecoveryRecordSchema;
                return false;
            }
            result.receipts.emplace(identity, hash);
        }
    }
    return true;
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
    // Replay hashes many small records. Reuse the algorithm context instead of
    // resolving the crypto provider for every value; each worker owns its state.
    thread_local QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.reset();
    hash.addData(bytes);
    return hash.result().toHex();
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
    auto bytes = file.read(maxBytes + 1);
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
    result.payloads = std::move(payloads);
    if (result.succeeded && file.error() != QFileDevice::NoError) {
        result.succeeded = false;
        result.problem = Problem{ProblemCode::RecoveryJournalIncomplete};
    }
    return result;
}

Problem RecoveryStore::retention(qint64 journalBytes, const QMap<QByteArray, QByteArray> &additional,
                                 qint64 generations) const
{
    qint64 used = 0, count = 0;
    if (!charge(used, journalBytes, limits_.storeBytes * generations)) return resourceLimit();
    const auto directory = directory_ + QStringLiteral("/payloads");
    QDirIterator entries(directory, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    while (entries.hasNext()) {
        entries.next();
        const auto info = entries.fileInfo();
        if (!charge(count, 1, limits_.payloadFiles * generations)
            || !charge(used, info.size(), limits_.storeBytes * generations))
            return resourceLimit();
        if (!payloadName(info.fileName().toLatin1()) || !regularPrivateFile(*files_, info.absoluteFilePath()))
            return ProblemCode::RecoveryPayloadUnsafe;
    }
    for (auto entry = additional.cbegin(); entry != additional.cend(); ++entry) {
        if (QFileInfo::exists(directory + QLatin1Char('/') + QString::fromLatin1(entry.key()))) continue;
        const QFileInfo pending(directory + QLatin1Char('/') + QString::fromLatin1(entry.key())
                                + QStringLiteral(".pending"));
        if (!charge(count, pending.exists() ? 0 : 1, limits_.payloadFiles * generations)
            || !charge(used, std::max(qint64{0}, entry.value().size() - (pending.exists() ? pending.size() : 0)),
                       limits_.storeBytes * generations))
            return resourceLimit();
    }
    return {};
}

Problem RecoveryStore::readPayloads(const QJsonObject &record, std::set<QByteArray> &payloads,
                                    qint64 &payloadBytes) const
{
    for (const auto &reference : record.value(QStringLiteral("payloads")).toArray()) {
        const auto name = reference.toString().toLatin1();
        if (payloads.contains(name)) continue;
        Problem error;
        const auto bytes = payload(name, &error);
        if (!charge(payloadBytes, bytes.size(), limits_.storeBytes)
            || payloads.size() >= static_cast<std::size_t>(limits_.payloadFiles))
            return resourceLimit();
        if (!error.isEmpty()) {
            return error;
        }
        payloads.insert(name);
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
    }
    result.validBytes = file.pos();
    quint64 sequence = 1;
    qint64 retained = 0, payloadBytes = file.size();
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
        const auto frame = decodeFrame(line, sequence, limits_, retained, result);
        if (!frame) return result;
        const auto &record = frame->record;
        const auto &id = frame->id;
        if (!identities.insert(id).second) {
            result.problem = Problem{ProblemCode::RecoveryTransactionRepeated};
            return result;
        }
        if (!decodeReceipts(record, sequence, limits_, identities, result)) return result;
        if (!firstRecordOnly) {
            result.problem = readPayloads(record, payloads, payloadBytes);
            if (!result.problem.isEmpty()) return result;
        }
        result.transactions.push_back({id, record.value(QStringLiteral("state")).toObject(), frame->expanded});
        result.validBytes = file.pos();
        result.lastDigest = frame->hash;
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
    // Reopening after a crash cannot know whether a previous rename's parent
    // sync finished. Stabilize both namespaces before acknowledging a retry or
    // a new append that depends on them.
    unsynced_.insert(directory_);
    unsynced_.insert(directory_ + QStringLiteral("/payloads"));
    end_->committed = current.receipts;
    for (const auto &stored : current.transactions)
        end_->committed.emplace(stored.id, stateDigest(stored.state, stored.retainedBytes));
    return &*end_;
}

RecoveryCommit RecoveryStore::commit(const RecoveryTransaction &transaction)
{
    const std::lock_guard lock(commits_);
    try {
        return append(transaction);
    } catch (const std::length_error &) {
        return {false, resourceLimit()};
    } catch (...) {
        return {false, ProblemCode::RecoveryCommitFailed};
    }
}

struct RecoveryStore::AppendAdmission {
    QByteArray frame, digest;
    qint64 size, stateBytes, retainedBytes;
};

Problem RecoveryStore::writePayloads(const RecoveryTransaction &transaction, const QString &payloadDirectory)
{
    for (auto entry = transaction.payloads.cbegin(); entry != transaction.payloads.cend(); ++entry) {
        if (verified_.contains(entry.key())) continue;
        const auto path = payloadDirectory + QLatin1Char('/') + QString::fromLatin1(entry.key());
        if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) {
            Problem error;
            (void)payload(entry.key(), &error);
            if (!error.isEmpty()) return error;
        } else {
            if (!atomicFile(*files_, path, entry.value())) return ProblemCode::RecoveryPayloadCommit;
            unsynced_.insert(payloadDirectory);
        }
        verified_.insert(entry.key());
    }
    return {};
}

RecoveryCommit RecoveryStore::appendPrepared(const RecoveryTransaction &transaction, const JournalEnd &current,
                                             const AppendAdmission &admission)
{
    const auto journalPath = directory_ + QStringLiteral("/journal");
    // Admission is complete before creating payloads, initializing the journal
    // or cutting an interrupted tail. Rejections leave the store unchanged.
    const auto payloadDirectory = directory_ + QStringLiteral("/payloads");
    if (!privateDirectory(*files_, directory_, unsynced_) || !privateDirectory(*files_, payloadDirectory, unsynced_))
        return {false, Problem{ProblemCode::RecoveryStorageCreate}};
    if (!QFileInfo::exists(journalPath)) {
        if (!atomicFile(*files_, journalPath, QByteArray(header)))
            return {false, Problem{ProblemCode::RecoveryJournalInitialize}};
        unsynced_.insert(directory_);
    }
    if (const auto error = writePayloads(transaction, payloadDirectory); !error.isEmpty()) return {false, error};
    for (auto pending = unsynced_.begin(); pending != unsynced_.end(); pending = unsynced_.erase(pending)) {
        if (!syncDirectory(*files_, *pending)) return {false, Problem{ProblemCode::RecoveryStorageSync}};
    }
    QFile journal(journalPath);
    const auto validBytes = current.size ? current.size : header.size();
    if (!journal.open(QIODevice::ReadWrite) || (journal.size() != validBytes && !journal.resize(validBytes))
        || !journal.seek(validBytes))
        return {false, Problem{ProblemCode::RecoveryCommitBoundary}};
    if (journal.write(admission.frame) != admission.frame.size() || !synchronize(*files_, journal)) {
        uncertain_.insert_or_assign(transaction.id, stateDigest(transaction.state, transaction.retainedBytes));
        end_.reset();
        return {false, Problem{ProblemCode::RecoveryCommitFailed}};
    }
    if (!end_) end_ = current;
    end_->size = admission.size;
    ++end_->records;
    end_->stateBytes = admission.stateBytes;
    end_->retainedBytes = admission.retainedBytes;
    end_->lastDigest = admission.digest;
    end_->committed.emplace(transaction.id, stateDigest(transaction.state, transaction.retainedBytes));
    end_->stamp = stampOf(journal);
    uncertain_.erase(transaction.id);
    return {true, {}};
}

RecoveryCommit RecoveryStore::append(const RecoveryTransaction &transaction, bool continuing)
{
    if (!QDir::isAbsolutePath(directory_)) return {false, Problem{ProblemCode::RecoveryStorageLocation}};
    if (transaction.id.isEmpty() || transaction.state.isEmpty())
        return {false, Problem{ProblemCode::RecoveryTransactionInvalid}};
    if (transaction.id.size() > 256) return {false, resourceLimit()};
    Problem readError;
    const auto *current = refreshEnd(&readError);
    if (!current) return {false, readError};
    const auto exhausted = [&] {
        return continuing && transaction.continuation ? rollover(transaction) : RecoveryCommit{false, resourceLimit()};
    };
    const auto journalPath = directory_ + QStringLiteral("/journal");
    // A complete record left by a failed sync keeps its ID, even at capacity.
    // A retry flushes it without admitting new history or touching payloads.
    if (const auto committed = current->committed.find(transaction.id); committed != current->committed.end()) {
        if (committed->second != stateDigest(transaction.state, transaction.retainedBytes))
            return {false, Problem{ProblemCode::RecoveryTransactionReused}};
        QFile journal(journalPath);
        if (!journal.open(QIODevice::ReadWrite) || !synchronize(*files_, journal))
            return {false, Problem{ProblemCode::RecoveryPendingFlush}};
        for (auto pending = unsynced_.begin(); pending != unsynced_.end(); pending = unsynced_.erase(pending))
            if (!syncDirectory(*files_, *pending)) return {false, ProblemCode::RecoveryStorageSync};
        if (end_) end_->stamp = stampOf(journal);
        uncertain_.erase(transaction.id);
        return {true, {}};
    }
    if (uncertain_.size() >= static_cast<std::size_t>(limits_.transactions) && !uncertain_.contains(transaction.id))
        return {false, resourceLimit()};
    if (current->records >= limits_.transactions) return exhausted();
    auto expanded = current->retainedBytes;
    if (!charge(expanded, transaction.retainedBytes, limits_.expandedBytes)) return exhausted();
    if (transaction.payloads.size() > limits_.payloadFiles) return {false, resourceLimit()};
    for (auto entry = transaction.payloads.cbegin(); entry != transaction.payloads.cend(); ++entry) {
        if (entry.value().size() > static_cast<qsizetype>(branch_truce::kMaxRecoveryPayloadBytes))
            return {false, payloadLimitError(entry.value().size())};
        if (!validDigest(entry.key()) || digest(entry.value()) != entry.key())
            return {false, Problem{ProblemCode::RecoveryPayloadIdentity}};
    }
    const auto encoded = encodeFrame(transaction, current->records + 1, current->lastDigest);
    auto retained = current->stateBytes;
    if (!charge(retained, encoded.stateBytes, limits_.stateBytes)) return exhausted();
    const auto &frame = encoded.bytes;
    auto size = current->size ? current->size : header.size();
    if (frame.size() > limits_.frameBytes) return {false, resourceLimit()};
    if (!charge(size, frame.size(), limits_.journalBytes)) return exhausted();
    if (const auto error = retention(size, transaction.payloads); !error.isEmpty()) {
        if (error.code == ProblemCode::RecoveryResourceLimit) return exhausted();
        return {false, error};
    }

    return appendPrepared(transaction, *current, {encoded.bytes, encoded.digest, size, retained, expanded});
}

Problem RecoveryStore::reclaim(const std::set<QByteArray> &reachable)
{
    // A failed post-rename sync may leave either authoritative journal after a
    // power loss. Sync that name before reclaiming any old references.
    if (!syncDirectory(*files_, directory_)) return ProblemCode::RecoveryStorageSync;
    const auto directory = directory_ + QStringLiteral("/payloads");
    QDirIterator entries(directory, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    qint64 count = 0;
    while (entries.hasNext()) {
        entries.next();
        if (++count > 2 * limits_.payloadFiles) return resourceLimit();
        const auto name = entries.fileName().toLatin1();
        if (!payloadName(name) || !regularPrivateFile(*files_, entries.filePath()))
            return ProblemCode::RecoveryPayloadUnsafe;
        if (!reachable.contains(name)
            && files_->unlink(AT_FDCWD, QFile::encodeName(entries.filePath()).constData(), 0) != 0)
            return ProblemCode::RecoveryStorageSync;
    }
    verified_.clear();
    return syncDirectory(*files_, directory) ? Problem{} : Problem{ProblemCode::RecoveryStorageSync};
}

RecoveryCommit RecoveryStore::rollover(const RecoveryTransaction &transaction)
{
    auto checkpoint = transaction.continuation(*this);
    const auto read = this->read();
    if (!read.succeeded) return {false, read.problem};
    QJsonArray receipts;
    std::map<QString, QByteArray> kept;
    for (const auto &stored : read.transactions)
        kept.emplace(stored.id, stateDigest(stored.state, stored.retainedBytes));
    // Failed flushes can still be retried after several continuations. Their
    // exact identities remain pinned until a successful retry settles them.
    if (!end_) return {false, ProblemCode::RecoveryCommitFailed};
    const auto &committed = end_->committed;
    for (const auto &[id, hash] : uncertain_)
        if (committed.contains(id)) kept.insert_or_assign(id, hash);
    if (kept.size() > static_cast<std::size_t>(limits_.transactions * 2)) return {false, resourceLimit()};
    for (const auto &[id, hash] : kept) receipts.push_back(QJsonArray{id, QString::fromLatin1(hash)});
    checkpoint.state.insert("receipts", receipts);
    const auto head = encodeFrame(checkpoint, 1, {});
    const auto next = encodeFrame(transaction, 2, head.digest);
    qint64 journal = header.size(), state = 0, expanded = 0, payloadBytes = 0;
    auto reachable = checkpoint.payloads;
    for (auto it = transaction.payloads.cbegin(); it != transaction.payloads.cend(); ++it)
        reachable.insert(it.key(), it.value());
    if (limits_.transactions < 2 || head.bytes.size() > limits_.frameBytes || next.bytes.size() > limits_.frameBytes
        || !charge(journal, head.bytes.size() + next.bytes.size(), limits_.journalBytes)
        || !charge(state, head.stateBytes + next.stateBytes, limits_.stateBytes)
        || !charge(expanded, checkpoint.retainedBytes, limits_.expandedBytes)
        || !charge(expanded, transaction.retainedBytes, limits_.expandedBytes)
        || reachable.size() > limits_.payloadFiles)
        return {false, resourceLimit()};
    payloadBytes = journal;
    for (auto it = reachable.cbegin(); it != reachable.cend(); ++it) {
        if (!validDigest(it.key()) || digest(it.value()) != it.key())
            return {false, ProblemCode::RecoveryPayloadIdentity};
        if (it.value().size() > static_cast<qint64>(kMaxRecoveryPayloadBytes)
            || !charge(payloadBytes, it.value().size(), limits_.storeBytes))
            return {false, resourceLimit()};
    }
    // Reclaim earlier failed attempts against the durably published current
    // journal before reserving another generation. Its references remain intact.
    if (const auto error = reclaim(read.payloads); !error.isEmpty()) return {false, error};
    // At most two admitted payload generations can exist, including failed
    // attempts. No previous journal generation is accumulated.
    const auto physical = retention(journal, reachable, 2);
    if (!physical.isEmpty()) return {false, physical};
    const auto payloadDirectory = directory_ + QStringLiteral("/payloads");
    for (auto it = checkpoint.payloads.cbegin(); it != checkpoint.payloads.cend(); ++it) {
        const auto path = payloadDirectory + QLatin1Char('/') + QString::fromLatin1(it.key());
        if (QFileInfo::exists(path)) {
            Problem error;
            (void)payload(it.key(), &error);
            if (!error.isEmpty()) return {false, error};
        } else if (!atomicFile(*files_, path, it.value())) {
            return {false, ProblemCode::RecoveryPayloadCommit};
        }
    }
    if (!syncDirectory(*files_, payloadDirectory)) return {false, ProblemCode::RecoveryStorageSync};
    const auto staging = directory_ + QStringLiteral("/journal-next");
    if (QFileInfo::exists(staging) && files_->unlink(AT_FDCWD, QFile::encodeName(staging).constData(), 0) != 0)
        return {false, ProblemCode::RecoveryStorageSync};
    if (!atomicFile(*files_, staging, QByteArray(header) + head.bytes))
        return {false, ProblemCode::RecoveryJournalInitialize};
    const auto destination = directory_ + QStringLiteral("/journal");
    if (files_->rename(AT_FDCWD, QFile::encodeName(staging).constData(), AT_FDCWD,
                       QFile::encodeName(destination).constData())
        != 0)
        return {false, ProblemCode::RecoveryCommitFailed};
    end_.reset();
    verified_.clear();
    unsynced_.insert(directory_);
    // Publication is authoritative even if cleanup fails. No input transaction
    // was acknowledged or erased. Its retry starts from the equivalent checkpoint.
    std::set<QByteArray> references;
    for (auto it = checkpoint.payloads.cbegin(); it != checkpoint.payloads.cend(); ++it) references.insert(it.key());
    if (const auto error = reclaim(references); !error.isEmpty()) return {false, error};
    return append(transaction, false);
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
    std::shared_ptr<RecoveryLease> lease(new RecoveryLease(directory, std::move(files), limits));
    if (!privateDirectory(*lease->store_.files_, directory, lease->store_.unsynced_)) return fail(Failure::Unsafe);
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
