#pragma once

#include "ComparisonScene.h"
#include "recovery/RecoveryRestore.h"

namespace comparison_scene {
// Typing is recorded once its burst ends and its record is durable.
inline void checkRecovered(SessionStore &store, SessionModel &files, const Draft &expected,
                           std::size_t expectedFiles = 1)
{
    settleFor("typing recorded", [&] { return !store.recording(); });
    const auto &file = *store.file("choices");
    const auto replay = replayRecovery(storage::RecoveryStore(store.recoveryDirectory()));
    if (!replay.succeeded) throw std::runtime_error("comparison recovery: " + replay.problem.encoded().toStdString());
    CHECK(replay.succeeded && replay.files.size() == expectedFiles);
    const auto recovered = std::find_if(replay.files.begin(), replay.files.end(),
                                        [&](const FileSession &candidate) { return candidate.id() == file.id(); });
    CHECK(recovered != replay.files.end());
    CHECK(recovered->current().draft == expected);
    CHECK(recovered->current().id == file.current().id);
    CHECK(*recovered->snapshots() == *file.snapshots());
    CHECK(!recovered->facts().resolved && !files.hasPendingRecovery());
}

} // namespace comparison_scene
