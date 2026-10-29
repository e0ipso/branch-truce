#pragma once
#include <QString>

namespace branch_truce {
// Blocking Git mergetool child. Exit 0 only after the session reports the
// exact published revision was handed back for the parent to stage.
// gitExecutable is the Git it inspects the repository with; main resolves it.
int runMergetoolLauncher(int argc, char **argv, const QString &gitExecutable);
}
