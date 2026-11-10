// Checkpoint import is a transactional engine boundary, separate from editing.
#pragma once

#include "SessionState.h"

namespace branch_truce {
struct SessionCheckpoint {
    RevisionHistory::Checkpoint history;
    PublicationCheckpoint publication;
    bool aligned = false;
    [[nodiscard]] static SessionCheckpoint capture(const FileSession &file);
    // Rebuilds derived state from recorded drafts. Invalid input throws and
    // leaves file unchanged, including its immutable launch captures.
    void restore(FileSession &file, std::vector<RecordedRevision> revisions) &&;
};
}
