#include "net/retry.h"

#include <chrono>
#include <thread>

namespace net {

using namespace std::chrono_literals;

constexpr int maxAttempts = 5;
constexpr auto attemptTimeout = 2s;

Result retry(const Request &request)
{
    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        auto result = send(request, attemptTimeout);
        if (result.ok())
            return result;
        logAttempt(request, attempt, result.error());
    }
    return Result::exhausted(request);
}

} // namespace net