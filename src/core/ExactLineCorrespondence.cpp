// Exact line correspondence aligns two texts by their lines, keeping only matches every optimal alignment shares.
#include "ExactLineCorrespondence.h"
#include "ResourceLimits.h"
#include "Utf8.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>

namespace branch_truce {
namespace {
struct Line {
    std::size_t begin;
    std::size_t end;
    std::size_t token;
};

std::vector<Line> lines(std::string_view text, std::map<std::string_view, std::size_t, std::less<>> &tokens)
{
    std::vector<Line> result;
    for (std::size_t begin = 0; begin < text.size();) {
        const auto range = lineAt(text, begin);
        const auto [entry, inserted] = tokens.try_emplace(slice(text, range), tokens.size());
        (void)inserted;
        result.push_back({range.begin, range.end, entry->second});
        begin = range.end;
    }
    return result;
}

struct LineMatch {
    std::size_t source;
    std::size_t result;
    bool operator==(const LineMatch &) const = default;
};

using Length = std::ptrdiff_t;
// A grid point no optimal alignment can reach. Two of them still add up
// without overflow.
constexpr Length kUnreachable = std::numeric_limits<Length>::min() / 4;

std::ptrdiff_t signedSize(std::size_t size) noexcept { return static_cast<std::ptrdiff_t>(size); }

// Diagonal offsets, result line index minus source line index, that a grid
// point may have. Points outside the band are unreachable.
struct Band {
    std::ptrdiff_t low;
    std::ptrdiff_t high;
};

// LCS lengths from this call's origin to each point of the last row, in the
// given direction. `band` bounds q - p for each point (p, q) in that direction.
// Each row visits only its band, so the work is rows times band width.
std::vector<Length> lcsLengths(std::span<const Line> source, std::span<const Line> result, bool reverse, Band band)
{
    const auto rows = signedSize(source.size());
    const auto columns = signedSize(result.size());
    const auto index = [](std::ptrdiff_t at) { return static_cast<std::size_t>(at); };
    // A column the band has not reached yet holds kUnreachable.
    std::vector<Length> row(result.size() + 1, kUnreachable);
    for (auto q = std::max<std::ptrdiff_t>(0, band.low); q <= std::min(columns, band.high); ++q) row[index(q)] = 0;
    for (std::ptrdiff_t p = 1; p <= rows; ++p) {
        const auto first = std::max<std::ptrdiff_t>(0, p + band.low);
        const auto last = std::min(columns, p + band.high);
        const auto token = source[index(reverse ? rows - p : p - 1)].token;
        // (p - 1, first - 1) is in the previous row's band whenever first > 0.
        Length diagonal = first > 0 ? row[index(first - 1)] : kUnreachable;
        Length left = kUnreachable; // (p, first - 1) is outside this row's band.
        for (auto q = first; q <= last; ++q) {
            const auto up = row[index(q)];
            auto value = std::max(up, left);
            if (q > 0 && diagonal != kUnreachable && token == result[index(reverse ? columns - q : q - 1)].token)
                value = std::max(value, diagonal + 1);
            diagonal = up;
            row[index(q)] = value;
            left = value;
        }
    }
    // Columns the band has passed hold earlier rows' lengths.
    std::fill_n(row.begin(), index(std::clamp<std::ptrdiff_t>(rows + band.low, 0, columns + 1)), kUnreachable);
    return row;
}

// Hirschberg's divide and conquer keeps only two length rows. Choosing the
// first/last optimal crossing at every split traces the two boundary paths of
// the optimal-alignment graph, including skips at equal lines. A match shared
// by both boundaries is shared by every optimal alignment. Every optimal path
// stays inside `band`, given as absolute diagonal offsets, so restricting the
// length rows to it changes no crossing.
void boundaryAlignment(std::span<const Line> source, std::span<const Line> result, std::size_t sourceOffset,
                       std::size_t resultOffset, Band band, bool last, std::vector<LineMatch> &matches)
{
    if (source.empty() || result.empty()) return;
    if (source.size() == 1) {
        for (std::size_t offset = 0; offset < result.size(); ++offset) {
            const auto j = last ? result.size() - 1 - offset : offset;
            if (source.front().token == result[j].token) {
                matches.push_back({sourceOffset, resultOffset + j});
                return;
            }
        }
        return;
    }
    const auto middle = source.size() / 2;
    std::size_t split = 0;
    {
        // Diagonal offsets of this call's first and last corners.
        const auto origin = signedSize(resultOffset) - signedSize(sourceOffset);
        const auto end = origin + signedSize(result.size()) - signedSize(source.size());
        const auto forward = lcsLengths(source.first(middle), result, false, {band.low - origin, band.high - origin});
        const auto backward = lcsLengths(source.subspan(middle), result, true, {end - band.high, end - band.low});
        Length best = kUnreachable;
        for (std::size_t j = 0; j <= result.size(); ++j) {
            const auto length = forward[j] + backward[result.size() - j];
            if (length > best || (last && length == best)) {
                best = length;
                split = j;
            }
        }
    } // Release the rows before descending, keeping workspace linear.
    boundaryAlignment(source.first(middle), result.first(split), sourceOffset, resultOffset, band, last, matches);
    boundaryAlignment(source.subspan(middle), result.subspan(split), sourceOffset + middle, resultOffset + split, band,
                      last, matches);
}

// Lines both texts share at their start, and then at their end. Together they
// are one common subsequence, so their count is a lower bound for the LCS.
struct SharedEdges {
    std::size_t prefix = 0;
    std::size_t suffix = 0;
};

// Where the last line before `end` starts.
std::size_t lastLineBegin(std::string_view text, std::size_t end) noexcept
{
    const auto newline = end < 2 ? std::string_view::npos : text.rfind('\n', end - 2);
    return newline == std::string_view::npos ? 0 : newline + 1;
}

SharedEdges sharedEdges(std::string_view source, std::string_view result) noexcept
{
    SharedEdges edges;
    std::size_t begin = 0;
    while (begin < source.size() && begin < result.size()) {
        const auto line = lineAt(source, begin);
        if (slice(source, line) != slice(result, lineAt(result, begin))) break;
        ++edges.prefix;
        begin = line.end;
    }
    auto sourceEnd = source.size();
    auto resultEnd = result.size();
    while (sourceEnd > begin && resultEnd > begin) {
        const auto sourceLine = lastLineBegin(source, sourceEnd);
        const auto resultLine = lastLineBegin(result, resultEnd);
        if (source.substr(sourceLine, sourceEnd - sourceLine) != result.substr(resultLine, resultEnd - resultLine))
            break;
        ++edges.suffix;
        sourceEnd = sourceLine;
        resultEnd = resultLine;
    }
    return edges;
}

// An optimal alignment of n and m lines with LCS length L skips n - L source
// lines and m - L result lines, and only a skip moves its diagonal offset, so
// the offset stays within [-(n - L), m - L]. The shared edges bound L from
// below, which gives the band.
Band optimalBand(std::size_t sourceLines, std::size_t resultLines, SharedEdges edges) noexcept
{
    const auto shared = edges.prefix + edges.suffix;
    return {-signedSize(sourceLines - shared), signedSize(resultLines - shared)};
}

// Cells the boundary passes visit: at most every line pair, and at most the
// band width per source line on each level of the recursion.
std::size_t alignmentCost(std::size_t sourceLines, std::size_t resultLines, SharedEdges edges) noexcept
{
    const auto band = optimalBand(sourceLines, resultLines, edges);
    const auto width = std::min<std::uint64_t>(static_cast<std::uint64_t>(band.high - band.low) + 1, resultLines + 1);
    const auto depth = static_cast<std::uint64_t>(std::bit_width(std::uint64_t{sourceLines}));
    const auto banded = std::uint64_t{sourceLines} * width * depth;
    const auto cells = std::min(std::uint64_t{sourceLines} * resultLines, banded);
    return static_cast<std::size_t>(std::min<std::uint64_t>(cells, std::numeric_limits<std::size_t>::max()));
}

bool overIndexLimit(std::size_t sourceLines, std::size_t resultLines) noexcept
{
    return sourceLines > kMaxIndexedLines || resultLines > kMaxIndexedLines;
}

}

std::size_t ExactLineCorrespondence::cost(std::string_view source, std::string_view result) const
{
    if (source == result) return 0;
    const auto sourceLines = lineCount(source);
    const auto resultLines = lineCount(result);
    if (overIndexLimit(sourceLines, resultLines)) return std::numeric_limits<std::size_t>::max();
    return alignmentCost(sourceLines, resultLines, sharedEdges(source, result));
}

std::vector<Passage> ExactLineCorrespondence::compare(std::string_view source, std::string_view result) const
{
    requireComparable(source);
    requireComparable(result);
    if (source == result) return {{{0, source.size()}, {0, result.size()}, PassageKind::Equal}};
    const auto sourceLines = lineCount(source);
    const auto resultLines = lineCount(result);
    const auto ambiguous = [&] {
        return std::vector<Passage>{{{0, source.size()}, {0, result.size()}, PassageKind::Ambiguous}};
    };
    if (overIndexLimit(sourceLines, resultLines)) return ambiguous();
    const auto edges = sharedEdges(source, result);
    if (alignmentCost(sourceLines, resultLines, edges) > kMaxAlignmentCells) return ambiguous();
    const auto band = optimalBand(sourceLines, resultLines, edges);
    std::map<std::string_view, std::size_t, std::less<>> tokens;
    const auto a = lines(source, tokens);
    const auto b = lines(result, tokens);
    std::vector<LineMatch> first;
    std::vector<LineMatch> last;
    boundaryAlignment(a, b, 0, 0, band, false, first);
    boundaryAlignment(a, b, 0, 0, band, true, last);
    std::vector<Passage> passages;
    const auto emit = [&](Passage passage) {
        if (!passages.empty() && passage.kind == PassageKind::Equal && passages.back().kind == PassageKind::Equal
            && passages.back().source.end == passage.source.begin
            && passages.back().result.end == passage.result.begin) {
            passages.back().source.end = passage.source.end;
            passages.back().result.end = passage.result.end;
        } else
            passages.push_back(passage);
    };
    const auto sourceOffset = [&](std::size_t i) { return i == a.size() ? source.size() : a[i].begin; };
    const auto resultOffset = [&](std::size_t j) { return j == b.size() ? result.size() : b[j].begin; };
    std::size_t sourceStart = 0;
    std::size_t resultStart = 0;
    const auto gap = [&](std::size_t sourceEnd, std::size_t resultEnd, bool ambiguous) {
        if (sourceStart == sourceEnd && resultStart == resultEnd) return;
        const auto kind = ambiguous    ? PassageKind::Ambiguous
            : sourceStart == sourceEnd ? PassageKind::ResultOnly
            : resultStart == resultEnd ? PassageKind::SourceOnly
                                       : PassageKind::Changed;
        emit({{sourceOffset(sourceStart), sourceOffset(sourceEnd)},
              {resultOffset(resultStart), resultOffset(resultEnd)},
              kind});
    };
    std::size_t nextMatch = 0;
    for (std::size_t k = 0; k < first.size(); ++k) {
        if (first[k] != last[k]) continue;
        const auto [i, j] = first[k];
        gap(i, j, nextMatch != k);
        emit({{a[i].begin, a[i].end}, {b[j].begin, b[j].end}, PassageKind::Equal});
        sourceStart = i + 1;
        resultStart = j + 1;
        nextMatch = k + 1;
    }
    gap(a.size(), b.size(), nextMatch != first.size());
    return passages;
}

}
