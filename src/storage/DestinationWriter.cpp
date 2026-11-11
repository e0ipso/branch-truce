// Destination writing preserves file identity and metadata while safely publishing reviewed result bytes.
#include "DestinationWriter.h"
#include "storage/FileDescriptors.h"

#include <QFile>
#include <QUuid>
#include <QtGlobal>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <optional>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace branch_truce::storage {
namespace {
struct Directory {
    QByteArray name;
    Descriptor descriptor;
    struct stat identity{};
};
// A directory the snapshot passed through: its name in its parent and what it was.
struct DirectoryIdentity {
    QByteArray name;
    struct stat identity{};
};
struct Failure {
    DestinationFailure kind = DestinationFailure::None;
    Problem problem;
    explicit operator bool() const { return kind != DestinationFailure::None; }
};
bool sameInode(const struct stat &a, const struct stat &b) { return a.st_dev == b.st_dev && a.st_ino == b.st_ino; }
struct PublicationSlot {
    const FileSystem &files;
    int parent;
    const QByteArray &temporary;
    const Directory &staging;
    const Descriptor &output, &original;
    const PublicationResult &result;

    bool cleanup() const
    {
        const int stage = staging.descriptor.get();
        struct stat linked{};
        if (files.statusAt(parent, temporary.constData(), &linked, AT_SYMLINK_NOFOLLOW) != 0
            || !sameInode(linked, staging.identity))
            return false;
        if (files.statusAt(stage, "previous", &linked, AT_SYMLINK_NOFOLLOW) == 0) {
            struct stat owned{};
            const int owner = result.state == PublicationState::Unchanged ? output.get() : original.get();
            if (owner < 0 || files.status(owner, &owned) != 0 || !sameInode(linked, owned)) return false;
            if (files.unlink(stage, "previous", 0) != 0) return false;
        } else if (errno != ENOENT)
            return false;
        return files.unlink(parent, temporary.constData(), AT_REMOVEDIR) == 0;
    }
};
// call names the system call that failed; the problem carries the system's error text.
Failure systemFailure(ProblemCode call)
{
    const int error = errno;
    const auto kind = error == EACCES || error == EPERM || error == EROFS ? DestinationFailure::PermissionDenied
                                                                          : DestinationFailure::IoError;
    return {kind, Problem{call, {qt_error_string(error)}}};
}
bool sameMetadata(const struct stat &a, const struct stat &b, bool exchanged = false)
{
    return sameInode(a, b) && a.st_mode == b.st_mode && a.st_uid == b.st_uid && a.st_gid == b.st_gid
        && a.st_nlink == b.st_nlink && a.st_size == b.st_size && a.st_mtim.tv_sec == b.st_mtim.tv_sec
        && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
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
Failure openDirectory(const FileSystem &files, int parent, const QByteArray &name, Directory &directory)
{
    struct stat before{};
    if (files.statusAt(parent, name.constData(), &before, AT_SYMLINK_NOFOLLOW) != 0)
        return systemFailure(ProblemCode::SystemInspectDirectory);
    if (S_ISLNK(before.st_mode)) return {DestinationFailure::Symlink, ProblemCode::DestinationDirectorySymlink};
    if (!S_ISDIR(before.st_mode))
        return {DestinationFailure::PathChanged, ProblemCode::DestinationAncestorNotDirectory};
    directory.name = name;
    directory.descriptor =
        Descriptor(files.openAt(parent, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (directory.descriptor.get() < 0) return systemFailure(ProblemCode::SystemOpenDirectory);
    if (files.status(directory.descriptor.get(), &directory.identity) != 0)
        return systemFailure(ProblemCode::SystemInspectOpenedDirectory);
    if (!sameInode(before, directory.identity))
        return {DestinationFailure::PathChanged, ProblemCode::DestinationDirectoryChangedWhileOpening};
    return {};
}
Failure readStable(const FileSystem &files, int fd, QByteArray &bytes, struct stat &identity,
                   const struct stat *expected = nullptr, bool exchanged = false)
{
    if (files.status(fd, &identity) != 0) return systemFailure(ProblemCode::SystemInspectDestination);
    if (!S_ISREG(identity.st_mode)) return {DestinationFailure::NotRegularFile, ProblemCode::DestinationNotRegular};
    if (expected && !sameInode(*expected, identity))
        return {DestinationFailure::PathChanged, ProblemCode::DestinationIdentityChanged};
    if (expected && !sameMetadata(*expected, identity, exchanged))
        return {DestinationFailure::ContentChanged, ProblemCode::DestinationContentChanged};
    if (identity.st_size < 0 || static_cast<quint64>(identity.st_size) > kMaxDestinationBytes)
        return {DestinationFailure::ReadLimit,
                Problem{ProblemCode::ClassificationResultUnreadable, {QString::number(kMaxDestinationBytes)}}};
    bytes.resize(static_cast<qsizetype>(identity.st_size));
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const auto size = static_cast<size_t>(std::min(qsizetype{65536}, bytes.size() - offset));
        const auto count = files.readAt(fd, bytes.data() + offset, size, static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return systemFailure(ProblemCode::SystemReadDestination);
        if (count == 0) return {DestinationFailure::ContentChanged, ProblemCode::DestinationShrank};
        offset += count;
    }
    char extra;
    ssize_t count;
    do {
        count = files.readAt(fd, &extra, 1, identity.st_size);
    } while (count < 0 && errno == EINTR);
    if (count < 0) return systemFailure(ProblemCode::SystemFinishReading);
    struct stat after{};
    if (files.status(fd, &after) != 0) return systemFailure(ProblemCode::SystemReinspectDestination);
    if (count != 0 || !sameMetadata(identity, after))
        return {DestinationFailure::ContentChanged, ProblemCode::DestinationChangedWhileReading};
    return {};
}
} // namespace

struct DestinationSnapshot::State {
    QString root;
    QByteArray path;
    QByteArray contents;
    // From '/' down to the file's parent.
    std::vector<DirectoryIdentity> directories;
    struct stat identity{};
    std::shared_ptr<const FileSystem> files;
};

namespace {
// Opens the snapshot's directories again from '/' and returns the file's
// parent in parent. Every directory must still be the one the snapshot saw,
// with the same permissions and owner.
Failure openParent(const DestinationSnapshot::State &state, Descriptor &parent)
{
    const auto &files = *state.files;
    Descriptor current(files.openAt(AT_FDCWD, "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (current.get() < 0) return systemFailure(ProblemCode::SystemReopenRoot);
    for (const auto &expected : state.directories) {
        Directory opened;
        if (auto failure = openDirectory(files, current.get(), expected.name, opened)) return failure;
        const auto &a = expected.identity;
        const auto &b = opened.identity;
        if (!sameInode(a, b) || a.st_mode != b.st_mode || a.st_uid != b.st_uid || a.st_gid != b.st_gid)
            return {DestinationFailure::PathChanged, ProblemCode::DestinationDirectoryChanged};
        current = std::move(opened.descriptor);
    }
    parent = std::move(current);
    return {};
}
Failure checkDirectories(const DestinationSnapshot::State &state)
{
    Descriptor parent;
    return openParent(state, parent);
}
// Opens name in parent without following a link. The entry must be the
// snapshot's file, with its bytes and metadata.
Failure checkFile(const DestinationSnapshot::State &state, int parent, const QByteArray &name, bool exchanged = false)
{
    const auto &files = *state.files;
    struct stat entry{};
    if (files.statusAt(parent, name.constData(), &entry, AT_SYMLINK_NOFOLLOW) != 0)
        return systemFailure(ProblemCode::SystemReinspectEntry);
    if (S_ISLNK(entry.st_mode)) return {DestinationFailure::Symlink, ProblemCode::DestinationReplacedBySymlink};
    if (!sameInode(entry, state.identity))
        return {DestinationFailure::PathChanged, ProblemCode::DestinationIdentityChanged};
    Descriptor file(files.openAt(parent, name.constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (file.get() < 0) return systemFailure(ProblemCode::SystemReopenEntry);
    QByteArray bytes;
    struct stat current{};
    if (auto failure = readStable(files, file.get(), bytes, current, &state.identity, exchanged)) return failure;
    if (!sameInode(entry, current)) return {DestinationFailure::PathChanged, ProblemCode::DestinationIdentityChanged};
    if (bytes != state.contents) return {DestinationFailure::ContentChanged, ProblemCode::DestinationContentChanged};
    return {};
}
// The only extended attribute publication preserves: the SELinux label, which
// every file carries on a system with SELinux enabled.
constexpr char selinuxLabel[] = "security.selinux";

Failure checkMetadata(const FileSystem &files, int fd, const struct stat &identity)
{
    if (identity.st_uid != files.effectiveUser() || identity.st_nlink != 1 || (identity.st_mode & 07000U) != 0)
        return {DestinationFailure::UnsupportedMetadata, ProblemCode::PublicationOwnership};
    QByteArray names;
    for (;;) {
        const auto size = files.listAttributes(fd, nullptr, 0);
        if (size < 0) return systemFailure(ProblemCode::SystemInspectAttributes);
        names.resize(static_cast<qsizetype>(size));
        const auto listed = files.listAttributes(fd, names.data(), static_cast<std::size_t>(names.size()));
        if (listed >= 0) {
            names.truncate(static_cast<qsizetype>(listed));
            break;
        }
        // The list grew between the two calls.
        if (errno != ERANGE) return systemFailure(ProblemCode::SystemInspectAttributes);
    }
    for (const auto &name : names.split('\0')) {
        if (!name.isEmpty() && name != selinuxLabel)
            return {DestinationFailure::UnsupportedMetadata, ProblemCode::PublicationExtendedAttributes};
    }
    return {};
}

// The SELinux label of fd: empty when it has none, nullopt when it cannot be read.
std::optional<QByteArray> label(const FileSystem &files, int fd)
{
    for (;;) {
        const auto size = files.attribute(fd, selinuxLabel, nullptr, 0);
        if (size < 0) {
            if (errno == ENODATA || errno == ENOTSUP) return QByteArray{};
            return std::nullopt;
        }
        QByteArray value(static_cast<qsizetype>(size), Qt::Uninitialized);
        const auto read = files.attribute(fd, selinuxLabel, value.data(), static_cast<std::size_t>(value.size()));
        if (read >= 0) {
            value.truncate(static_cast<qsizetype>(read));
            return value;
        }
        if (errno != ERANGE) return std::nullopt;
    }
}

// Gives the replacement the destination's SELinux label, which the new file
// does not inherit when policy labels the publication directory differently.
Failure preserveLabel(const FileSystem &files, int destination, int replacement)
{
    const auto wanted = label(files, destination);
    const auto current = label(files, replacement);
    if (!wanted || !current) return systemFailure(ProblemCode::SystemReadLabel);
    if (wanted->isEmpty() || *wanted == *current) return {};
    if (files.setAttribute(replacement, selinuxLabel, wanted->constData(), static_cast<std::size_t>(wanted->size()))
            != 0
        || label(files, replacement) != wanted)
        return {DestinationFailure::UnsupportedMetadata, ProblemCode::PublicationSelinuxLabel};
    return {};
}
Failure reopenDestination(const DestinationSnapshot::State &state, int parent, const QByteArray &name,
                          Descriptor &original, Descriptor &writable)
{
    const auto &files = *state.files;
    if (auto failure = checkFile(state, parent, name)) return failure;
    original = Descriptor(files.openAt(parent, name.constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    struct stat originalIdentity{};
    if (original.get() < 0 || files.status(original.get(), &originalIdentity) != 0)
        return systemFailure(ProblemCode::SystemReopenDestination);
    if (!sameMetadata(state.identity, originalIdentity))
        return {DestinationFailure::PathChanged, ProblemCode::DestinationChangedWhileReopening};
    if (auto failure = checkMetadata(files, original.get(), state.identity)) return failure;
    // Replacement must not bypass a read-only destination merely because its
    // directory is writable. O_WRONLY without O_TRUNC checks the actual access.
    writable = Descriptor(files.openAt(parent, name.constData(), O_WRONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (writable.get() < 0) return systemFailure(ProblemCode::SystemNotWritable);
    struct stat accessIdentity{};
    if (files.status(writable.get(), &accessIdentity) != 0) return systemFailure(ProblemCode::SystemInspectWritable);
    if (!sameMetadata(state.identity, accessIdentity))
        return {DestinationFailure::PathChanged, ProblemCode::DestinationChangedDuringPermissionCheck};
    return {};
}
Failure prepareReplacement(const DestinationSnapshot::State &state, int original, int output, const QByteArray &bytes,
                           int stage, struct stat &preparedIdentity)
{
    const auto &files = *state.files;
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const auto count = files.write(output, bytes.constData() + offset,
                                       static_cast<size_t>(std::min(qsizetype{65536}, bytes.size() - offset)));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return systemFailure(ProblemCode::SystemWriteReplacement);
        if (count == 0) return {DestinationFailure::IoError, ProblemCode::PublicationNoProgress};
        offset += count;
    }
    if (::fchown(output, state.identity.st_uid, state.identity.st_gid) != 0
        || ::fchmod(output, state.identity.st_mode & 0777U) != 0)
        return systemFailure(ProblemCode::SystemPreservePermissions);
    if (auto failure = preserveLabel(files, original, output)) return failure;
    QByteArray prepared;
    if (auto failure = readStable(files, output, prepared, preparedIdentity)) return failure;
    if (prepared != bytes) return {DestinationFailure::ContentChanged, ProblemCode::PublicationReplacementChanged};
    if (auto failure = checkMetadata(files, output, preparedIdentity)) return failure;
    if (files.sync(output) != 0 || files.sync(stage) != 0) return systemFailure(ProblemCode::SystemSyncReplacement);
    return {};
}
} // namespace

DestinationSnapshot::DestinationSnapshot(std::shared_ptr<const State> state) : d(std::move(state)) {}
const QString &DestinationSnapshot::worktreeRoot() const { return d->root; }
const QByteArray &DestinationSnapshot::path() const { return d->path; }
const QByteArray &DestinationSnapshot::bytes() const { return d->contents; }
namespace {
SnapshotFileIdentity fileIdentity(const struct stat &info)
{
    return {static_cast<std::uint64_t>(info.st_dev),        static_cast<std::uint64_t>(info.st_ino),
            static_cast<std::uint32_t>(info.st_mode),       static_cast<std::int64_t>(info.st_size),
            static_cast<std::int64_t>(info.st_mtim.tv_sec), static_cast<std::int64_t>(info.st_mtim.tv_nsec),
            static_cast<std::int64_t>(info.st_ctim.tv_sec), static_cast<std::int64_t>(info.st_ctim.tv_nsec)};
}

} // namespace
SnapshotFileIdentity DestinationSnapshot::identity() const { return fileIdentity(d->identity); }

DestinationCapture captureDestination(const QString &root, const QByteArray &path,
                                      std::shared_ptr<const FileSystem> fileSystem,
                                      const SnapshotFileIdentity *expected)
{
    auto state = std::make_shared<DestinationSnapshot::State>();
    state->root = root;
    state->path = path;
    state->files = std::move(fileSystem);
    const auto nativeRoot = QFile::encodeName(root);
    if (!state->files || !root.startsWith('/') || root.contains(QChar::Null) || QFile::decodeName(nativeRoot) != root
        || (nativeRoot != "/" && !relativePath(nativeRoot.mid(1))) || !relativePath(path))
        return {{}, DestinationFailure::InvalidPath, ProblemCode::DestinationInvalidPath};
    // Start at '/' and open every component without following links. Only the
    // identities are kept; publication reopens the path and compares them.
    std::vector<QByteArray> components{"/"};
    if (nativeRoot != "/") {
        const auto rootComponents = nativeRoot.mid(1).split('/');
        for (const auto &part : rootComponents) components.push_back(part);
    }
    const auto relative = path.split('/');
    for (qsizetype i = 0; i + 1 < relative.size(); ++i) components.push_back(relative[i]);
    Descriptor parent(AT_FDCWD);
    for (const auto &component : components) {
        Directory directory;
        if (auto failure = openDirectory(*state->files, parent.get(), component, directory))
            return {{}, failure.kind, failure.problem};
        state->directories.push_back({directory.name, directory.identity});
        parent = std::move(directory.descriptor);
    }
    const auto &name = relative.last();
    const auto &files = *state->files;
    struct stat entry{};
    if (files.statusAt(parent.get(), name.constData(), &entry, AT_SYMLINK_NOFOLLOW) != 0) {
        const auto failure = systemFailure(ProblemCode::SystemInspectFile);
        return {{}, failure.kind, failure.problem};
    }
    if (S_ISLNK(entry.st_mode)) return {{}, DestinationFailure::Symlink, ProblemCode::DestinationSymlink};
    if (!S_ISREG(entry.st_mode)) return {{}, DestinationFailure::NotRegularFile, ProblemCode::DestinationNotRegular};
    if (expected && fileIdentity(entry) != *expected)
        return {{}, DestinationFailure::ContentChanged, ProblemCode::DestinationContentChanged};
    const Descriptor file(files.openAt(parent.get(), name.constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (file.get() < 0) {
        const auto failure = systemFailure(ProblemCode::SystemOpenFile);
        return {{}, failure.kind, failure.problem};
    }
    if (auto failure = readStable(files, file.get(), state->contents, state->identity, &entry))
        return {{}, failure.kind, failure.problem};
    if (auto failure = checkDirectories(*state)) return {{}, failure.kind, failure.problem};
    if (auto failure = checkFile(*state, parent.get(), name)) return {{}, failure.kind, failure.problem};
    return {DestinationSnapshot(std::move(state)), DestinationFailure::None, {}};
}

PublicationResult publishDestination(const DestinationSnapshot &expected, const QByteArray &bytes,
                                     const QByteArray &mode, const std::function<bool()> &beginMutation)
{
    const auto &state = *expected.d;
    const auto &files = *state.files;
    PublicationResult result;
    auto reject = [&result](const Failure &failure) {
        result.failure = failure.kind;
        result.problem = failure.problem;
        return result;
    };
    if (!beginMutation || (mode != "100644" && mode != "100755")
        || ((state.identity.st_mode & 0111U) != 0) != (mode == "100755"))
        return reject({DestinationFailure::InvalidRequest, ProblemCode::PublicationInvalidRequest});
    // These descriptors live only for this call. The parent and the original
    // file stay open across the exchange, so a later rename of an ancestor or
    // of the file cannot redirect what this call checks or retains.
    Descriptor parentDirectory;
    if (auto failure = openParent(state, parentDirectory)) return reject(failure);
    const int parent = parentDirectory.get();
    const auto name = state.path.mid(state.path.lastIndexOf('/') + 1);
    Descriptor original, writable;
    if (auto failure = reopenDestination(state, parent, name, original, writable)) return reject(failure);
    if (!beginMutation()) return reject({DestinationFailure::Cancelled, ProblemCode::PublicationCancelled});

    const auto temporary = ".branch-truce-publish-" + QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
    if (::mkdirat(parent, temporary.constData(), 0700) != 0)
        return reject(systemFailure(ProblemCode::SystemCreatePrivateDirectory));
    Directory staging;
    if (auto failure = openDirectory(files, parent, temporary, staging)) {
        (void)files.unlink(parent, temporary.constData(), AT_REMOVEDIR);
        return reject(failure);
    }
    const int stage = staging.descriptor.get();
    const auto parentPath = QFile::encodeName(state.root) + '/' + state.path.left(state.path.size() - name.size());
    const auto retained = parentPath + temporary + "/previous";
    Descriptor output(files.openAt(stage, "previous", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    const PublicationSlot slot{files, parent, temporary, staging, output, original, result};
    auto failBeforeExchange = [&](const Failure &failure) {
        if (!slot.cleanup()) {
            result.retainedPath = retained;
            return reject({DestinationFailure::CleanupFailure,
                           Problem{ProblemCode::PublicationCleanupFailed}.wrapping(failure.problem)});
        }
        return reject(failure);
    };
    if (output.get() < 0) return failBeforeExchange(systemFailure(ProblemCode::SystemCreateReplacement));
    struct stat preparedIdentity{};
    if (auto failure = prepareReplacement(state, original.get(), output.get(), bytes, stage, preparedIdentity))
        return failBeforeExchange(failure);
    if (auto failure = checkDirectories(state)) return failBeforeExchange(failure);
    if (auto failure = checkFile(state, parent, name)) return failBeforeExchange(failure);
    if (auto failure = checkMetadata(files, original.get(), state.identity)) return failBeforeExchange(failure);

    // Unlike overwriting rename, exchange keeps the actual displaced entry for
    // inspection. There is no compare-and-rename syscall and no unsafe fallback.
    if (files.exchange(stage, "previous", parent, name.constData()) != 0) {
        // A remote filesystem can lose the reply after performing a rename.
        // Preserve the exchange slot on ambiguous I/O errors; never remove what
        // could already be the displaced entry or claim that nothing changed.
        if (errno == EIO || errno == ESTALE || errno == ETIMEDOUT) {
            const auto failure = systemFailure(ProblemCode::SystemExchangeUncertain);
            result.state = PublicationState::Uncertain;
            result.retainedPath = retained;
            return reject(failure);
        }
        if (errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP || errno == EXDEV)
            return failBeforeExchange(
                {DestinationFailure::UnsupportedFilesystem, ProblemCode::PublicationUnsupportedFilesystem});
        return failBeforeExchange(systemFailure(ProblemCode::SystemExchange));
    }
    result.state = PublicationState::Uncertain;
    result.retainedPath = retained;
    if (auto failure = checkFile(state, stage, "previous", true)) return reject(failure);
    if (auto failure = checkMetadata(files, original.get(), state.identity)) return reject(failure);
    if (auto failure = checkDirectories(state)) return reject(failure);
    auto published = captureDestination(state.root, state.path, state.files);
    if (!published.destination) return reject({published.failure, published.problem});
    const auto &current = *published.destination->d;
    if (!sameMetadata(preparedIdentity, current.identity, true) || current.contents != bytes)
        return reject({DestinationFailure::ContentChanged, ProblemCode::PublicationUnverified});
    if (auto failure = checkDirectories(state)) return reject(failure);
    if (files.sync(parent) != 0 || files.sync(stage) != 0)
        return reject(systemFailure(ProblemCode::SystemSyncPublished));
    if (auto failure = checkFile(state, stage, "previous", true)) return reject(failure);
    if (auto failure = checkFile(current, parent, name)) return reject(failure);
    if (auto failure = checkDirectories(state)) return reject(failure);
    result.state = PublicationState::Published;
    if (!slot.cleanup()) return reject({DestinationFailure::CleanupFailure, ProblemCode::PublicationCleanupIncomplete});
    result.retainedPath.clear();
    result.destination = std::move(published.destination);
    return result;
}
} // namespace branch_truce::storage
