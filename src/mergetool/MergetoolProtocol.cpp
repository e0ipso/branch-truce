// The mergetool protocol names a session's socket and intake, and reads and restores the files Git launched with.
#include "mergetool/MergetoolProtocol.h"
#include "core/ResourceLimits.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace branch_truce {
namespace {
// sun_path keeps a byte for the terminating NUL. On Linux, Qt 6 also reserves
// one for the abstract-namespace prefix, even for a filesystem path, so its
// QLocalServer rejects a 107-byte path with "Name error".
constexpr qsizetype kSocketPathLimit = sizeof(sockaddr_un::sun_path) - 2;
constexpr char kSocketPrefix[] = "bt-mt-";
// 128 bits of the session key name the socket; the intake is still checked
// against the full key.
constexpr qsizetype kSocketKeyLength = 32;
constexpr qsizetype kSocketNameLength = sizeof(kSocketPrefix) - 1 + kSocketKeyLength;
// The file naming the launcher that owns an intake, written before its copies.
constexpr char kOwnerFile[] = "launcher";
// An intake with no owner file yet is still being written, unless it is older than this.
constexpr qint64 kUnownedIntakeSeconds = 60;

bool socketPathFits(const QByteArray &directory)
{
    return directory.size() + 1 + kSocketNameLength <= kSocketPathLimit;
}

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

int openWalk(const QByteArray &path, bool directory, QString *error)
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
        const int access = last && !directory ? (O_RDONLY | O_NONBLOCK) : (O_RDONLY | O_DIRECTORY);
        const int next = ::openat(fd, part.constData(), O_CLOEXEC | O_NOFOLLOW | access);
        ::close(fd);
        if (next < 0) {
            fail(error, QStringLiteral("The mergetool path could not be opened."));
            return -1;
        }
        fd = next;
    }
    struct stat info{};
    if (::fstat(fd, &info) != 0 || (directory ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode))) {
        ::close(fd);
        fail(error, QStringLiteral("The mergetool path is not a regular file."));
        return -1;
    }
    return fd;
}

// Creates one directory level with mode 0700, or accepts an existing one that
// is a real directory private to this user.
bool privateDirectory(const QString &path)
{
    const auto native = QFile::encodeName(path);
    if (::mkdir(native.constData(), 0700) != 0 && errno != EEXIST) return false;
    struct stat info{};
    return ::lstat(native.constData(), &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == ::geteuid()
        && (info.st_mode & 077) == 0;
}

bool writeAll(int fd, const QByteArray &bytes)
{
    qsizetype written = 0;
    while (written < bytes.size()) {
        const auto count = ::write(fd, bytes.constData() + written, static_cast<std::size_t>(bytes.size() - written));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        written += count;
    }
    return true;
}

bool launcherAlive(const QByteArray &owner)
{
    bool number = false;
    const auto pid = owner.trimmed().toLongLong(&number);
    if (!number || pid <= 0) return false;
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
}
}

QByteArray mergetoolSessionKey(const git::RepositoryContext &repository)
{
    QByteArray operation;
    if (repository.operation == git::RepositoryOperation::Merge)
        operation = "merge";
    else if (repository.operation == git::RepositoryOperation::Rebase)
        operation = "rebase";
    else
        return {};
    if (!repository.supported || repository.worktreeRoot.isEmpty() || repository.stage2.commitId.isEmpty()
        || repository.stage3.commitId.isEmpty())
        return {};
    const auto material = QFile::encodeName(repository.worktreeRoot) + '\n' + operation + '\n'
        + repository.stage2.commitId + '\n' + repository.stage3.commitId;
    return sha256(material);
}

QString mergetoolSocketDirectory(const QByteArray &runtimeDirectory)
{
    auto runtime = runtimeDirectory;
    if (runtime.startsWith('/')) {
        while (runtime.endsWith('/')) runtime.chop(1);
        const auto directory = runtime + "/branch-truce";
        if (socketPathFits(directory)) return QFile::decodeName(directory);
    }
    return QStringLiteral("/tmp/branch-truce-") + QString::number(::geteuid());
}

QString mergetoolServerName(const QString &directory, const git::RepositoryContext &repository)
{
    const auto key = mergetoolSessionKey(repository);
    if (key.isEmpty() || directory.isEmpty()) return {};
    return directory + QLatin1Char('/') + QString::fromLatin1(kSocketPrefix)
        + QString::fromLatin1(key.left(kSocketKeyLength));
}

