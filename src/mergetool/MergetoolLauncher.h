// The Git mergetool launcher waits for the reviewed result to be handed back to its parent process.
#pragma once
#include "git/GitAdapter.h"
#include "mergetool/MergetoolSocket.h"
#include <QString>
#include <QStringList>
#include <functional>

namespace branch_truce {
// Starts a Branch Truce window on the launched path of worktreeRoot. Returns
// why it could not, or an empty string once the window process started.
using MergetoolWindowStarter = std::function<QString(const QString &worktreeRoot, const QByteArray &path)>;

// Starts the installed program this launcher runs from. Its standard streams
// go to the null device, so it never writes into the git mergetool terminal.
[[nodiscard]] QString startInstalledWindow(const QString &worktreeRoot, const QByteArray &path);

// What the launcher takes from its process besides the command line. The
// defaults are the real ones.
struct MergetoolLauncherOptions {
    // Whether the process at the other end of a connected socket runs as this user.
    std::function<bool(qintptr descriptor)> peerIsCurrentUser = mergetoolPeerIsCurrentUser;
    // How long the session may take to accept or reject the attach.
    int replyWaitMs = 15000;
    // Used when no window listens. --no-window clears it.
    MergetoolWindowStarter startWindow = startInstalledWindow;
    // The sentence the user reads for a detail that may hold an encoded
    // Problem, from this launcher or from the window. Empty prints it as it is.
    std::function<QString(const QString &detail)> describe = {};
};

// Blocking Git mergetool child. It exits 0 only after the session reports a
// written revision and MERGED still holds that revision's canonical object,
// so Git stages exactly the reviewed result. Every other end restores MERGED
// and exits 1. arguments is the command line, starting with the program;
// a QCoreApplication must exist. git is the Git it inspects the repository
// with; main resolves it.
int runMergetoolLauncher(const QStringList &arguments, const git::GitProgram &git,
                         const MergetoolLauncherOptions &options = {});
}
