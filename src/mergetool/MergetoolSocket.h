// The mergetool socket names where a window listens for one worktree operation and checks who connects.
#pragma once

#include "git/GitTypes.h"
#include "storage/Problem.h"
#include <QByteArray>
#include <QString>

namespace branch_truce {

[[nodiscard]] QByteArray mergetoolSessionKey(const git::RepositoryContext &repository);
// The per-user socket directory for a runtimeDirectory (XDG_RUNTIME_DIR),
// ignoring TMPDIR: <runtimeDirectory>/branch-truce when it is absolute and the
// socket path stays within 106 bytes. Otherwise a private directory under /tmp
// with an unpredictable name, which a link in this user's cache directory
// names, so the window and its launchers find the same one and no other user
// can create it first. Empty when that directory cannot be made.
[[nodiscard]] QString mergetoolSocketDirectory(const QByteArray &runtimeDirectory);
// Absolute path of the local socket for one worktree operation in directory.
// Empty when the operation is not a supported merge or rebase with both stage
// commits. The GUI and the launcher both use it.
[[nodiscard]] QString mergetoolServerName(const QString &directory, const git::RepositoryContext &repository);
// Creates directory with mode 0700 when missing, then requires a real
// directory owned by this user with no group or other access.
[[nodiscard]] bool prepareMergetoolSocketDirectory(const QString &directory, Problem *error);
// True when the process at the other end of a connected local socket runs as this user.
[[nodiscard]] bool mergetoolPeerIsCurrentUser(qintptr descriptor);
}