bool prepareMergetoolSocketDirectory(const QString &directory, QString *error)
{
    const auto native = QFile::encodeName(directory);
    if (native.isEmpty() || (::mkdir(native.constData(), 0700) != 0 && errno != EEXIST))
        return failBool(error,
                        QStringLiteral("The mergetool socket directory %1 could not be created.").arg(directory));
    struct stat info{};
    if (::lstat(native.constData(), &info) != 0 || !S_ISDIR(info.st_mode))
        return failBool(error, QStringLiteral("The mergetool socket directory %1 is not a directory.").arg(directory));
    if (info.st_uid != ::geteuid() || (info.st_mode & 077) != 0)
        return failBool(
            error, QStringLiteral("The mergetool socket directory %1 is not private to this user.").arg(directory));
    return true;
}

bool mergetoolPeerIsCurrentUser(qintptr descriptor)
{
    struct ucred peer{};
    socklen_t size = sizeof(peer);
    if (descriptor < 0 || ::getsockopt(static_cast<int>(descriptor), SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0
        || size != sizeof(peer))
        return false;
    return peer.uid == ::geteuid();
}

QString mergetoolIntakeRoot(const QString &socketDirectory)
{
    return socketDirectory.isEmpty() ? QString{} : socketDirectory + QStringLiteral("/intake");
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
        return failBool(error,
                        QStringLiteral("The mergetool file exceeds the %1 byte recovery payload limit.")
                            .arg(static_cast<qulonglong>(limit)));
    }
    return true;
}

