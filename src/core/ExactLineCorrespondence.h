// Exact line correspondence aligns two texts by their lines, keeping only matches every optimal alignment shares.
#pragma once

#include "Correspondence.h"
#include <string_view>
#include <vector>

namespace branch_truce {

// Exact line LCS, retaining only matches shared by every optimal alignment.
// Linear-space Myers runs first. Its work grows with the lines the texts
// change, so a few changed lines in a long file align at any length up to
// kMaxIndexedLines. When the changed-line count passes its cap or its work
// budget, a banded Hirschberg runs if its band cost is within
// kMaxAlignmentCells. Both give the same passages. Texts over
// kMaxIndexedLines, and texts that neither path can align within its limit,
// are one Ambiguous passage. See Correspondence.md for the cost, conservative
// range policy and evidence.
// Called directly, it also rejects text outside the contract's inputs with
// std::invalid_argument.
class ExactLineCorrespondence final : public CorrespondenceAlgorithm {
public:
    [[nodiscard]] std::vector<Passage> compare(std::string_view source, std::string_view result) const override;
    [[nodiscard]] std::size_t cost(std::string_view source, std::string_view result) const override;
    [[nodiscard]] std::string_view identity() const noexcept override { return "exact-line/4"; }
};

}
