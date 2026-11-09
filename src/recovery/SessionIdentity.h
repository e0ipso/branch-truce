// Session identity: the checks that a repository, its index and its policy still match a session's captures.
#pragma once

#include "core/SessionState.h"
#include "git/GitAdapter.h"
#include <QByteArray>
#include <QList>
#include <cstdint>
#include <optional>
#include <string_view>

namespace branch_truce {

// Whether the repository runs the operation the capture was taken in: the
// same worktree, Git directory, operation, source commits, base and rebase
// context. An unsupported repository never matches. Restore and publication
// both decide with this.
[[nodiscard]] bool sameOperation(const SnapshotProvenance &provenance, const git::RepositoryContext &repository);

// The captured conflict stages as index entries.
[[nodiscard]] QList<git::IndexEntry> capturedStages(const FileSnapshots &snapshots);

// Whether the index entries are exactly the captured conflict: one entry per
// captured stage, with its path, mode and object.
[[nodiscard]] bool matchesCapturedConflict(const QList<git::IndexEntry> &entries, const FileSnapshots &snapshots);

// Whether a live conversion policy is the captured one.
[[nodiscard]] bool samePolicy(const ConversionSnapshot &captured, const git::ConversionPolicy &live);

// The revision's text in the file's launch encoding and line endings. Empty
// for a file without captures, or a text that format cannot hold.
[[nodiscard]] std::optional<QByteArray> serializedRevision(const FileSession &file, const Revision &revision);

// Whether a file's permission bits agree with a Git regular-file mode on
// whether it is executable.
[[nodiscard]] bool modeMatches(std::uint32_t fileMode, std::string_view gitMode);
}
