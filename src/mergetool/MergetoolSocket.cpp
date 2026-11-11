// The mergetool socket names where a window listens for one worktree operation and checks who connects.
#include "mergetool/MergetoolSocket.h"
#include "storage/FileDescriptors.h"

#include <QCryptographicHash>
#include <QFile>
#include <QStandardPaths>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <fcntl.h>
#include <sys/file.h>
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

bool socketPathFits(const QByteArray &directory)
{
    return directory.size() + 1 + kSocketNameLength <= kSocketPathLimit;
}

// A real directory of this user's with no group or other access.
bool privateDirectory(const QByteArray &path)
{
    struct stat info{};
    return ::lstat(path.constData(), &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == ::geteuid()
        && (info.st_mode & 077U) == 0;
}

// The /tmp directory a link in this user's cache directory names, made with
// an unpredictable name when the link is missing or names anything else. A
// lock beside the link keeps two processes from making two.
QString fallbackDirectory()
{
    const auto cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (!cache.startsWith(QLatin1Char('/'))) return {};
    const auto home = QFile::encodeName(cache) + "/branch-truce";
    if ((::mkdir(QFile::encodeName(cache).constData(), 0700) != 0 && errno != EEXIST)
        || (::mkdir(home.constData(), 0700) != 0 && errno != EEXIST) || !privateDirectory(home))
        return {};
    const storage::Descriptor lock(
        ::open((home + "/mergetool-runtime.lock").constData(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!lock.valid() || ::flock(lock.get(), LOCK_EX) != 0) return {};
    const auto link = home + "/mergetool-runtime";
    QByteArray target(PATH_MAX, Qt::Uninitialized);
    const auto length = ::readlink(link.constData(), target.data(), static_cast<size_t>(target.size()));
    if (length > 0 && length < target.size()) {
        target.truncate(length);
        if (privateDirectory(target) && socketPathFits(target)) return QFile::decodeName(target);
    }
    if (::unlink(link.constData()) != 0 && errno != ENOENT) return {};
    auto made = "/tmp/branch-truce-" + QByteArray::number(::geteuid()) + "-XXXXXX";
    if (!::mkdtemp(made.data())) return {};
    if (::symlink(made.constData(), link.constData()) != 0) {
        ::rmdir(made.constData());
        return {};
    }
    return QFile::decodeName(made);
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
    return QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex();
}

QString mergetoolSocketDirectory(const QByteArray &runtimeDirectory)
{
    auto runtime = runtimeDirectory;
    if (runtime.startsWith('/')) {
        while (runtime.endsWith('/')) runtime.chop(1);
        const auto directory = runtime + "/branch-truce";
        if (socketPathFits(directory)) return QFile::decodeName(directory);
    }
    return fallbackDirectory();
}

QString mergetoolServerName(const QString &directory, const git::RepositoryContext &repository)
{
    const auto key = mergetoolSessionKey(repository);
    if (key.isEmpty() || directory.isEmpty()) return {};
    return directory + QLatin1Char('/') + QString::fromLatin1(kSocketPrefix)
        + QString::fromLatin1(key.left(kSocketKeyLength));
}

bool prepareMergetoolSocketDirectory(const QString &directory, Problem *error)
{
    const auto fail = [error, &directory](ProblemCode problem) {
        if (error) *error = {problem, {directory}};
        return false;
    };
    const auto native = QFile::encodeName(directory);
    if (native.isEmpty()) {
        if (error) *error = ProblemCode::MergetoolNoSocketDirectory;
        return false;
    }
    if (::mkdir(native.constData(), 0700) != 0 && errno != EEXIST) return fail(ProblemCode::MergetoolSocketCreate);
    struct stat info{};
    if (::lstat(native.constData(), &info) != 0 || !S_ISDIR(info.st_mode))
        return fail(ProblemCode::MergetoolSocketNotDirectory);
    if (info.st_uid != ::geteuid() || (info.st_mode & 077U) != 0) return fail(ProblemCode::MergetoolSocketNotPrivate);
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
}
