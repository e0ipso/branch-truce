#pragma once

// A storage file system that fails one sync with EIO or one publication exchange with EACCES.
// Give it to the store, journal or session under test; nothing else sees its faults.

#include "storage/FileSystem.h"

#include <atomic>
#include <cerrno>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace test_support {

class SyncFaults : public branch_truce::storage::FileSystem {
public:
    // The next failing operation: a recovery sync, a directory sync or publication exchange.
    enum class Point { None, PayloadSync, JournalSync, DirectorySync, PublicationExchange };

    // Arms one fault. Only one matching operation fails, even when two threads call at once.
    void arm(Point point)
    {
        failures = 0;
        next_ = point;
    }
    void disarm() { next_ = Point::None; }
    [[nodiscard]] std::vector<std::string> syncedDirectories() const
    {
        const std::lock_guard lock(mutex_);
        return directories_;
    }

    mutable std::atomic<int> failures = 0;

    int exchange(int fromDirectory, const char *from, int toDirectory, const char *to) const override
    {
        auto armed = Point::PublicationExchange;
        if (next_.compare_exchange_strong(armed, Point::None)) {
            ++failures;
            errno = EACCES;
            return -1;
        }
        return FileSystem::exchange(fromDirectory, from, toDirectory, to);
    }

    int sync(int descriptor) const override
    {
        const auto target = pathOf(descriptor);
        struct stat identity{};
        const bool directory = ::fstat(descriptor, &identity) == 0 && S_ISDIR(identity.st_mode);
        if (directory) {
            const std::lock_guard lock(mutex_);
            directories_.push_back(target);
        }
        auto armed = next_.load();
        const bool matches = armed == Point::DirectorySync ? directory
            : armed == Point::JournalSync                  ? std::string_view(target).ends_with("/journal")
            : armed == Point::PayloadSync ? !directory && target.find("/payloads/") != std::string::npos
                                          : false;
        if (matches && next_.compare_exchange_strong(armed, Point::None)) {
            ++failures;
            errno = EIO;
            return -1;
        }
        return FileSystem::sync(descriptor);
    }

private:
    static std::string pathOf(int descriptor)
    {
        char path[4096];
        const auto link = "/proc/self/fd/" + std::to_string(descriptor);
        const auto size = ::readlink(link.c_str(), path, sizeof(path));
        return size < 0 ? std::string{} : std::string(path, static_cast<std::size_t>(size));
    }

    mutable std::atomic<Point> next_ = Point::None;
    mutable std::mutex mutex_;
    mutable std::vector<std::string> directories_;
};

// Arms a fault for its lifetime and disarms it however the case leaves the scope.
class ArmedFault {
public:
    ArmedFault(SyncFaults &files, SyncFaults::Point point) : files_(files) { files_.arm(point); }
    ~ArmedFault() { files_.disarm(); }
    ArmedFault(const ArmedFault &) = delete;
    ArmedFault &operator=(const ArmedFault &) = delete;

private:
    SyncFaults &files_;
};

} // namespace test_support
