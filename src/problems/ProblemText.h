// Problem text turns the typed failures of Git, storage, recovery and the mergetool into translated sentences.
#pragma once

#include "problems/Problem.h"
#include <QString>

namespace branch_truce::problem_text {

// The translated sentence of problem, with the problem it wraps.
[[nodiscard]] QString text(const Problem &problem);
}
