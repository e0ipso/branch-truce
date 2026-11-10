// The mergetool intake keeps a waiting launcher's input copies where its window can read and check them.
#include "mergetool/MergetoolIntake.h"
#include "mergetool/MergetoolFiles.h"
#include "storage/FileDescriptors.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

namespace branch_truce {
namespace {
// The file naming the launcher that owns an intake, written before its copies.
constexpr char kOwnerFile[] = "launcher";
// An intake with no owner file yet is still being written, unless it is older than this.
constexpr qint64 kUnownedIntakeSeconds = 60;

QString fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return {};
}

QByteArray sha256(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
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

// When process pid started, in clock ticks after boot, as /proc/<pid>/stat
// reports it. Empty when the process is gone.
QByteArray processStartTime(qint64 pid)
{
    QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!stat.open(QIODevice::ReadOnly)) return {};
    const auto bytes = stat.read(4096);
    // The command name in parentheses can hold spaces; the fields after it cannot.
    const auto end = bytes.lastIndexOf(')');
    if (end < 0) return {};
    // Field 22 is the start time; the fields after the name start at field 3.
    const auto fields = bytes.mid(end + 2).split(' ');
    return fields.size() > 19 ? fields[19] : QByteArray{};
}

QByteArray ownerRecord(qint64 pid) { return QByteArray::number(pid) + ' ' + processStartTime(pid) + '\n'; }

// A PID alone can name a later process, so the start time must match as well.
bool launcherAlive(const QByteArray &owner)
{
    const auto fields = owner.trimmed().split(' ');
    bool number = false;
    const auto pid = fields.front().toLongLong(&number);
    if (!number || pid <= 0 || fields.size() != 2 || fields[1].isEmpty()) return false;
    return processStartTime(pid) == fields[1];
}
}

QString mergetoolIntakeRoot(const QString &socketDirectory)
{
    return socketDirectory.isEmpty() ? QString{} : socketDirectory + QStringLiteral("/intake");
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
    const auto opened = storage::openWithoutLinks(QFile::encodeName(directory), storage::OpenTarget::Directory);
    if (!opened.descriptor.valid()) return fail(error, QStringLiteral("The mergetool intake could not be created."));
    const auto &files = *storage::systemFileSystem();
    const auto store = [&](const char *name, const QByteArray &bytes) {
        return storage::createSynced(files, opened.descriptor.get(), name, bytes);
    };
    bool stored = store(kOwnerFile, ownerRecord(static_cast<qint64>(::getpid())));
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
    if (stored) stored = files.sync(opened.descriptor.get()) == 0;
    if (!stored) {
        removeMergetoolIntake(directory);
        return fail(error, QStringLiteral("The mergetool intake could not be written."));
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
    const auto hashed = [&](const QString &name, QByteArray *digest) {
        QByteArray bytes;
        if (!readMergetoolFile(QFile::encodeName(canonical + QLatin1Char('/') + name), &bytes, error)) return false;
        *digest = sha256(bytes);
        if (QString::fromLatin1(*digest) != manifest.value(name).toString()) {
            fail(error, QStringLiteral("The mergetool intake bytes do not match its manifest."));
            return false;
        }
        return true;
    };
    MergetoolIntake intake;
    intake.invocation = invocation;
    intake.path = path;
    if (!hashed(QStringLiteral("merged"), &intake.mergedSha256)) return std::nullopt;
    intake.basePresent = manifest.value(QStringLiteral("basePresent")).toBool();
    intake.localPresent = manifest.value(QStringLiteral("localPresent")).toBool();
    intake.remotePresent = manifest.value(QStringLiteral("remotePresent")).toBool();
    if (intake.basePresent && !hashed(QStringLiteral("base"), &intake.baseSha256)) return std::nullopt;
    if (intake.localPresent && !hashed(QStringLiteral("local"), &intake.localSha256)) return std::nullopt;
    if (intake.remotePresent && !hashed(QStringLiteral("remote"), &intake.remoteSha256)) return std::nullopt;
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
            if (launcherAlive(owner.read(64))) continue;
        } else if (entry.lastModified().secsTo(now) < kUnownedIntakeSeconds) {
            continue;
        }
        if (QDir(entry.filePath()).removeRecursively()) ++removed;
    }
    return removed;
}
}
