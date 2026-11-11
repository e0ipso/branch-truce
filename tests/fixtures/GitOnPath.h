#pragma once

#include "git/GitAdapter.h"

#include <QStandardPaths>
#include <QString>

// The Git that main would pass to the controller: the one PATH names now.
// Tests that do not choose a Git construct the controller with this.
inline QString gitOnPath() { return QStandardPaths::findExecutable(QStringLiteral("git")); }

// The Git program main builds: executable, by default the one on PATH, reading the configuration
// this process's environment selects now.
inline branch_truce::git::GitProgram gitProgramOnPath(const QString &executable = gitOnPath())
{
    return {executable, branch_truce::git::GitEnvironment::fromProcess()};
}
