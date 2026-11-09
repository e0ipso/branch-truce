// Byte ranges address text by half-open UTF-8 byte offsets.
#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <vector>

namespace branch_truce {

// A half-open range of UTF-8 byte offsets. An empty range is an insertion site.
struct ByteRange {
    std::size_t begin = 0;
    std::size_t end = 0;
    bool operator==(const ByteRange &) const = default;
};

// The text a range covers. The range must lie inside the text.
[[nodiscard]] inline std::string_view slice(std::string_view text, ByteRange range) noexcept
{
    return text.substr(range.begin, range.end - range.begin);
}

// Whether `inner` lies inside `outer`. An empty `inner` at either edge does.
[[nodiscard]] constexpr bool contains(ByteRange outer, ByteRange inner) noexcept
{
    return outer.begin <= inner.begin && inner.end <= outer.end;
}

// Whether two ranges share a byte. An empty range shares none, so it overlaps
// nothing, not even an equal empty range.
[[nodiscard]] constexpr bool overlaps(ByteRange a, ByteRange b) noexcept { return a.begin < b.end && b.begin < a.end; }

// The ranges in text order, with overlapping or touching ones joined.
[[nodiscard]] inline std::vector<ByteRange> mergeRanges(std::vector<ByteRange> ranges)
{
    std::sort(ranges.begin(), ranges.end(),
              [](const auto &a, const auto &b) { return a.begin != b.begin ? a.begin < b.begin : a.end < b.end; });
    std::vector<ByteRange> result;
    result.reserve(ranges.size());
    for (const auto &range : ranges) {
        if (!result.empty() && range.begin <= result.back().end)
            result.back().end = std::max(result.back().end, range.end);
        else
            result.push_back(range);
    }
    return result;
}

// The line that starts at `begin`, including its newline, or up to the end of
// the text when the last line has none.
[[nodiscard]] inline ByteRange lineAt(std::string_view text, std::size_t begin) noexcept
{
    const auto newline = text.find('\n', begin);
    return {begin, newline == std::string_view::npos ? text.size() : newline + 1};
}

// Where each line of the text starts, in order. An empty text has no lines.
[[nodiscard]] inline std::vector<std::size_t> lineStarts(std::string_view text)
{
    std::vector<std::size_t> starts;
    for (std::size_t begin = 0; begin < text.size(); begin = lineAt(text, begin).end) starts.push_back(begin);
    return starts;
}

}
