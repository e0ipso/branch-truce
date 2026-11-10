// Problem text turns the typed failures of Git, storage, recovery and the mergetool into translated sentences.
#pragma once

#include "storage/Problem.h"
#include <QString>

namespace branch_truce::problem_text {

// The translated sentence of problem, with the problem it wraps.
[[nodiscard]] QString text(const Problem &problem);
// detail as the user reads it: the sentence of an encoded problem, or detail
// itself when it holds anything else.
[[nodiscard]] QString detail(const QString &detail);
// Each line of details, as detail() shows it.
[[nodiscard]] QString details(const QStringList &details);
}
