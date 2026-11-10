// File descriptors open paths without following links, write whole buffers and replace files atomically.
#pragma once

#include "storage/FileSystem.h"
#include <QByteArray>
#include <sys/types.h>
#include <utility>

namespace branch_truce::storage {

// Owns one file descriptor and closes it when it goes.
class Descriptor {
public:
    explicit Descriptor(int value = -1) : fd_(value) {}
    ~Descriptor() { reset(); }
    Descriptor(Descriptor &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Descriptor &operator=(Descriptor &&other) noexcept
    {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
    [[nodiscard]] int get() const { return fd_; }
    [[nodiscard]] bool valid() const { return fd_ >= 0; }
    // Closes the descriptor and reports whether close succeeded.
    bool close();

private:
    void reset();
    int fd_;
};

enum class OpenTarget { RegularFile, Directory };
// Why a path could not be opened without following links.
enum class OpenFailure { None, EmptyPath, ParentSegment, Unopenable, WrongType };
struct OpenedPath {
    Descriptor descriptor;
    OpenFailure failure = OpenFailure::None;
};
// Opens path one component at a time, from "/" when it is absolute and from
// the working directory otherwise, refusing every symbolic link and every ".."
// segment. A regular file opens read-only and non-blocking, so a FIFO cannot
// stall it; the result must have the target's type.
[[nodiscard]] OpenedPath openWithoutLinks(const QByteArray &path, OpenTarget target);

// Writes all of bytes through files, retrying interrupted and partial writes.
// False when a write fails or makes no progress; errno says why.
[[nodiscard]] bool writeAll(const FileSystem &files, int fd, const QByteArray &bytes);

// Creates name in directory exclusively with mode, writes bytes and syncs
// them. False leaves no file behind unless removing it failed.
[[nodiscard]] bool createSynced(const FileSystem &files, int directory, const char *name, const QByteArray &bytes,
                                mode_t mode = 0600);

// Replaces the regular file name in directory with bytes: they go to a new
// sibling with the file's permission bits, which is synced and renamed over
// name before the directory is synced. A failure before the rename leaves the
// file as it was, so a crash never leaves a partial file.
[[nodiscard]] bool replaceFile(const FileSystem &files, int directory, const QByteArray &name, const QByteArray &bytes);
} // namespace branch_truce::storage
