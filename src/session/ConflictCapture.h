// Conflict capture turns inspected Git sources into immutable session snapshots and support decisions.
#pragma once
#include "core/FileSnapshot.h"
#include "core/SessionState.h"
#include "git/ConflictClassifier.h"
#include <optional>
#include <string>

namespace branch_truce {
// How a conflict that cannot be captured is listed: a failed read or inspection
// is unreadable, any other exclusion stays outside the app.
[[nodiscard]] Support supportFor(const git::Conflict &file);

// The decoded text and exact bytes of one document, with its format.
[[nodiscard]] TextSnapshot textSnapshot(const storage::TextDocument &document);

// The immutable launch capture of one editable conflict in a merge or rebase:
// its sources, launch result, provenance and conversion policy. Empty unless
// the conflict is editable, has a result and both sides, and every stage
// agrees with its source on path, object ID, mode, worktree and policy.
[[nodiscard]] std::optional<FileSnapshots> captureFile(const git::Conflict &file,
    const git::RepositoryContext &repository, const std::string &sessionId);
}
