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

template <typename Work, typename Done>
QFuture<std::invoke_result_t<Work &>> runOffThread(QThreadPool &pool, QObject *context, Work work, Done done)
{
    using Result = std::invoke_result_t<Work &>;
    auto promise = std::make_shared<QPromise<Result>>();
    auto *watcher = new QFutureWatcher<Result>(context);
    QObject::connect(watcher, &QFutureWatcher<Result>::finished, context, [watcher, done = std::move(done)] {
        watcher->deleteLater();
        // Work that can fail supplies a typed result. Unexpected exceptions
        // still finish the future and cannot unwind through Qt's event loop.
        if (watcher->future().isCanceled()) return;
        std::optional<Result> result;
        try {
            result.emplace(watcher->result());
        } catch (...) {
            // The exceptional future remains inspectable by its caller.
            return;
        }
        done(*result);
    });
    watcher->setFuture(promise->future());
    promise->start();
    pool.start([promise, work = std::move(work)]() mutable {
        try {
            promise->addResult(work());
        } catch (...) {
            promise->setException(std::current_exception());
        }
        promise->finish();
    });
    return promise->future();
}
}
