#pragma once

#include "SessionRecovery.h"
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
    std::vector<FileSession> files;
    std::map<std::string, QString> failures;
    std::set<std::string> newFiles;
    QStringList diagnostics;
    // Filled only when several stores with app edits block restoration.
    std::vector<RecoveryChoice> choices;
    std::shared_ptr<QLockFile> lease;
};

// Worker-only, read-only repository inspection. Ambiguous inventories and
// mismatched identities fail closed; no worktree/index mutation is retried.
// When several stores match, stores holding only launch captures are set
// aside and never make the inventory ambiguous. chosenSession names the store
// the user picked; it is adopted and the other matching stores are set aside.
// Setting aside writes a marker inside the store and never deletes it.
[[nodiscard]] RecoveryRestore restoreRecovery(const QString &inventory,
    const QString &exclude, const git::RepositoryContext &repository,
    const std::vector<FileSession> &observed, const QString &gitExecutable,
    const QString &chosenSession = {});
}
