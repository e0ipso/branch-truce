// Conflict marker parsing identifies unresolved marker blocks in the result text.
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

// Where diff3 block text comes from: the texts whose lines Git copies into
// base regions and right side regions, and base regions an earlier scan found,
// each from its ||||||| line up to Git's separator.
struct RegionSources {
    std::string_view earlier;
    std::string_view right;
    std::vector<std::string_view> bases;
};

// Git writes markers of seven characters unless the conflict-marker-size
// attribute names another length.
inline constexpr std::size_t kDefaultConflictMarkerSize = 7;

// The marker length Git uses for a conflict-marker-size attribute value. Git
// reads the value as a leading decimal integer and falls back to the default
// for anything that is not positive, including "set" and "unset". A length
// over kMaxConflictMarkerSize is unsupported.
[[nodiscard]] std::optional<std::size_t> conflictMarkerSize(std::string_view attribute) noexcept;

// What a scan knows about the text besides its lines. Every range list may be
// unsorted and may overlap.
struct MarkerContext {
    std::size_t markerSize = kDefaultConflictMarkerSize;
    // Known source text: a line inside one of these is never a marker.
    std::vector<ByteRange> known = {};
    // Known earlier-version text, which counts only inside a diff3 base region.
    std::vector<ByteRange> earlier = {};
    // Where earlier-version text may lie in a base region.
    std::vector<ByteRange> uncertain = {};
    // Known right-side text.
    std::vector<ByteRange> right = {};
    std::optional<RegionSources> sources = {};
};

// The only marker recognizer. A marker line starts with exactly markerSize of
// the same marker character, followed by a space, a tab, a newline or the end
// of the text. Text elsewhere in a line is never a marker, and neither is a
// line inside one of the `known` ranges. Inside a diff3 base region, after its
// ||||||| line and before its separator, a line inside one of the `earlier`
// ranges is content too. So is a separator line inside one of the `uncertain`
// ranges when only separator lines follow it up to the start of the right side
// region: text inside one of the `right` ranges, or the end marker. The last
// of those separator lines is Git's.
//
// With `sources`, Git's separator among several adjacent separator lines in a
// base region is the only one that leaves one of the `bases` before it, or
// consecutive whole lines of `earlier` between the ||||||| line and it and
// consecutive whole lines of `right` between it and the end marker. Git copies
// both regions from those texts, so no other line can be Git's. The separator
// lines before it are base text and those after it right side text. Without
// exactly one such line the rule above applies. Blocks are assembled from the
// remaining marker lines.
[[nodiscard]] MarkerScan scanMarkers(std::string_view text, MarkerContext context = {});

}
