#include "DestinationWriter.h"

#include <QFile>
#include <QUuid>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace branch_truce::storage {
namespace {
class Descriptor {
public:
    explicit Descriptor(int value = -1) : fd(value) {}
    ~Descriptor() { if (fd >= 0) ::close(fd); }
    Descriptor(Descriptor &&other) noexcept : fd(std::exchange(other.fd, -1)) {}
    Descriptor &operator=(Descriptor &&other) noexcept
    {
        if (fd >= 0) ::close(fd);
        fd = std::exchange(other.fd, -1);
        return *this;
    }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
    int get() const { return fd; }
private:
    int fd;
};
struct Directory {
    QByteArray name;
    Descriptor descriptor;
    struct stat identity {};
};
struct Failure {
    DestinationFailure kind = DestinationFailure::None;
    QString diagnostic;
    explicit operator bool() const { return kind != DestinationFailure::None; }
};
Failure systemFailure(const char *operation)
{
    const int error = errno;
    const auto kind = error == EACCES || error == EPERM || error == EROFS
        ? DestinationFailure::PermissionDenied : DestinationFailure::IoError;
    return {kind, QString::fromLatin1(operation) + QStringLiteral(": ")
        + QString::fromLocal8Bit(std::strerror(error))};
}
bool sameInode(const struct stat &a, const struct stat &b)
{
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
bool sameMetadata(const struct stat &a, const struct stat &b, bool exchanged = false)
{
    return sameInode(a, b) && a.st_mode == b.st_mode && a.st_uid == b.st_uid
        && a.st_gid == b.st_gid && a.st_nlink == b.st_nlink && a.st_size == b.st_size
        && a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
        // renameat2 updates ctime. All other baseline fields and bytes still match.
        && (exchanged || (a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec));
}
bool relativePath(const QByteArray &path)
{
    if (path.isEmpty() || path.startsWith('/') || path.contains('\0')) return false;
    const auto parts = path.split('/');
    for (const auto &part : parts) {
        if (part.isEmpty() || part == "." || part == ".." || part == ".git") return false;
    }
    return true;
}
Failure openDirectory(int parent, const QByteArray &name, Directory &directory)
{
    struct stat before {};
    if (::fstatat(parent, name.constData(), &before, AT_SYMLINK_NOFOLLOW) != 0)
        return systemFailure("Cannot inspect destination directory");
    if (S_ISLNK(before.st_mode))
        return {DestinationFailure::Symlink, QStringLiteral("Destination directory is a symlink")};
    if (!S_ISDIR(before.st_mode))
        return {DestinationFailure::PathChanged, QStringLiteral("Destination ancestor is not a directory")};
    directory.name = name;
    directory.descriptor = Descriptor(::openat(parent, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (directory.descriptor.get() < 0) return systemFailure("Cannot open destination directory");
    if (::fstat(directory.descriptor.get(), &directory.identity) != 0)
        return systemFailure("Cannot inspect opened destination directory");
    if (!sameInode(before, directory.identity))
        return {DestinationFailure::PathChanged, QStringLiteral("Destination directory changed while opening")};
    return {};
}
Failure readStable(int fd, QByteArray &bytes, struct stat &identity)
{
    if (::fstat(fd, &identity) != 0) return systemFailure("Cannot inspect destination");
    if (!S_ISREG(identity.st_mode))
        return {DestinationFailure::NotRegularFile, QStringLiteral("Destination must be a regular file")};
    if (identity.st_size < 0 || static_cast<quint64>(identity.st_size)
        >= static_cast<quint64>(std::numeric_limits<qsizetype>::max()))
        return {DestinationFailure::IoError, QStringLiteral("Destination size cannot be represented")};
    bytes.resize(static_cast<qsizetype>(identity.st_size));
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const auto size = static_cast<size_t>(std::min(qsizetype{65536}, bytes.size() - offset));
        const auto count = ::pread(fd, bytes.data() + offset, size, static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return systemFailure("Cannot read destination");
        if (count == 0) return {DestinationFailure::ContentChanged, QStringLiteral("Destination shrank while reading")};
        offset += count;
    }
    char extra;
    ssize_t count;
    do { count = ::pread(fd, &extra, 1, identity.st_size); } while (count < 0 && errno == EINTR);
    if (count < 0) return systemFailure("Cannot finish reading destination");
    struct stat after {};
    if (::fstat(fd, &after) != 0) return systemFailure("Cannot reinspect destination");
    if (count != 0 || !sameMetadata(identity, after))
        return {DestinationFailure::ContentChanged, QStringLiteral("Destination changed while reading")};
    return {};
}
} // namespace

struct DestinationSnapshot::State {
    QString root;
    QByteArray path;
    QByteArray contents;
    std::vector<Directory> directories;
    Descriptor file;
    struct stat identity {};
};

namespace {
Failure checkDirectories(const DestinationSnapshot::State &state)
{
    Descriptor current(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (current.get() < 0) return systemFailure("Cannot reopen filesystem root");
    for (const auto &expected : state.directories) {
        Directory opened;
        if (auto failure = openDirectory(current.get(), expected.name, opened)) return failure;
        const auto &a = expected.identity;
        const auto &b = opened.identity;
        if (!sameInode(a, b) || a.st_mode != b.st_mode || a.st_uid != b.st_uid || a.st_gid != b.st_gid)
            return {DestinationFailure::PathChanged, QStringLiteral("Destination directory identity or permissions changed")};
        current = std::move(opened.descriptor);
    }
    return {};
}
Failure checkFile(const DestinationSnapshot::State &state, int parent, const QByteArray &name, bool exchanged = false)
{
    struct stat entry {};
    if (::fstatat(parent, name.constData(), &entry, AT_SYMLINK_NOFOLLOW) != 0)
        return systemFailure("Cannot reinspect destination entry");
    if (S_ISLNK(entry.st_mode))
        return {DestinationFailure::Symlink, QStringLiteral("Destination was replaced by a symlink")};
    if (!sameInode(entry, state.identity))
        return {DestinationFailure::PathChanged, QStringLiteral("Destination file identity changed")};
    QByteArray bytes;
    struct stat current {};
    if (auto failure = readStable(state.file.get(), bytes, current)) return failure;
    if (!sameMetadata(state.identity, current, exchanged) || bytes != state.contents)
        return {DestinationFailure::ContentChanged, QStringLiteral("Destination content or permissions changed")};
    return {};
}
Failure checkMetadata(int fd, const struct stat &identity)
{
    if (identity.st_uid != ::geteuid() || identity.st_nlink != 1 || (identity.st_mode & 07000) != 0)
        return {DestinationFailure::UnsupportedMetadata,
            QStringLiteral("Publication requires an owned file with one link and no special permission bits")};
    const auto attributes = ::flistxattr(fd, nullptr, 0);
    if (attributes < 0) return systemFailure("Cannot inspect destination extended attributes");
    if (attributes != 0)
        return {DestinationFailure::UnsupportedMetadata,
            QStringLiteral("Publication cannot preserve extended attributes or ACLs for this destination")};
    return {};
}
} // namespace

DestinationSnapshot::DestinationSnapshot(std::shared_ptr<const State> state) : d(std::move(state)) {}
const QString &DestinationSnapshot::worktreeRoot() const { return d->root; }
const QByteArray &DestinationSnapshot::path() const { return d->path; }
const QByteArray &DestinationSnapshot::bytes() const { return d->contents; }
SnapshotFileIdentity DestinationSnapshot::identity() const
{
    const auto &info = d->identity;
    return {static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino),
        static_cast<std::uint32_t>(info.st_mode), static_cast<std::int64_t>(info.st_size),
        static_cast<std::int64_t>(info.st_mtim.tv_sec), static_cast<std::int64_t>(info.st_mtim.tv_nsec),
        static_cast<std::int64_t>(info.st_ctim.tv_sec), static_cast<std::int64_t>(info.st_ctim.tv_nsec)};
}

DestinationCapture captureDestination(const QString &root, const QByteArray &path)
{
    auto state = std::make_shared<DestinationSnapshot::State>();
    state->root = root;
    state->path = path;
    const auto nativeRoot = QFile::encodeName(root);
    if (!root.startsWith('/') || root.contains(QChar::Null) || QFile::decodeName(nativeRoot) != root
        || (nativeRoot != "/" && !relativePath(nativeRoot.mid(1))) || !relativePath(path))
        return {{}, DestinationFailure::InvalidPath, QStringLiteral("Expected canonical worktree root and exact relative destination bytes")};
    // Start at '/' and open every component without following links. Keeping all
    // descriptors prevents inode reuse from disguising a replacement during editing.
    std::vector<QByteArray> components{"/"};
    if (nativeRoot != "/") {
        const auto rootComponents = nativeRoot.mid(1).split('/');
        for (const auto &part : rootComponents) components.push_back(part);
    }
    const auto relative = path.split('/');
    for (qsizetype i = 0; i + 1 < relative.size(); ++i) components.push_back(relative[i]);
    int parent = AT_FDCWD;
    for (const auto &component : components) {
        Directory directory;
        if (auto failure = openDirectory(parent, component, directory)) return {{}, failure.kind, failure.diagnostic};
        parent = directory.descriptor.get();
        state->directories.push_back(std::move(directory));
    }
    const auto name = relative.last();
    struct stat entry {};
    if (::fstatat(parent, name.constData(), &entry, AT_SYMLINK_NOFOLLOW) != 0) {
        const auto failure = systemFailure("Cannot inspect destination file");
        return {{}, failure.kind, failure.diagnostic};
    }
    if (S_ISLNK(entry.st_mode)) return {{}, DestinationFailure::Symlink, QStringLiteral("Destination is a symlink")};
    if (!S_ISREG(entry.st_mode)) return {{}, DestinationFailure::NotRegularFile, QStringLiteral("Destination must be a regular file")};
    state->file = Descriptor(::openat(parent, name.constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (state->file.get() < 0) {
        const auto failure = systemFailure("Cannot open destination file");
        return {{}, failure.kind, failure.diagnostic};
    }
    if (auto failure = readStable(state->file.get(), state->contents, state->identity)) return {{}, failure.kind, failure.diagnostic};
    if (!sameMetadata(entry, state->identity)) return {{}, DestinationFailure::PathChanged, QStringLiteral("Destination changed while opening")};
    if (auto failure = checkDirectories(*state)) return {{}, failure.kind, failure.diagnostic};
    if (auto failure = checkFile(*state, parent, name)) return {{}, failure.kind, failure.diagnostic};
    return {DestinationSnapshot(std::move(state)), DestinationFailure::None, {}};
}

PublicationResult publishDestination(const DestinationSnapshot &expected, const QByteArray &bytes,
    const QByteArray &mode, const std::function<bool()> &beginMutation)
{
    const auto &state = *expected.d;
    PublicationResult result;
    auto reject = [&result](const Failure &failure) {
        result.failure = failure.kind;
        result.diagnostic = failure.diagnostic;
        return result;
    };
    if (!beginMutation || (mode != "100644" && mode != "100755")
        || ((state.identity.st_mode & 0111) != 0) != (mode == "100755"))
        return reject({DestinationFailure::InvalidPath, QStringLiteral("Mutation admission and an agreed matching regular-file Git mode are required")});
    const int parent = state.directories.back().descriptor.get();
    const auto name = state.path.mid(state.path.lastIndexOf('/') + 1);
    if (auto failure = checkDirectories(state)) return reject(failure);
    if (auto failure = checkFile(state, parent, name)) return reject(failure);
    if (auto failure = checkMetadata(state.file.get(), state.identity)) return reject(failure);
    // Replacement must not bypass a read-only destination merely because its
    // directory is writable. O_WRONLY without O_TRUNC checks the actual access.
    Descriptor writable(::openat(parent, name.constData(), O_WRONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (writable.get() < 0) return reject(systemFailure("Destination is not writable"));
    struct stat accessIdentity {};
    if (::fstat(writable.get(), &accessIdentity) != 0) return reject(systemFailure("Cannot inspect writable destination"));
    if (!sameMetadata(state.identity, accessIdentity))
        return reject({DestinationFailure::PathChanged, QStringLiteral("Destination changed during permission check")});
    if (!beginMutation()) return reject({DestinationFailure::Cancelled, QStringLiteral("Publication cancelled before mutation")});

    const auto temporary = ".branch-truce-publish-" + QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
    if (::mkdirat(parent, temporary.constData(), 0700) != 0) return reject(systemFailure("Cannot create private publication directory"));
    Directory staging;
    if (auto failure = openDirectory(parent, temporary, staging)) {
        ::unlinkat(parent, temporary.constData(), AT_REMOVEDIR);
        return reject(failure);
    }
    const int stage = staging.descriptor.get();
    const auto parentPath = QFile::encodeName(state.root) + '/' + state.path.left(state.path.size() - name.size());
    const auto retained = parentPath + temporary + "/previous";
    Descriptor output(::openat(stage, "previous", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    auto cleanup = [&]() {
        struct stat linked {};
        if (::fstatat(parent, temporary.constData(), &linked, AT_SYMLINK_NOFOLLOW) != 0
            || !sameInode(linked, staging.identity)) return false;
        if (::fstatat(stage, "previous", &linked, AT_SYMLINK_NOFOLLOW) == 0) {
            struct stat owned {};
            const int owner = result.state == PublicationState::Unchanged ? output.get() : state.file.get();
            if (owner < 0 || ::fstat(owner, &owned) != 0 || !sameInode(linked, owned)) return false;
            if (::unlinkat(stage, "previous", 0) != 0) return false;
        } else if (errno != ENOENT) return false;
        return ::unlinkat(parent, temporary.constData(), AT_REMOVEDIR) == 0;
    };
    auto failBeforeExchange = [&](const Failure &failure) {
        if (!cleanup()) {
            result.retainedPath = retained;
            return reject({DestinationFailure::CleanupFailure, failure.diagnostic + QStringLiteral("; could not remove private publication files")});
        }
        return reject(failure);
    };
    if (output.get() < 0) return failBeforeExchange(systemFailure("Cannot create replacement file"));
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(output.get(), bytes.constData() + offset,
            static_cast<size_t>(std::min(qsizetype{65536}, bytes.size() - offset)));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return failBeforeExchange(systemFailure("Cannot write complete replacement"));
        if (count == 0) return failBeforeExchange({DestinationFailure::IoError, QStringLiteral("Replacement write made no progress")});
        offset += count;
    }
    if (::fchown(output.get(), state.identity.st_uid, state.identity.st_gid) != 0
        || ::fchmod(output.get(), state.identity.st_mode & 0777) != 0)
        return failBeforeExchange(systemFailure("Cannot preserve destination permissions"));
    QByteArray prepared;
    struct stat preparedIdentity {};
    if (auto failure = readStable(output.get(), prepared, preparedIdentity)) return failBeforeExchange(failure);
    if (prepared != bytes) return failBeforeExchange({DestinationFailure::ContentChanged, QStringLiteral("Replacement bytes changed before publication")});
    if (auto failure = checkMetadata(output.get(), preparedIdentity)) return failBeforeExchange(failure);
    if (::fsync(output.get()) != 0 || ::fsync(stage) != 0) return failBeforeExchange(systemFailure("Cannot sync replacement"));
    if (auto failure = checkDirectories(state)) return failBeforeExchange(failure);
    if (auto failure = checkFile(state, parent, name)) return failBeforeExchange(failure);
    if (auto failure = checkMetadata(state.file.get(), state.identity)) return failBeforeExchange(failure);

    // Unlike overwriting rename, exchange keeps the actual displaced entry for
    // inspection. There is no compare-and-rename syscall and no unsafe fallback.
    if (::renameat2(stage, "previous", parent, name.constData(), RENAME_EXCHANGE) != 0) {
        // A remote filesystem can lose the reply after performing a rename.
        // Preserve the exchange slot on ambiguous I/O errors; never remove what
        // could already be the displaced entry or claim that nothing changed.
        if (errno == EIO || errno == ESTALE || errno == ETIMEDOUT) {
            const auto failure = systemFailure("Atomic exchange outcome is uncertain");
            result.state = PublicationState::Uncertain;
            result.retainedPath = retained;
            return reject(failure);
        }
        if (errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP || errno == EXDEV)
            return failBeforeExchange({DestinationFailure::UnsupportedFilesystem, QStringLiteral("Filesystem does not support atomic destination exchange")});
        return failBeforeExchange(systemFailure("Cannot exchange destination"));
    }
    result.state = PublicationState::Uncertain;
    result.retainedPath = retained;
    if (auto failure = checkFile(state, stage, "previous", true)) return reject(failure);
    if (auto failure = checkMetadata(state.file.get(), state.identity)) return reject(failure);
    if (auto failure = checkDirectories(state)) return reject(failure);
    auto published = captureDestination(state.root, state.path);
    if (!published.destination) return reject({published.failure, published.diagnostic});
    const auto &current = *published.destination->d;
    if (!sameMetadata(preparedIdentity, current.identity, true) || current.contents != bytes)
        return reject({DestinationFailure::ContentChanged, QStringLiteral("Published destination identity or bytes could not be verified")});
    if (auto failure = checkDirectories(state)) return reject(failure);
    if (::fsync(parent) != 0 || ::fsync(stage) != 0) return reject(systemFailure("Cannot sync published destination"));
    if (auto failure = checkFile(state, stage, "previous", true)) return reject(failure);
    if (auto failure = checkFile(current, parent, name)) return reject(failure);
    if (auto failure = checkDirectories(state)) return reject(failure);
    result.state = PublicationState::Published;
    if (!cleanup()) return reject({DestinationFailure::CleanupFailure, QStringLiteral("Published result verified; private publication cleanup is incomplete")});
    result.retainedPath.clear();
    result.destination = std::move(published.destination);
    return result;
}
} // namespace branch_truce::storage
