// Recovery restore chooses a saved session for this operation and reconciles it before editing resumes.
#pragma once

#include "core/Correspondence.h"
#include "git/GitAdapter.h"
#include "recovery/RecoveryJournal.h"
#include "recovery/RecoveryReconciler.h"
#include "recovery/RecoveryReplay.h"
#include "storage/FileSystem.h"
#include <QDateTime>
#include <QStringList>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace branch_truce {
// One matching store with app edits when restoration is blocked. The user
// picks one of these; paths are the exact path bytes of the files it edited.
struct RecoveryChoice {
    QString session;
    QString directory;
    QDateTime lastChanged;
    std::vector<std::string> paths;
};

struct RecoveryRestore {
    bool found = false;
    // The adopted store; set exactly when found.
    std::optional<AdoptedStore> adopted;
    std::vector<FileSession> files;
    std::set<std::string> newFiles;
    QStringList diagnostics;
    // Filled only when several stores with app edits block restoration.
    std::vector<RecoveryChoice> choices;
    // Files whose repository file was replaced by a copy: bytes, mode, size,
    // device, index entries and conversion policy still match the launch
    // capture, only the inode and timestamps changed, as when git mergetool
    // moves MERGED aside and copies it back. Each keeps its entry in the
    // adopted store's failures (copiedLaunchFailure) with the identity
    // validation observed; only a waiting launcher whose MERGED copy holds the
    // launch bytes may clear it.
    std::map<std::string, SnapshotFileIdentity> copiedLaunches;
};

struct RestoreOptions {
    // Restored and new files align their sources with this, as loaded files do.
    std::shared_ptr<const CorrespondenceAlgorithm> correspondence;
    // A set-aside store whose marker is older than kSetAsideLifetimeDays at
    // this time is deleted. An invalid time deletes none.
    QDateTime now;
    std::shared_ptr<const storage::FileSystem> files = storage::systemFileSystem();
};

// How long a set-aside store is kept before a later restore deletes it.
inline constexpr int kSetAsideLifetimeDays = 30;

// Worker-only, read-only. Runs Git on runners from git.
[[nodiscard]] QString revalidateCopiedLaunch(const FileSnapshots &captured, const SnapshotFileIdentity &identity,
                                             const git::RepositoryContext &repository, const git::GitRunners &git);

// Worker-only. Inspects the repository read-only, running Git on runners from
// git; ambiguous inventories and
// mismatched identities fail closed and no worktree or index mutation is
// retried. Stores holding only launch captures carry no work: one that is not
// adopted is deleted. chosenSession names the store the user picked; it is
// adopted and the other matching stores with app edits are set aside. If the
// named store is not adopted, no store with app edits is adopted in its place:
// the edited stores stay unmarked and are listed as choices. Setting aside
// writes a marker inside the store; a set-aside store is deleted once it
// expires (see RestoreOptions::now).
[[nodiscard]] RecoveryRestore restoreRecovery(const QString &inventory, const QString &exclude,
                                              const git::RepositoryContext &repository,
                                              const std::vector<FileSession> &observed, const git::GitRunners &git,
                                              const QString &chosenSession = {}, const RestoreOptions &options = {});
}
