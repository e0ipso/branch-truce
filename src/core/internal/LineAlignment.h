// The two line alignment paths behind ExactLineCorrespondence, callable one at a time.
#pragma once
#include "../Correspondence.h"
#include <optional>
#include <string_view>
#include <vector>

namespace branch_truce::detail {
// Passages from the banded Hirschberg boundary paths. It has no work limit of
// its own: ExactLineCorrespondence runs it only when its band cost is within
// kMaxAlignmentCells.
std::vector<Passage> hirschbergPassages(std::string_view source, std::string_view result);

// The same passages from Myers' first and last optimal paths, or nullopt when
// the changed-line count passes its cap, the search passes its work budget, or
// a text is over kMaxIndexedLines.
std::optional<std::vector<Passage>> myersPassages(std::string_view source, std::string_view result);
}
