#pragma once

#include "core/FileSnapshot.h"
#include <QByteArray>
#include <QString>
#include <functional>
#include <memory>
#include <optional>

namespace branch_truce::storage {
enum class DestinationFailure {
    None, InvalidPath, Symlink, NotRegularFile, PermissionDenied, PathChanged,
    ContentChanged, UnsupportedMetadata, UnsupportedFilesystem, IoError,
    Cancelled, CleanupFailure
};
enum class PublicationState { Unchanged, Published, Uncertain };
struct DestinationCapture;
struct PublicationResult;

// Capture at launch or after a verified write, before the user edits. Copies pin
// the same inodes. A fresh capture must not silently accept an external edit.
class DestinationSnapshot {
public:
    struct State;
    [[nodiscard]] const QString &worktreeRoot() const;
    [[nodiscard]] const QByteArray &path() const;
    [[nodiscard]] const QByteArray &bytes() const;
    [[nodiscard]] SnapshotFileIdentity identity() const;
private:
    explicit DestinationSnapshot(std::shared_ptr<const State> state);
    std::shared_ptr<const State> d;
    friend DestinationCapture captureDestination(const QString &, const QByteArray &);
    friend PublicationResult publishDestination(const DestinationSnapshot &, const QByteArray &,
        const QByteArray &, const std::function<bool()> &);
};
struct DestinationCapture {
    std::optional<DestinationSnapshot> destination;
    DestinationFailure failure = DestinationFailure::None;
    QString diagnostic;
};
struct PublicationResult {
    PublicationState state = PublicationState::Unchanged;
    DestinationFailure failure = DestinationFailure::None;
    QString diagnostic;
    // Present only after verified publication. Use this baseline for a later write.
    std::optional<DestinationSnapshot> destination;
    // Exact native bytes, never a display path. On uncertainty the exchange slot
    // is retained without rollback. It may contain the displaced entry or the
    // prepared result if the exchange outcome is unknown. An ancestor rename
    // can move this path;
    // it is a location hint, not authority to overwrite or delete anything.
    QByteArray retainedPath;
};

// Requires an absolute canonical worktree root and exact relative filename bytes.
// Rejects symlinks in every component, including the root's ancestry.
[[nodiscard]] DestinationCapture captureDestination(const QString &worktreeRoot, const QByteArray &path);

// Synchronous worker operation. Caller owns draft persistence, review/revision,
// operation/index/conversion guards, the worktree mutation queue and Git staging.
// beginMutation is mandatory and runs before creating any temporary entry.
// No draft buffer is consumed or changed. Failures never authorize staging.
[[nodiscard]] PublicationResult publishDestination(const DestinationSnapshot &expected,
    const QByteArray &serializedBytes, const QByteArray &agreedGitMode,
    const std::function<bool()> &beginMutation);
} // namespace branch_truce::storage
