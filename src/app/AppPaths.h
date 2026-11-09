// App paths name the standard locations the application keeps its own data in.
#pragma once
#include <QString>

namespace branch_truce {
// Holds every session's recovery store. Built from GenericDataLocation and a
// fixed segment, so setting an organization or application name never moves it.
[[nodiscard]] QString recoveryRoot();
// A new session's recovery store: a fresh directory under recoveryRoot().
[[nodiscard]] QString newRecoveryDirectory();
}
