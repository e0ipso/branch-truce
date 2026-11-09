#pragma once

// Event-loop driving for the Qt suites. Nothing here lets an exception unwind through Qt: a
// predicate that throws, or an EXPECT that fails in a slot, stops the wait and the failure is
// raised after the loop returns.
//
//   waitUntil(ready, detail)         runs the loop until ready() holds.
//   AsyncCompletion(service, signal) drives one request of a start()/busy()/finished() service.

#include "TestHarness.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QObject>
#include <QTimer>

#include <exception>
#include <string>
#include <vector>

namespace test_support {

// How long a wait lasts before it fails, unless a case asks for longer.
inline constexpr int kWaitDeadlineMs = 25000;
// How long one asynchronous service request may take.
inline constexpr int kRequestDeadlineMs = 10000;

// Runs the event loop until `ready` returns true, re-evaluating it every `pulseMs`. Fails the case
// with `detail` after `deadlineMs`. Stops early when ready() throws or an EXPECT fails, and
// raises that failure here.
template <typename Predicate>
void waitUntil(Predicate ready, const std::string &detail, int deadlineMs = kWaitDeadlineMs, int pulseMs = 10)
{
    std::exception_ptr error;
    bool satisfied = false;
    const auto evaluate = [&] {
        try {
            satisfied = ready();
        } catch (...) {
            error = std::current_exception();
        }
        return satisfied || error || failurePending();
    };
    if (!evaluate()) {
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        QTimer pulse;
        pulse.setInterval(pulseMs);
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&pulse, &QTimer::timeout, &loop, [&] {
            if (evaluate()) loop.quit();
        });
        deadline.start(deadlineMs);
        pulse.start();
        loop.exec();
    }
    if (error) std::rethrow_exception(error);
    throwPending();
    if (!satisfied) throw Failure(detail);
}

// Collects the results one service emits. run() checks the service's request contract: start()
// accepts the request, the service stays busy, a second start() is refused without a result,
// and exactly one result arrives asynchronously.
template <typename Service, typename Result> class AsyncCompletion {
public:
    AsyncCompletion(Service &service, void (Service::*finished)(const Result &)) : service_(service)
    {
        QObject::connect(&service, finished, &loop_, [this](const Result &result) {
            results.push_back(result);
            loop_.quit();
        });
    }
    AsyncCompletion(const AsyncCompletion &) = delete;
    AsyncCompletion &operator=(const AsyncCompletion &) = delete;

    // Waits for a result. Exactly one must have arrived, and one more pass of the event loop
    // must not add another.
    Result wait(int deadlineMs = kRequestDeadlineMs)
    {
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &loop_, &QEventLoop::quit);
        deadline.start(deadlineMs);
        if (results.empty()) loop_.exec();
        throwPending();
        CHECK_EQ(results.size(), 1u);
        CHECK(!service_.busy());
        QCoreApplication::processEvents();
        CHECK_EQ(results.size(), 1u);
        return results.front();
    }
    template <typename Request> Result run(const Request &request, int deadlineMs = kRequestDeadlineMs)
    {
        CHECK(service_.start(request));
        CHECK(service_.busy() && results.empty()); // Completion must be asynchronous.
        CHECK(!service_.start(request)); // A busy refusal must not emit another result.
        return wait(deadlineMs);
    }

    std::vector<Result> results;

private:
    Service &service_;
    QEventLoop loop_;
};

} // namespace test_support
