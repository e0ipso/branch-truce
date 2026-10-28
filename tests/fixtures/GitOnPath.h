#pragma once

#include <QStandardPaths>
#include <QString>

// The Git that main would pass to the controller: the one PATH names now.
// Tests that do not choose a Git construct the controller with this.
inline QString gitOnPath()
{
    return QStandardPaths::findExecutable(QStringLiteral("git"));
}
