#include "MergetoolProtocol.h"
#include "core/ResourceLimits.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QUuid>

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace branch_truce {
namespace {
constexpr qsizetype kFrameLimit = 65536;

QString fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return {};
}

bool failBool(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

QByteArray sha256(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

int openWalk(const QByteArray &path, bool directory, QString *error, bool writable = false)
{
    if (path.isEmpty() || path.contains('\0')) {
        fail(error, QStringLiteral("The mergetool path is empty."));
        return -1;
    }
    const bool absolute = path.startsWith('/');
    int fd = ::open(absolute ? "/" : ".", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) {
        fail(error, QStringLiteral("The mergetool path could not be opened."));
        return -1;
    }
    const auto parts = path.split('/');
    qsizetype meaningful = 0;
    for (const auto &part : parts)
        if (!part.isEmpty() && part != ".") ++meaningful;
    if (meaningful == 0) {
        ::close(fd);
        fail(error, QStringLiteral("The mergetool path is empty."));
        return -1;
    }
    qsizetype seen = 0;
    for (const auto &part : parts) {
        if (part.isEmpty() || part == ".") continue;
        if (part == "..") {
            ::close(fd);
            fail(error, QStringLiteral("The mergetool path leaves its directory."));
            return -1;
        }
        ++seen;
        const bool last = seen == meaningful;
        const int access = last && writable ? O_WRONLY
            : (last && !directory ? (O_RDONLY | O_NONBLOCK) : (O_RDONLY | O_DIRECTORY));
        const int flags = O_CLOEXEC | O_NOFOLLOW | access;
        const int next = ::openat(fd, part.constData(), flags);
        ::close(fd);
        if (next < 0) {
            fail(error, QStringLiteral("The mergetool path could not be opened."));
            return -1;
        }
        fd = next;
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0 || (directory ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode))) {
        ::close(fd);
        fail(error, QStringLiteral("The mergetool path is not a regular file."));
        return -1;
    }
    return fd;
}
}

QByteArray mergetoolSessionKey(const git::RepositoryContext &repository)
{
    QByteArray operation;
    if (repository.operation == git::RepositoryOperation::Merge) operation = "merge";
    else if (repository.operation == git::RepositoryOperation::Rebase) operation = "rebase";
    else return {};
    if (!repository.supported || repository.worktreeRoot.isEmpty()
        || repository.stage2.commitId.isEmpty() || repository.stage3.commitId.isEmpty()) return {};
    const auto material = QFile::encodeName(repository.worktreeRoot) + '\n' + operation + '\n'
        + repository.stage2.commitId + '\n' + repository.stage3.commitId;
    return sha256(material);
}

QString mergetoolServerName(const git::RepositoryContext &repository)
{
    const auto key = mergetoolSessionKey(repository);
    if (key.isEmpty()) return {};
    return QStringLiteral("bt-mt-") + QString::fromLatin1(key);
}

QString mergetoolIntakeRoot()
{
    const auto base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty()) return {};
    return base + QStringLiteral("/branch-truce/mergetool");
}

std::optional<QByteArray> mergetoolRepositoryPath(const QByteArray &merged)
{
    auto path = merged;
    if (path.startsWith("./")) path = path.mid(2);
    if (path.isEmpty() || path.startsWith('/') || path.contains('\0') || path.contains('\\')) return std::nullopt;
    for (const auto &part : path.split('/')) {
        if (part.isEmpty() || part == "." || part == "..") return std::nullopt;
    }
    return path;
}

bool readMergetoolFile(const QByteArray &path, QByteArray *bytes, QString *error)
{
    if (!bytes) return failBool(error, QStringLiteral("The mergetool file could not be read."));
    const int fd = openWalk(path, false, error);
    if (fd < 0) return false;
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        return failBool(error, QStringLiteral("The mergetool file could not be read."));
    }
    constexpr qsizetype limit = static_cast<qsizetype>(kMaxRecoveryPayloadBytes);
    *bytes = file.read(limit + 1);
    if (file.error() != QFileDevice::NoError || bytes->size() > limit) {
        bytes->clear();
        return failBool(error, QStringLiteral("The mergetool file exceeds the 8388608 byte recovery payload limit."));
    }
    return true;
}

