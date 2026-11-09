#pragma once

// Fails one fsync of a recovery payload or journal with EIO. An executable that includes this
// header links with --wrap=fsync; product targets use the real fsync.

#include <atomic>
#include <cerrno>
#include <string>
#include <string_view>
#include <unistd.h>

namespace recovery_faults {
enum class Point { None, PayloadSync, JournalSync };
// The armed fault and how many times it fired. Both are atomic because the sync under test may
// run on a worker thread while the case reads them, and exactly one matching sync on any thread
// takes the fault.
inline std::atomic<Point> next = Point::None;
inline std::atomic<int> failures = 0;
struct Failure {
    explicit Failure(Point point)
    {
        failures = 0;
        next = point;
    }
    ~Failure() { next = Point::None; }
    Failure(const Failure &) = delete;
    Failure &operator=(const Failure &) = delete;
};
}

extern "C" int __real_fsync(int descriptor);
extern "C" int __wrap_fsync(int descriptor)
{
    auto armed = recovery_faults::next.load();
    if (armed != recovery_faults::Point::None) {
        char path[4096];
        const auto link = "/proc/self/fd/" + std::to_string(descriptor);
        const auto size = ::readlink(link.c_str(), path, sizeof(path));
        if (size >= 0) {
            const std::string_view target(path, static_cast<std::size_t>(size));
            const bool matches = armed == recovery_faults::Point::JournalSync
                ? target.ends_with("/journal")
                : target.find("/payloads/") != std::string_view::npos;
            // Only the sync that disarms the fault fails, even when two threads match at once.
            if (matches && recovery_faults::next.compare_exchange_strong(armed, recovery_faults::Point::None)) {
                ++recovery_faults::failures;
                errno = EIO;
                return -1;
            }
        }
    }
    return __real_fsync(descriptor);
}
