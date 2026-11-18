// Exact line correspondence aligns two texts by their lines, keeping only matches every optimal alignment shares.
#pragma once

#include "Correspondence.h"
#include <string_view>
#include <vector>

namespace branch_truce {

// Exact line LCS, retaining only matches shared by every optimal alignment.
// Lines the texts share at their start and end bound how far any optimal
// alignment strays from the diagonal, so a few changed lines in a long file
// cost about one band of cells per line rather than the product of the line
// counts. Texts over kMaxIndexedLines, or whose cost() is over
// kMaxAlignmentCells, are one Ambiguous passage. See Correspondence.md for the
// cost, conservative range policy and evidence.
// Called directly, it also rejects text outside the contract's inputs with
// std::invalid_argument.
class ExactLineCorrespondence final : public CorrespondenceAlgorithm {
public:
    [[nodiscard]] std::vector<Passage> compare(std::string_view source, std::string_view result) const override;
    [[nodiscard]] std::size_t cost(std::string_view source, std::string_view result) const override;
    [[nodiscard]] std::string_view identity() const noexcept override { return "exact-line/3"; }
};

}
