// Exact line correspondence aligns two texts by their lines, keeping only matches every optimal alignment shares.
#pragma once

#include "Correspondence.h"
#include <string_view>
#include <vector>

namespace branch_truce {

// Exact line LCS, retaining only matches shared by every optimal alignment.
// Texts whose line counts multiply to more than kMaxAlignmentCells are one
// Ambiguous passage. See Correspondence.md for the cost, conservative range
// policy and evidence.
// Called directly, it also rejects text outside the contract's inputs with
// std::invalid_argument.
class ExactLineCorrespondence final : public CorrespondenceAlgorithm {
public:
    [[nodiscard]] std::vector<Passage> compare(std::string_view source, std::string_view result) const override;
    [[nodiscard]] std::string_view identity() const noexcept override { return "exact-line/1"; }
};

}
