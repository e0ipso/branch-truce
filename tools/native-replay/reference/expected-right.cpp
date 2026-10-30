#include "net/retry.h"

#include <chrono>
#include <random>
#include <thread>

namespace net {

using namespace std::chrono_literals;

constexpr int maxAttempts = 3;
constexpr auto baseDelay = 200ms;

Result retry(const Request &request)
{
    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        auto result = send(request);
        if (result.ok())
            return result;
        std::this_thread::sleep_for(baseDelay * attempt);
    }
    return Result::exhausted(request);
}

} // namespace net