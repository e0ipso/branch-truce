#pragma once

#include "Correspondence.h"
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace branch_truce {

enum class MarkerKind { Begin, Base, Separator, End };

// One conflict-marker line. `line` is zero-based. `bytes` covers the whole
// line, including its line ending, so removing the range removes the line.
struct MarkerLine {
    MarkerKind kind = MarkerKind::Begin;
    std::size_t line = 0;
    ByteRange bytes;
    bool operator==(const MarkerLine &) const = default;
};

// A complete block: <<<<<<<, an optional |||||||, =======, then >>>>>>>.
struct MarkerRange {
    MarkerLine begin;
    std::optional<MarkerLine> base;
    MarkerLine separator;
    MarkerLine end;
    bool operator==(const MarkerRange &) const = default;
};

struct MarkerScan {
    std::vector<MarkerRange> blocks;
    // Marker lines outside any complete block, in text order.
    std::vector<MarkerLine> orphans;
    // Each block and each orphan line counts once.
    [[nodiscard]] std::size_t count() const noexcept { return blocks.size() + orphans.size(); }
    [[nodiscard]] std::optional<MarkerLine> first() const;
    bool operator==(const MarkerScan &) const = default;
};

// The only marker recognizer. A marker line starts with exactly seven of the
// same marker character, followed by whitespace or the end of the text, as
// Git writes them by default. Text elsewhere in a line is never a marker, and
// neither is a line inside one of the `content` ranges, which hold known
// source text. Blocks are assembled from the remaining marker lines.
[[nodiscard]] MarkerScan scanMarkers(std::string_view text, std::vector<ByteRange> content = {});

}
