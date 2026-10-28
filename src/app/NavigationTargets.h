#pragma once
#include "core/EditingCommands.h"
#include <cstddef>
#include <string>
#include <vector>

namespace branch_truce {
// A place navigation can stop at: a byte range of the current result, or of
// the source when the result has no mapped range for it.
struct ScopeTarget {
    std::size_t byteBegin = 0;
    std::size_t byteEnd = 0;
    bool resultRange = false;
    std::string contributionId;
    SourceSide side = SourceSide::Stage2;
};

// The UTF-16 length of the first bytes of utf8, or -1 when bytes is past its
// end or the length does not fit an int.
[[nodiscard]] int utf16Offset(const std::string &utf8, std::size_t bytes);

// Whether every decision of the choice is recorded and still undecided. A
// choice without decisions is not.
[[nodiscard]] bool undecidedChoice(const FileSession &file, const ChoiceTarget &target);

// Whether the section has a line-scoped choice on its side.
[[nodiscard]] bool sectionHasLine(const FileSession &file, const ChoiceTarget &section);

// The current result's ranges that differ from the left or right source, in
// result order, one entry per range. Empty without a correspondence.
[[nodiscard]] std::vector<ScopeTarget> differenceTargets(const FileSession &file);

// The undecided choices, one entry per choice, at their mapped result range
// or, when unmapped, at their source range. A section with line choices is
// left to its lines. Mapped targets come first in result order, then the
// unmapped ones in source byte order. Empty without a correspondence.
[[nodiscard]] std::vector<ScopeTarget> undecidedTargets(const FileSession &file);
}
