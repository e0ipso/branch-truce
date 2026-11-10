// Tests drive the event loop around the real asynchronous command contract.
#pragma once
#include "EventLoop.h"
#include "session/SessionStore.h"
#include <QFuture>
#include <utility>

namespace test_support {
template <typename T> T await(QFuture<T> future)
{
    waitUntil([&] { return future.isFinished(); }, "command did not complete");
    CHECK(!future.isCanceled());
    return future.result();
}
template <typename T> T await(T value) { return value; }
template <typename Store, typename Result> auto awaitAligned(Store &store, Result result)
{
    const auto outcome = await(std::move(result));
    if constexpr (requires { store.files(); }) {
        if (outcome == branch_truce::Transition::Applied)
            for (const auto &file : store.files()) {
                const auto aligned = await(store.awaitAlignment(file.id(), file.current().id));
                CHECK(aligned == branch_truce::Transition::Applied || aligned == branch_truce::Transition::NoChange);
            }
    }
    return outcome;
}
}
