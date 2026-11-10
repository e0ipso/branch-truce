// Recovery replay rebuilds file sessions from a journal by repeating each recorded domain transition.
#pragma once

#include "core/Correspondence.h"
#include "core/SessionState.h"
#include "recovery/RecoveryRecord.h"
#include "storage/RecoveryStore.h"
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace branch_truce {
// A repository mutation the journal recorded, with the file it targeted and
// what inspection saw afterwards, if the journal got that far.
struct RecoveryMutation {
    QString id;
    MutationKind kind = MutationKind::Publication;
    std::optional<MutationObservation> observation;
    FileSession target;
    bool completed = false;
    RecoveryBoundary boundary;
};

struct RecoveryReplay {
    bool succeeded = false;
    Problem problem;
    // The journal was written by a newer Branch Truce, not damaged.
    bool newerFormat = false;
    bool incompleteTail = false;
    // False when every record is a launch capture or a large-file load: the
    // store holds no app edit, decision, Save, review or repository outcome.
    bool userWork = false;
    // File IDs with at least one record after their launch capture, other than
    // a large-file load.
    std::set<std::string> editedFiles;
    std::vector<FileSession> files;
    std::vector<RecoveryMutation> mutations;
    std::map<std::string, RecoveryBoundary> boundaries;
};

// Repeats each record's domain transition on the file it names and compares
// the result with the record's canonical encoding. Files align their sources
// with correspondence, as loaded files do. No repository I/O or
// acknowledgment happens here.
[[nodiscard]] RecoveryReplay replayRecovery(const storage::RecoveryStore &store,
                                            std::shared_ptr<const CorrespondenceAlgorithm> correspondence = {});

// The record of a mutation intent, or with an observation, of what inspection
// saw afterwards. Both record file as it was when the mutation was admitted.
[[nodiscard]] storage::RecoveryTransaction mutationRecoveryTransaction(const FileSession &file,
                                                                       const MutationFact &fact);
}
