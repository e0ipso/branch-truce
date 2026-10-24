#pragma once

#include "core/SessionState.h"
#include "storage/RecoveryStore.h"

namespace branch_truce {
// Complete state at one acknowledgment boundary. Original sources and exact
// byte strings use content-addressed payloads, including non-UTF-8 path bytes.
[[nodiscard]] storage::RecoveryTransaction recoveryTransaction(
    const FileSession &file, const QString &action, const QString &transactionId);
}
