// The file system calls storage makes whose failures its callers must survive.
#include "FileSystem.h"

#include <QFile>
#include <cstdio>
#include <fcntl.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace branch_truce::storage {
ssize_t FileSystem::write(int fd, const void *bytes, std::size_t size) const { return ::write(fd, bytes, size); }
int FileSystem::sync(int fd) const { return ::fsync(fd); }
int FileSystem::exchange(int fromDirectory, const char *from, int toDirectory, const char *to) const
{
    return ::renameat2(fromDirectory, from, toDirectory, to, RENAME_EXCHANGE);
}
int FileSystem::unlink(int directory, const char *name, int flags) const { return ::unlinkat(directory, name, flags); }
uid_t FileSystem::effectiveUser() const { return ::geteuid(); }
ssize_t FileSystem::listAttributes(int fd, char *names, std::size_t size) const
{
    return ::flistxattr(fd, names, size);
}
ssize_t FileSystem::attribute(int fd, const char *name, void *value, std::size_t size) const
{
    return ::fgetxattr(fd, name, value, size);
}
int FileSystem::setAttribute(int fd, const char *name, const void *value, std::size_t size) const
{
    return ::fsetxattr(fd, name, value, size, 0);
}

std::shared_ptr<const FileSystem> systemFileSystem()
{
    static const auto shared = std::make_shared<const FileSystem>();
    return shared;
}

bool syncDirectory(const FileSystem &files, const QString &path)
{
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    const bool synced = files.sync(fd) == 0;
    return ::close(fd) == 0 && synced;
}
} // namespace branch_truce::storage
