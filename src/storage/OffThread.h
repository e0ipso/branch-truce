// Off-thread work runs a blocking function on a thread pool and hands its result back on the caller's thread.
#pragma once

#include <QFuture>
#include <QFutureWatcher>
#include <QObject>
#include <QPromise>
#include <QThreadPool>
#include <memory>
#include <type_traits>
#include <utility>

namespace branch_truce {
// Runs work on pool, then done with its result on context's thread. done never
// runs once context is gone; work still finishes on the pool. The returned
// future holds the result for a caller that must wait for it. This is the one
// way the layers move file, Git or alignment work off the GUI thread.
template <typename Work, typename Done>
QFuture<std::invoke_result_t<Work &>> runOffThread(QThreadPool &pool, QObject *context, Work work, Done done)
{
    using Result = std::invoke_result_t<Work &>;
    auto promise = std::make_shared<QPromise<Result>>();
    auto *watcher = new QFutureWatcher<Result>(context);
    QObject::connect(watcher, &QFutureWatcher<Result>::finished, context, [watcher, done = std::move(done)] {
        watcher->deleteLater();
        done(watcher->result());
    });
    watcher->setFuture(promise->future());
    promise->start();
    pool.start([promise, work = std::move(work)]() mutable {
        promise->addResult(work());
        promise->finish();
    });
    return promise->future();
}
}
