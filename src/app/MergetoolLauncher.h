#pragma once

namespace branch_truce {
// Blocking Git mergetool child. Exit 0 only after the session reports the
// exact published revision was handed back for the parent to stage.
int runMergetoolLauncher(int argc, char **argv);
}
