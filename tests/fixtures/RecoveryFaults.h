#pragma once

#include <cerrno>
#include <string>
#include <string_view>
#include <unistd.h>

namespace recovery_faults {
enum class Point { None, PayloadSync, JournalSync };
inline Point next = Point::None;
inline int failures = 0;
struct Failure {
    explicit Failure(Point point) { next = point; failures = 0; }
    ~Failure() { next = Point::None; }
};
}

// This header is compiled once per test executable; product targets use real fsync.
extern "C" int __real_fsync(int descriptor);
extern "C" int __wrap_fsync(int descriptor)
{
    if (recovery_faults::next != recovery_faults::Point::None) {
        char path[4096];
        const auto link = "/proc/self/fd/" + std::to_string(descriptor);
        const auto size = ::readlink(link.c_str(), path, sizeof(path));
        if (size >= 0) {
            const std::string_view target(path, static_cast<std::size_t>(size));
            const bool matches = recovery_faults::next == recovery_faults::Point::JournalSync
                ? target.ends_with("/journal") : target.find("/payloads/") != std::string_view::npos;
            if (matches) {
                recovery_faults::next = recovery_faults::Point::None;
                ++recovery_faults::failures;
                errno = EIO;
                return -1;
            }
        }
    }
    return __real_fsync(descriptor);
}
