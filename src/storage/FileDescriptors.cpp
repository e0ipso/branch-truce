// File descriptors open paths without following links, write whole buffers and replace files atomically.
#include "storage/FileDescriptors.h"

#include <QByteArrayList>
#include <QUuid>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace branch_truce::storage {
void Descriptor::reset()
{
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

bool Descriptor::close()
{
    const int fd = std::exchange(fd_, -1);
    return fd < 0 || ::close(fd) == 0;
}

OpenedPath openWithoutLinks(const QByteArray &path, OpenTarget target)
{
    const auto parts = path.split('/');
    const auto meaningful = std::count_if(parts.cbegin(), parts.cend(),
                                          [](const QByteArray &part) { return !part.isEmpty() && part != "."; });
    if (path.isEmpty() || path.contains('\0') || meaningful == 0) return {Descriptor{}, OpenFailure::EmptyPath};
    if (parts.contains("..")) return {Descriptor{}, OpenFailure::ParentSegment};
    Descriptor current(::open(path.startsWith('/') ? "/" : ".", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (!current.valid()) return {Descriptor{}, OpenFailure::Unopenable};
    qsizetype seen = 0;
    for (const auto &part : parts) {
        if (part.isEmpty() || part == ".") continue;
        const bool last = ++seen == meaningful;
        const int access =
            last && target == OpenTarget::RegularFile ? (O_RDONLY | O_NONBLOCK) : (O_RDONLY | O_DIRECTORY);
        current = Descriptor(::openat(current.get(), part.constData(), O_CLOEXEC | O_NOFOLLOW | access));
        if (!current.valid()) return {Descriptor{}, OpenFailure::Unopenable};
    }
    struct stat info{};
    const bool directory = target == OpenTarget::Directory;
    if (::fstat(current.get(), &info) != 0 || (directory ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode)))
        return {Descriptor{}, OpenFailure::WrongType};
    return {std::move(current), OpenFailure::None};
}

bool writeAll(const FileSystem &files, int fd, const QByteArray &bytes)
{
    qsizetype written = 0;
    while (written < bytes.size()) {
        const auto count =
            files.write(fd, bytes.constData() + written, static_cast<std::size_t>(bytes.size() - written));
        if (count < 0 && errno == EINTR) continue;
        if (count == 0) errno = EIO;
        if (count <= 0) return false;
        written += count;
    }
    return true;
}

bool createSynced(const FileSystem &files, int directory, const char *name, const QByteArray &bytes, mode_t mode)
{
    Descriptor file(::openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode));
    if (!file.valid()) return false;
    const bool written = writeAll(files, file.get(), bytes) && files.sync(file.get()) == 0;
    if (file.close() && written) return true;
    (void)files.unlink(directory, name, 0);
    return false;
}

bool replaceFile(const FileSystem &files, int directory, const QByteArray &name, const QByteArray &bytes)
{
    struct stat original{};
    if (::fstatat(directory, name.constData(), &original, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(original.st_mode))
        return false;
    // A sibling, so the rename stays on one file system. Git's own temporaries
    // use other names, and the random part keeps two replacements apart.
    const auto temporary =
        "." + name + ".branch-truce-" + QUuid::createUuid().toString(QUuid::Id128).left(12).toLatin1();
    Descriptor file(
        ::openat(directory, temporary.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!file.valid()) return false;
    const bool written = writeAll(files, file.get(), bytes) && ::fchmod(file.get(), original.st_mode & 07777U) == 0
        && files.sync(file.get()) == 0;
    const bool renamed =
        file.close() && written && files.rename(directory, temporary.constData(), directory, name.constData()) == 0;
    if (!renamed) {
        (void)files.unlink(directory, temporary.constData(), 0);
        return false;
    }
    return files.sync(directory) == 0;
}
} // namespace branch_truce::storage