bool restoreMergetoolFile(const QByteArray &path, const QByteArray &bytes, QString *error)
{
    const auto restoreFailed = [error] {
        return failBool(error, QStringLiteral("The original file could not be restored."));
    };
    QByteArray current;
    if (!readMergetoolFile(path, &current, error)) return false;
    if (current == bytes) return true;
    const auto slash = path.lastIndexOf('/');
    const auto name = path.mid(slash + 1);
    const int directory = slash < 0 ? ::open(".", O_RDONLY | O_CLOEXEC | O_DIRECTORY)
                                    : openWalk(slash == 0 ? QByteArray("/") : path.first(slash), true, error);
    if (directory < 0) return restoreFailed();
    struct stat original{};
    if (::fstatat(directory, name.constData(), &original, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(original.st_mode)) {
        ::close(directory);
        return restoreFailed();
    }
    // A sibling, so the rename stays on one file system. Git's own temporaries
    // use other names, and the random part keeps two restores apart.
    const auto temporary =
        "." + name + ".branch-truce-" + QUuid::createUuid().toString(QUuid::Id128).left(12).toLatin1();
    const int fd =
        ::openat(directory, temporary.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        ::close(directory);
        return restoreFailed();
    }
    const bool written = writeAll(fd, bytes) && ::fchmod(fd, original.st_mode & 07777) == 0 && ::fsync(fd) == 0;
    const bool closed = ::close(fd) == 0;
    const bool renamed =
        written && closed && ::renameat(directory, temporary.constData(), directory, name.constData()) == 0;
    if (!renamed) ::unlinkat(directory, temporary.constData(), 0);
    const bool synced = renamed && ::fsync(directory) == 0;
    ::close(directory);
    return synced ? true : restoreFailed();
}

QString writeMergetoolIntake(const QString &root, const QByteArray &sessionKey, const QString &invocation,
                             const MergetoolSources &sources, QString *error)
{
    if (QUuid::fromString(invocation).isNull() || !mergetoolRepositoryPath(sources.path) || sessionKey.size() != 64
        || root.isEmpty())
        return fail(error, QStringLiteral("The mergetool invocation is invalid."));
    const auto keyDirectory = root + QLatin1Char('/') + QString::fromLatin1(sessionKey);
    const auto directory = keyDirectory + QLatin1Char('/') + invocation;
    if (!privateDirectory(root) || !privateDirectory(keyDirectory) || !privateDirectory(directory))
        return fail(error, QStringLiteral("The mergetool intake could not be created."));
    const int dirfd = openWalk(QFile::encodeName(directory), true, error);
    if (dirfd < 0) return {};
    const auto store = [&](const char *name, const QByteArray &bytes) {
        const int fd = ::openat(dirfd, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) return failBool(error, QStringLiteral("The mergetool intake could not be written."));
        const bool written = writeAll(fd, bytes) && ::fsync(fd) == 0;
        return ::close(fd) == 0 && written
            ? true
            : failBool(error, QStringLiteral("The mergetool intake could not be written."));
    };
    bool stored = store(kOwnerFile, QByteArray::number(static_cast<qint64>(::getpid())) + '\n');
    if (stored) stored = store("merged", sources.merged);
    if (stored && sources.basePresent) stored = store("base", sources.base);
    if (stored && sources.localPresent) stored = store("local", sources.local);
    if (stored && sources.remotePresent) stored = store("remote", sources.remote);
    if (stored) {
        const QJsonObject manifest{
            {QStringLiteral("invocation"), invocation},
            {QStringLiteral("path"), QString::fromLatin1(sources.path.toHex())},
            {QStringLiteral("merged"), QString::fromLatin1(sha256(sources.merged))},
            {QStringLiteral("basePresent"), sources.basePresent},
            {QStringLiteral("localPresent"), sources.localPresent},
            {QStringLiteral("remotePresent"), sources.remotePresent},
            {QStringLiteral("base"), sources.basePresent ? QString::fromLatin1(sha256(sources.base)) : QString{}},
            {QStringLiteral("local"), sources.localPresent ? QString::fromLatin1(sha256(sources.local)) : QString{}},
            {QStringLiteral("remote"),
             sources.remotePresent ? QString::fromLatin1(sha256(sources.remote)) : QString{}}};
        stored = store("manifest.json", QJsonDocument(manifest).toJson(QJsonDocument::Compact));
    }
    if (stored && ::fsync(dirfd) != 0)
        stored = failBool(error, QStringLiteral("The mergetool intake could not be written."));
    ::close(dirfd);
    if (!stored) {
        removeMergetoolIntake(directory);
        return {};
    }
    return directory;
}

std::optional<MergetoolIntake> readMergetoolIntake(const QString &directory, const QString &root,
                                                   const QByteArray &sessionKey, QString *error)
{
    const auto canonicalRoot = QFileInfo(root).canonicalFilePath();
    const auto canonical = QFileInfo(directory).canonicalFilePath();
    const auto prefix = canonicalRoot + QLatin1Char('/');
    if (sessionKey.size() != 64 || canonicalRoot.isEmpty()
        || !canonical.startsWith(prefix + QString::fromLatin1(sessionKey) + QLatin1Char('/'))) {
        fail(error, QStringLiteral("The mergetool intake is outside app-owned storage."));
        return std::nullopt;
    }
    const auto invocation = QFileInfo(canonical).fileName();
    if (QUuid::fromString(invocation).isNull()
        || QFileInfo(canonical).dir().dirName() != QString::fromLatin1(sessionKey)) {
        fail(error, QStringLiteral("The mergetool intake identity does not match this session."));
        return std::nullopt;
    }
    QByteArray manifestBytes;
    if (!readMergetoolFile(QFile::encodeName(canonical + QStringLiteral("/manifest.json")), &manifestBytes, error))
        return std::nullopt;
    const auto manifest = QJsonDocument::fromJson(manifestBytes).object();
    if (manifest.value(QStringLiteral("invocation")).toString() != invocation) {
        fail(error, QStringLiteral("The mergetool intake manifest is invalid."));
        return std::nullopt;
    }
    const auto path = QByteArray::fromHex(manifest.value(QStringLiteral("path")).toString().toLatin1());
    if (!mergetoolRepositoryPath(path)
        || QString::fromLatin1(path.toHex()) != manifest.value(QStringLiteral("path")).toString()) {
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
    if (intake.basePresent
        && !hashed(QStringLiteral("base"), manifest.value(QStringLiteral("base")).toString(), &intake.baseSha256))
        return std::nullopt;
    if (intake.localPresent
        && !hashed(QStringLiteral("local"), manifest.value(QStringLiteral("local")).toString(), &intake.localSha256))
        return std::nullopt;
    if (intake.remotePresent
        && !hashed(QStringLiteral("remote"), manifest.value(QStringLiteral("remote")).toString(), &intake.remoteSha256))
        return std::nullopt;
    return intake;
}

void removeMergetoolIntake(const QString &directory)
{
    if (directory.isEmpty()) return;
    QDir(directory).removeRecursively();
    // The session key's directory goes with its last intake.
    const auto parent = QFileInfo(directory).absolutePath();
    ::rmdir(QFile::encodeName(parent).constData());
}

int pruneMergetoolIntakes(const QString &root, const QByteArray &sessionKey)
{
    if (root.isEmpty() || sessionKey.size() != 64) return 0;
    const QDir key(root + QLatin1Char('/') + QString::fromLatin1(sessionKey));
    int removed = 0;
    const auto now = QDateTime::currentDateTime();
    for (const auto &entry : key.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks)) {
        QFile owner(entry.filePath() + QLatin1Char('/') + QLatin1String(kOwnerFile));
        if (owner.open(QIODevice::ReadOnly)) {
            if (launcherAlive(owner.read(32))) continue;
        } else if (entry.lastModified().secsTo(now) < kUnownedIntakeSeconds) {
            continue;
        }
        if (QDir(entry.filePath()).removeRecursively()) ++removed;
    }
    return removed;
}
}
