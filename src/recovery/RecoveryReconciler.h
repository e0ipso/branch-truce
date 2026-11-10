// Recovery reconciliation checks recovered files against the live repository before editing resumes.
#pragma once

#include "git/GitAdapter.h"
#include "recovery/RecoveryReplay.h"
#include "storage/DestinationWriter.h"
#include "storage/RecoveryStore.h"
#include <QString>
#include <optional>
#include <vector>

namespace branch_truce {

// The read-only repository facts reconciliation checks recovered work
// against. The default runs Git on a runner from git and reads the
// destination; both block, so call it from a worker thread. A test
// substitutes its own facts.
class RepositoryProbe {
public:
    explicit RepositoryProbe(git::GitRunners git);
    virtual ~RepositoryProbe() = default;
    // Runs one read-only Git request to completion.
    [[nodiscard]] virtual git::Result inspect(const git::Request &request) const;
    [[nodiscard]] virtual storage::DestinationCapture capture(const QString &worktreeRoot,
                                                              const QByteArray &path) const;

private:
    git::GitRunners git_;
};

struct Reconciliation {
    // Empty when the file may be edited; otherwise its recovery error.
    Problem failure;
    // The live identity when the only change is git mergetool's copy of the
    // launch file. The file still fails with copiedLaunchFailure().
    std::optional<SnapshotFileIdentity> copied;
};

// Checks one recovered file against disk and index, given the files this
// load observed. Staging that a crash interrupted after Git installed the
// exact reviewed result is verified and committed through lease, which
// updates file and marks its mutations completed. Nothing in the repository
// is written or retried.
[[nodiscard]] Reconciliation reconcile(FileSession &file, std::vector<RecoveryMutation> &mutations,
                                       const std::vector<FileSession> &observed,
                                       const git::RepositoryContext &repository, const RepositoryProbe &probe,
                                       storage::RecoveryLease &lease);

// The recovery error of a file git mergetool replaced with a copy, encoded
// as the session store keeps it.
[[nodiscard]] QString copiedLaunchFailure();

// Empty while the file still has the identity validation observed and the
// launch bytes, and its index entries and conversion policy still match the
// capture. Otherwise the error that replaces copiedLaunchFailure.
[[nodiscard]] Problem revalidateCopiedLaunch(const FileSnapshots &captured, const SnapshotFileIdentity &identity,
                                             const git::RepositoryContext &repository, const RepositoryProbe &probe);
}
