#pragma once

#include "recovery/RecoveryRecord.h"
#include "git/GitAdapter.h"
#include <QLockFile>
#include <memory>
#include <set>

namespace branch_truce {
struct RecoveryMutation {
    QString id;
    QString kind;
    QString observation;
    FileSession target;
    bool completed = false;
};

struct RecoveryReplay {
    bool succeeded = false;
    QString error;
    bool incompleteTail = false;
    // False when every record is a launch capture: the store holds no app edit,
    // decision, Save, review or repository outcome.
    bool userWork = false;
    // File IDs with at least one record after their launch capture.
    std::set<std::string> editedFiles;
    std::vector<FileSession> files;
    std::vector<RecoveryMutation> mutations;
};

// Replays existing domain transitions and compares every complete state with
// its canonical encoding. No repository I/O or acknowledgment happens here.
[[nodiscard]] RecoveryReplay replayRecovery(const storage::RecoveryStore &store);
[[nodiscard]] storage::RecoveryTransaction mutationRecoveryTransaction(
    const FileSession &file, const QString &kind, const QString &id,
    const QString &observation = {});

// One matching store with app edits when restoration is blocked. The user
// picks one of these; paths are the exact path bytes of the files it edited.
struct RecoveryChoice {
    QString session;
    QString directory;
    QString lastChanged;
    std::vector<std::string> paths;
};

struct RecoveryRestore {
    bool found = false;
    QString directory;
    // Whether the adopted store holds a record other than a launch capture.
    bool userWork = false;
    std::vector<FileSession> files;
    std::map<std::string, QString> failures;
    std::set<std::string> newFiles;
    QStringList diagnostics;
    // Filled only when several stores with app edits block restoration.
    std::vector<RecoveryChoice> choices;
    // Files whose repository file was replaced by a copy: bytes, mode, size,
    // device, index entries and conversion policy still match the launch
    // capture, only the inode and timestamps changed, as when git mergetool
    // moves MERGED aside and copies it back. Each keeps its entry in failures
    // (copiedLaunchFailure) with the identity validation observed; only a
    // waiting launcher whose MERGED copy holds the launch bytes may clear it.
    std::map<std::string, SnapshotFileIdentity> copiedLaunches;
    std::shared_ptr<QLockFile> lease;
};

// The recovery error of a copiedLaunches entry.
[[nodiscard]] QString copiedLaunchFailure();

// Worker-only, read-only. Before a waiting launcher clears a copiedLaunches
// error: empty while the file still has the identity validation observed and
// the launch bytes, and its index entries and conversion policy still match
// the capture. Otherwise the error that replaces copiedLaunchFailure.
[[nodiscard]] QString revalidateCopiedLaunch(const FileSnapshots &captured, const SnapshotFileIdentity &identity,
    const git::RepositoryContext &repository, const QString &gitExecutable);

// Worker-only, read-only repository inspection. Ambiguous inventories and
// mismatched identities fail closed; no worktree/index mutation is retried.
// When several stores match, stores holding only launch captures are set
// aside and never make the inventory ambiguous. chosenSession names the store
// the user picked; it is adopted and the other matching stores are set aside.
// If the named store is not adopted, no store with app edits is adopted in its
// place: the edited stores stay unmarked and are listed as choices.
// Setting aside writes a marker inside the store and never deletes it.
[[nodiscard]] RecoveryRestore restoreRecovery(const QString &inventory,
    const QString &exclude, const git::RepositoryContext &repository,
    const std::vector<FileSession> &observed, const QString &gitExecutable,
    const QString &chosenSession = {});
}
