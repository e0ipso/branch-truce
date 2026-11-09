// The mergetool protocol names a session's socket and intake, and reads and restores the files Git launched with.
#pragma once

#include "git/GitAdapter.h"
#include <QByteArray>
#include <QString>
#include <optional>

namespace branch_truce {

[[nodiscard]] QByteArray mergetoolSessionKey(const git::RepositoryContext &repository);
// Per-user socket directory for a runtimeDirectory (XDG_RUNTIME_DIR) that
// ignores TMPDIR: <runtimeDirectory>/branch-truce when it is absolute and the
// socket path stays within 106 bytes, otherwise /tmp/branch-truce-<uid>.
[[nodiscard]] QString mergetoolSocketDirectory(const QByteArray &runtimeDirectory);
// Absolute path of the local socket for one worktree operation in directory.
// Empty when the operation is not a supported merge or rebase with both stage
// commits. The GUI and the launcher both use it.
[[nodiscard]] QString mergetoolServerName(const QString &directory, const git::RepositoryContext &repository);
// Creates directory with mode 0700 when missing, then requires a real
// directory owned by this user with no group or other access.
[[nodiscard]] bool prepareMergetoolSocketDirectory(const QString &directory, QString *error);
// True when the process at the other end of a connected local socket runs as this user.
[[nodiscard]] bool mergetoolPeerIsCurrentUser(qintptr descriptor);
// Where launchers copy their inputs: inside the private socket directory, so
// the copies live no longer than the user's runtime directory.
[[nodiscard]] QString mergetoolIntakeRoot(const QString &socketDirectory);

struct MergetoolIntake {
    QString invocation;
    QByteArray path;
    QByteArray mergedSha256;
    QByteArray baseSha256;
    QByteArray localSha256;
    QByteArray remoteSha256;
    bool basePresent = false;
    bool localPresent = false;
    bool remotePresent = false;
};

struct MergetoolSources {
    QByteArray path;
    QByteArray merged;
    QByteArray base;
    QByteArray local;
    QByteArray remote;
    bool basePresent = false;
    bool localPresent = false;
    bool remotePresent = false;
};

// Copies the sources into <root>/<sessionKey>/<invocation>, creating each
// directory with mode 0700. Returns that directory, or an empty string. The
// launcher that wrote it removes it when it exits.
[[nodiscard]] QString writeMergetoolIntake(const QString &root, const QByteArray &sessionKey, const QString &invocation,
                                           const MergetoolSources &sources, QString *error);
// Re-reads that directory and checks it still sits under root and the session key.
[[nodiscard]] std::optional<MergetoolIntake> readMergetoolIntake(const QString &directory, const QString &root,
                                                                 const QByteArray &sessionKey, QString *error);
void removeMergetoolIntake(const QString &directory);
// Removes the session's intakes whose launcher process has ended, as after a
// launcher was killed. Returns how many it removed.
int pruneMergetoolIntakes(const QString &root, const QByteArray &sessionKey);

// Repository-relative path Git passed as MERGED. Rejects parent segments.
[[nodiscard]] std::optional<QByteArray> mergetoolRepositoryPath(const QByteArray &merged);

[[nodiscard]] bool readMergetoolFile(const QByteArray &path, QByteArray *bytes, QString *error);
// Replaces the file with bytes through a sibling temporary file and a rename,
// keeping its permission bits, so a crash leaves either version whole.
[[nodiscard]] bool restoreMergetoolFile(const QByteArray &path, const QByteArray &bytes, QString *error);
}
