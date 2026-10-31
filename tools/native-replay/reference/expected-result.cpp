#include "net/retry.h"

#include <chrono>
#include <random>
#include <thread>

namespace net {

using namespace std::chrono_literals;

<<<<<<< HEAD
constexpr int maxAttempts = 5;
constexpr auto attemptTimeout = 2s;
=======
constexpr int maxAttempts = 3;
constexpr auto baseDelay = 200ms;
>>>>>>> feature/backoff

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