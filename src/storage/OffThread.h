// Off-thread work runs a blocking function on a thread pool and hands its result back on the caller's thread.
#pragma once

#include <QFuture>
#include <QFutureWatcher>
#include <QObject>
#include <QPromise>
#include <QThreadPool>
#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace branch_truce {
// Runs work on pool, then done with its result on context's thread. done never
// runs once context is gone; work still finishes on the pool. The returned
// future holds the result for a caller that must wait for it. This is the one
// way the layers move file, Git or alignment work off the GUI thread.
template <typename Result> QFuture<Result> completedFuture(Result result)
{
    QPromise<Result> promise;
    promise.start();
    promise.addResult(std::move(result));
    promise.finish();
    return promise.future();
}

// Cancellation cancels observation through the returned future, never admitted
// work. The owner callback receives the independently retained terminal result,
// even when that future was canceled. failed runs on the worker and must return
// the consumer's typed failure, including mutation reconciliation when needed.
template <typename Work, typename Done, typename Failed>
QFuture<std::invoke_result_t<Work &>> runOffThread(QThreadPool &pool, QObject *context, Work work, Done done,
                                                   Failed failed)
{
    using Result = std::invoke_result_t<Work &>;
    static_assert(std::is_nothrow_invocable_r_v<Result, Failed &, std::exception_ptr>);
    auto promise = std::make_shared<QPromise<Result>>();
    auto result = std::make_shared<std::optional<Result>>();
    auto ownedWatcher = std::make_unique<QFutureWatcher<Result>>(context);
    auto *watcher = ownedWatcher.get();
    QObject::connect(watcher, &QFutureWatcher<Result>::finished, context, [watcher, result, done = std::move(done)] {
        watcher->deleteLater();
        done(**result);
    });
    promise->start();
    watcher->setFuture(promise->future());
    pool.start([promise, result, work = std::move(work), failed = std::move(failed)]() mutable {
        try {
            result->emplace(work());
        } catch (...) {
            result->emplace(failed(std::current_exception()));
        }
        try {
            promise->addResult(**result);
        } catch (...) {
            promise->setException(std::current_exception());
        }
        promise->finish();
    });
    ownedWatcher.release();
    return promise->future();
}
}
