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
};

struct RecoveryReplay {
    bool succeeded = false;
    QString error;
    bool incompleteTail = false;
    std::vector<FileSession> files;
    std::vector<RecoveryMutation> mutations;
};

// Replays existing domain transitions and compares every complete state with
// its canonical encoding. No repository I/O or acknowledgment happens here.
[[nodiscard]] RecoveryReplay replayRecovery(const storage::RecoveryStore &store);
[[nodiscard]] storage::RecoveryTransaction mutationRecoveryTransaction(
    const FileSession &file, const QString &kind, const QString &id,
    const QString &observation = {});

struct RecoveryRestore {
    bool found = false;
    QString directory;
    std::vector<FileSession> files;
    std::map<std::string, QString> failures;
    std::set<std::string> newFiles;
    QStringList diagnostics;
    std::shared_ptr<QLockFile> lease;
};

// Worker-only, read-only repository inspection. Ambiguous inventories and
// mismatched identities fail closed; no worktree/index mutation is retried.
[[nodiscard]] RecoveryRestore restoreRecovery(const QString &inventory,
    const QString &exclude, const git::RepositoryContext &repository,
    const std::vector<FileSession> &observed, const QString &gitExecutable);
}
