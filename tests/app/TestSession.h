// Owns the real session store and the presentation model built over it.
#pragma once

#include "app/AppPaths.h"
#include "app/SessionModel.h"
#include "session/SessionStore.h"
#include <utility>

namespace test_support {
struct TestSession {
    explicit TestSession(
        QString directory = branch_truce::newRecoveryDirectory(),
        std::shared_ptr<const branch_truce::storage::FileSystem> files = branch_truce::storage::systemFileSystem())
        : store(std::move(directory), std::move(files)), rows(store)
    {}
    // Reverse destruction disconnects the model before the store drains its owned worker.
    branch_truce::SessionStore store;
    branch_truce::SessionModel rows;
};
}