bool restoreMergetoolFile(const QByteArray &path, const QByteArray &bytes, QString *error)
{
    QByteArray current;
    if (!readMergetoolFile(path, &current, error)) return false;
    if (current == bytes) return true;
    const int fd = openWalk(path, false, error, true);
    if (fd < 0) return failBool(error, QStringLiteral("The original file could not be restored."));
    QFile file;
    // Truncate is ignored for an already-open descriptor. Shrink after the
    // new bytes are flushed so a shorter restore does not keep the old tail.
    if (!file.open(fd, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        return failBool(error, QStringLiteral("The original file could not be restored."));
    }
    const auto wrote = file.write(bytes);
    if (wrote != static_cast<qint64>(bytes.size()) || !file.flush()
        || !file.resize(static_cast<qint64>(bytes.size())) || ::fsync(file.handle()) != 0)
        return failBool(error, QStringLiteral("The original file could not be restored."));
    return true;
}

bool writeMergetoolIntake(const QString &directory, const QString &invocation, const MergetoolSources &sources, QString *error)
{
    if (QUuid::fromString(invocation).isNull() || !mergetoolRepositoryPath(sources.path))
        return failBool(error, QStringLiteral("The mergetool invocation is invalid."));
    QDir root;
    if (!root.mkpath(directory)) return failBool(error, QStringLiteral("The mergetool intake could not be created."));
    const auto encoded = QFile::encodeName(directory);
    if (::chmod(encoded.constData(), 0700) != 0)
        return failBool(error, QStringLiteral("The mergetool intake could not be created."));
    const int dirfd = openWalk(encoded, true, error);
    if (dirfd < 0) return false;
    const auto store = [&](const char *name, const QByteArray &bytes) {
        const int fd = ::openat(dirfd, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) return failBool(error, QStringLiteral("The mergetool intake could not be written."));
        QFile file;
        if (!file.open(fd, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
            ::close(fd);
            return failBool(error, QStringLiteral("The mergetool intake could not be written."));
        }
        const auto wrote = file.write(bytes);
        return wrote == static_cast<qint64>(bytes.size()) && file.flush() && ::fsync(file.handle()) == 0
            ? true : failBool(error, QStringLiteral("The mergetool intake could not be written."));
    };
    bool stored = store("merged", sources.merged);
    if (stored && sources.basePresent) stored = store("base", sources.base);
    if (stored && sources.localPresent) stored = store("local", sources.local);
    if (stored && sources.remotePresent) stored = store("remote", sources.remote);
    if (!stored) {
        ::close(dirfd);
        return false;
    }
    const QJsonObject manifest{
        {QStringLiteral("v"), 1},
        {QStringLiteral("invocation"), invocation},
        {QStringLiteral("path"), QString::fromLatin1(sources.path.toHex())},
        {QStringLiteral("merged"), QString::fromLatin1(sha256(sources.merged))},
        {QStringLiteral("basePresent"), sources.basePresent},
        {QStringLiteral("localPresent"), sources.localPresent},
        {QStringLiteral("remotePresent"), sources.remotePresent},
        {QStringLiteral("base"), sources.basePresent ? QString::fromLatin1(sha256(sources.base)) : QString{}},
        {QStringLiteral("local"), sources.localPresent ? QString::fromLatin1(sha256(sources.local)) : QString{}},
        {QStringLiteral("remote"), sources.remotePresent ? QString::fromLatin1(sha256(sources.remote)) : QString{}}};
    const auto payload = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    stored = store("manifest.json", payload);
    if (stored && ::fsync(dirfd) != 0) stored = failBool(error, QStringLiteral("The mergetool intake could not be written."));
    ::close(dirfd);
    return stored;
}

std::optional<MergetoolIntake> readMergetoolIntake(const QString &directory, const QByteArray &sessionKey, QString *error)
{
    const auto root = mergetoolIntakeRoot();
    const auto canonicalRoot = QFileInfo(root).canonicalFilePath();
    const auto canonical = QFileInfo(directory).canonicalFilePath();
    const auto prefix = canonicalRoot + QLatin1Char('/');
    if (sessionKey.size() != 64 || canonicalRoot.isEmpty() || !canonical.startsWith(prefix + QString::fromLatin1(sessionKey) + QLatin1Char('/'))) {
        fail(error, QStringLiteral("The mergetool intake is outside app-owned storage."));
        return std::nullopt;
    }
    const auto invocation = QFileInfo(canonical).fileName();
    if (QUuid::fromString(invocation).isNull() || QFileInfo(canonical).dir().dirName() != QString::fromLatin1(sessionKey)) {
        fail(error, QStringLiteral("The mergetool intake identity does not match this session."));
        return std::nullopt;
    }
    QByteArray manifestBytes;
    if (!readMergetoolFile(QFile::encodeName(canonical + QStringLiteral("/manifest.json")), &manifestBytes, error))
        return std::nullopt;
    const auto manifest = QJsonDocument::fromJson(manifestBytes).object();
    if (manifest.value(QStringLiteral("v")).toInt() != 1 || manifest.value(QStringLiteral("invocation")).toString() != invocation) {
        fail(error, QStringLiteral("The mergetool intake manifest is invalid."));
        return std::nullopt;
    }
    const auto path = QByteArray::fromHex(manifest.value(QStringLiteral("path")).toString().toLatin1());
    if (!mergetoolRepositoryPath(path) || QString::fromLatin1(path.toHex()) != manifest.value(QStringLiteral("path")).toString()) {
        fail(error, QStringLiteral("The mergetool intake path is invalid."));
        return std::nullopt;
    }
    const auto hashed = [&](const QString &name, const QString &expected, QByteArray *digest) {
        QByteArray bytes;
        if (!readMergetoolFile(QFile::encodeName(canonical + QLatin1Char('/') + name), &bytes, error)) return false;
        *digest = sha256(bytes);
        if (QString::fromLatin1(*digest) != expected) {
            fail(error, QStringLiteral("The mergetool intake bytes do not match its manifest."));
            return false;
        }
        return true;
    };
    MergetoolIntake intake;
    intake.invocation = invocation;
    intake.path = path;
    if (!hashed(QStringLiteral("merged"), manifest.value(QStringLiteral("merged")).toString(), &intake.mergedSha256))
        return std::nullopt;
    intake.basePresent = manifest.value(QStringLiteral("basePresent")).toBool();
    intake.localPresent = manifest.value(QStringLiteral("localPresent")).toBool();
    intake.remotePresent = manifest.value(QStringLiteral("remotePresent")).toBool();
    if (intake.basePresent && !hashed(QStringLiteral("base"), manifest.value(QStringLiteral("base")).toString(), &intake.baseSha256))
        return std::nullopt;
    if (intake.localPresent && !hashed(QStringLiteral("local"), manifest.value(QStringLiteral("local")).toString(), &intake.localSha256))
        return std::nullopt;
    if (intake.remotePresent && !hashed(QStringLiteral("remote"), manifest.value(QStringLiteral("remote")).toString(), &intake.remoteSha256))
        return std::nullopt;
    return intake;
}

MergetoolFrame takeMergetoolFrame(QByteArray *buffer, QJsonObject *object, QString *error)
{
    if (!buffer || !object) {
        fail(error, QStringLiteral("The mergetool message is invalid."));
        return MergetoolFrame::Invalid;
    }
    if (buffer->size() < 4) return MergetoolFrame::NeedMore;
    const auto length = (static_cast<quint32>(static_cast<unsigned char>(buffer->at(0))) << 24)
        | (static_cast<quint32>(static_cast<unsigned char>(buffer->at(1))) << 16)
        | (static_cast<quint32>(static_cast<unsigned char>(buffer->at(2))) << 8)
        | static_cast<quint32>(static_cast<unsigned char>(buffer->at(3)));
    if (length == 0 || length > static_cast<quint32>(kFrameLimit)) {
        fail(error, QStringLiteral("The mergetool message is invalid."));
        return MergetoolFrame::Invalid;
    }
    if (buffer->size() < 4 + static_cast<qsizetype>(length)) return MergetoolFrame::NeedMore;
    const auto payload = buffer->mid(4, static_cast<qsizetype>(length));
    buffer->remove(0, 4 + static_cast<qsizetype>(length));
    const auto document = QJsonDocument::fromJson(payload);
    if (!document.isObject()) {
        fail(error, QStringLiteral("The mergetool message is invalid."));
        return MergetoolFrame::Invalid;
    }
    *object = document.object();
    return MergetoolFrame::Ready;
}

bool writeMergetoolFrame(QIODevice *device, const QJsonObject &object, QString *error)
{
    if (!device) return failBool(error, QStringLiteral("The mergetool message could not be written."));
    const auto payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (payload.size() <= 0 || payload.size() > kFrameLimit)
        return failBool(error, QStringLiteral("The mergetool message is invalid."));
    const auto length = static_cast<quint32>(payload.size());
    QByteArray frame(4 + payload.size(), '\0');
    frame[0] = static_cast<char>((length >> 24) & 0xffU);
    frame[1] = static_cast<char>((length >> 16) & 0xffU);
    frame[2] = static_cast<char>((length >> 8) & 0xffU);
    frame[3] = static_cast<char>(length & 0xffU);
    frame.replace(4, payload.size(), payload);
    const auto wrote = device->write(frame);
    return wrote == static_cast<qint64>(frame.size()) && device->waitForBytesWritten(1000)
        ? true : failBool(error, QStringLiteral("The mergetool message could not be written."));
}
}
