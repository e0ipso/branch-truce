// Owner-thread reservations survive invalidation until the admitted work terminates.
#pragma once
#include <cstddef>

namespace branch_truce::detail {
struct WorkAdmission {
    std::size_t maxCount = 2, maxBytes = 268435456;
    std::size_t count = 0, bytes = 0;
    bool take(std::size_t retained) noexcept
    {
        if (count >= maxCount || retained > maxBytes - bytes) return false;
        ++count;
        bytes += retained;
        return true;
    }
    void release(std::size_t retained) noexcept
    {
        --count;
        bytes -= retained;
    }
};
}
