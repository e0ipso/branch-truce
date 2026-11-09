// The Git mergetool launcher waits for the reviewed result to be handed back to its parent process.
#pragma once
#include "git/GitAdapter.h"

namespace branch_truce {
// Blocking Git mergetool child. Exit 0 only after the session reports the
// exact published revision was handed back for the parent to stage.
// git is the Git it inspects the repository with; main resolves it.
int runMergetoolLauncher(int argc, char **argv, const git::GitProgram &git);
}
