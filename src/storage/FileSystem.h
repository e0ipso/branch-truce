// The file system calls storage makes whose failures its callers must survive.
#pragma once

#include <QString>
#include <cstddef>
#include <memory>
#include <sys/stat.h>
#include <sys/types.h>

namespace branch_truce::storage {

// Each method makes one system call and reports it the way that call does:
// -1 (or a negative count) with errno set. The defaults call the system, so a
// test overrides only the calls it fails or observes. Methods may run on any
// thread at once; an override must be safe for that.
class FileSystem {
public:
    virtual ~FileSystem() = default;
    [[nodiscard]] virtual ssize_t write(int fd, const void *bytes, std::size_t size) const;
    [[nodiscard]] virtual int sync(int fd) const;
    // fstat.
    [[nodiscard]] virtual int status(int fd, struct stat *result) const;
    // renameat2 with RENAME_EXCHANGE.
    [[nodiscard]] virtual int exchange(int fromDirectory, const char *from, int toDirectory, const char *to) const;
    [[nodiscard]] virtual int unlink(int directory, const char *name, int flags) const;
    [[nodiscard]] virtual uid_t effectiveUser() const;
    [[nodiscard]] virtual ssize_t listAttributes(int fd, char *names, std::size_t size) const;
    [[nodiscard]] virtual ssize_t attribute(int fd, const char *name, void *value, std::size_t size) const;
    [[nodiscard]] virtual int setAttribute(int fd, const char *name, const void *value, std::size_t size) const;
};

// The process's real file system.
[[nodiscard]] std::shared_ptr<const FileSystem> systemFileSystem();

// Opens the directory at path and syncs it, so an entry created or renamed in
// it survives a power loss once this returns true.
[[nodiscard]] bool syncDirectory(const FileSystem &files, const QString &path);
} // namespace branch_truce::storage
