// Exact line correspondence aligns two texts by their lines, keeping only matches every optimal alignment shares.
#include "ExactLineCorrespondence.h"
#include "ResourceLimits.h"
#include "Utf8.h"
#include "internal/LineAlignment.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

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

// Line counts of two texts within the index limit, and the lines they share
// at their start and end. The changed lines are the ones between those edges.
struct LineCounts {
    std::size_t source;
    std::size_t result;
    SharedEdges edges;
    [[nodiscard]] std::size_t changedSource() const noexcept { return source - edges.prefix - edges.suffix; }
    [[nodiscard]] std::size_t changedResult() const noexcept { return result - edges.prefix - edges.suffix; }
};

std::size_t saturated(std::uint64_t cells) noexcept
{
    return static_cast<std::size_t>(std::min<std::uint64_t>(cells, std::numeric_limits<std::size_t>::max()));
}

// Myers' work model is the changed lines times D. D is at least the
// difference of the changed-line counts, and at least one for texts that
// differ, so this is a lower bound on that work. Both counts are within
// kMaxIndexedLines, so the product fits in 64 bits.
std::uint64_t myersEstimate(const LineCounts &counts) noexcept
{
    const std::uint64_t source = counts.changedSource();
    const std::uint64_t result = counts.changedResult();
    const auto difference = source > result ? source - result : result - source;
    return (source + result) * std::max<std::uint64_t>(difference, 1);
}

// Myers searches the same grid as Hirschberg, by changed-line count D: the
// source and result lines an alignment skips. Every optimal alignment skips
// the fewest, D = sourceLines + resultLines - 2 * LCS. Rows, diagonals
// (column - row) and skip counts are signed 64-bit, because the two line
// counts together can come close to 2 * kMaxIndexedLines.
using Offset = std::int64_t;
constexpr Offset kUnreached = std::numeric_limits<Offset>::min();

Offset offsetOf(std::size_t size) noexcept { return static_cast<Offset>(size); }
std::size_t sizeOf(Offset offset) noexcept { return static_cast<std::size_t>(offset); }

// Work all of one comparison's Myers searches may spend: one unit for each
// diagonal a search extends and each line pair it compares. Under the D cap,
// D * D is at most kMaxAlignmentCells, and the searches extend about
// 1.25 * D * D diagonals. Long repetitive shared edges can add many compared
// pairs outside the changed lines, and the budget stops those searches.
struct WorkBudget {
    std::uint64_t left = 2 * std::uint64_t{kMaxAlignmentCells};
    bool spend(std::uint64_t cells) noexcept
    {
        if (cells > left) return false;
        left -= cells;
        return true;
    }
};

// Reach rows the searches reuse, so the recursion does not allocate them for
// each region again.
struct Workspace {
    std::vector<Offset> forward;
    std::vector<Offset> backward;
};

// One direction of a Myers search. After advance() allows d skips, reach(k)
// is the furthest row of diagonal k that a path from this direction's start
// corner gets to with at most d skips. A backward search reads both texts
// from their ends, so its row r is row source.size() - r of the forward grid
// and its diagonal k is forward diagonal (result.size() - source.size()) - k.
// The skip count only grows along a diagonal, so every earlier row of the
// diagonal is reached too. A diagonal of the other parity holds the reach of
// one skip fewer, and a diagonal not reached yet holds kUnreached.
class Frontier {
public:
    Frontier(std::span<const Line> source, std::span<const Line> result, bool backward, Offset most,
             std::vector<Offset> &reach)
        : source_(source), result_(result), backward_(backward), most_(most), reach_(reach)
    {
        reach_.assign(sizeOf(2 * most + 3), kUnreached);
    }

    [[nodiscard]] Offset reach(Offset diagonal) const noexcept
    {
        if (diagonal < -most_ - 1 || diagonal > most_ + 1) return kUnreached;
        return reach_[index(diagonal)];
    }

    // Allows one more skip. False when the budget runs out or `most` skips
    // are already allowed.
    bool advance(WorkBudget &budget)
    {
        if (skips_ == most_) return false;
        const auto skips = ++skips_;
        const auto rows = offsetOf(source_.size());
        const auto columns = offsetOf(result_.size());
        auto diagonal = std::max(-skips, -rows);
        if ((diagonal - skips) % 2 != 0) ++diagonal;
        for (; diagonal <= std::min(skips, columns); diagonal += 2) {
            Offset row = 0;
            if (skips > 0) {
                // Skipping a source line moves from diagonal + 1 to here, and
                // skipping a result line from diagonal - 1. Both can start at
                // any row up to the reach there, so the grid edge clamps them.
                const auto down = reach(diagonal + 1);
                const auto right = reach(diagonal - 1);
                row = down == kUnreached ? kUnreached : std::min(down + 1, rows);
                if (right != kUnreached) row = std::max(row, std::min(right, columns - diagonal));
                if (row == kUnreached) continue; // Never: a diagonal in range has a reached neighbour.
            }
            const auto start = row;
            while (row < rows && row + diagonal < columns && same(row, row + diagonal)) ++row;
            reach_[index(diagonal)] = row;
            if (!budget.spend(static_cast<std::uint64_t>(row - start) + 1)) return false;
        }
        return true;
    }

private:
    [[nodiscard]] std::size_t index(Offset diagonal) const noexcept { return sizeOf(diagonal + most_ + 1); }
    [[nodiscard]] bool same(Offset row, Offset column) const noexcept
    {
        const auto i = backward_ ? source_.size() - 1 - sizeOf(row) : sizeOf(row);
        const auto j = backward_ ? result_.size() - 1 - sizeOf(column) : sizeOf(column);
        return source_[i].token == result_[j].token;
    }

    std::span<const Line> source_;
    std::span<const Line> result_;
    bool backward_;
    Offset most_;
    Offset skips_ = -1;
    std::vector<Offset> &reach_;
};

// D for two texts, from forward and backward searches that meet in the
// middle: the first diagonal where their reaches overlap. nullopt once D must
// be over `cap` or the budget runs out.
std::optional<Offset> changedLines(std::span<const Line> source, std::span<const Line> result, Offset cap,
                                   Workspace &workspace, WorkBudget &budget)
{
    const auto rows = offsetOf(source.size());
    const auto columns = offsetOf(result.size());
    const auto delta = columns - rows;
    // D has the parity of delta. The searches meet after ceil(D / 2) forward
    // and floor(D / 2) backward skips, and D is at most rows + columns.
    const bool odd = delta % 2 != 0;
    const auto most = std::min(cap, rows + columns) / 2 + 1;
    Frontier forward(source, result, false, most, workspace.forward);
    Frontier backward(source, result, true, most, workspace.backward);
    const auto meet = [&](Offset diagonal) {
        const auto ahead = forward.reach(diagonal);
        const auto behind = backward.reach(delta - diagonal);
        return ahead != kUnreached && behind != kUnreached && ahead + behind >= rows;
    };
    for (Offset skips = 0;; ++skips) {
        if (odd && 2 * skips - 1 > cap) return std::nullopt;
        if (!forward.advance(budget)) return std::nullopt;
        if (odd)
            for (auto diagonal = -skips; diagonal <= skips; diagonal += 2)
                if (meet(diagonal)) return 2 * skips - 1;
        if (!odd && 2 * skips > cap) return std::nullopt;
        if (!backward.advance(budget)) return std::nullopt;
        if (!odd)
            for (auto diagonal = -skips; diagonal <= skips; diagonal += 2)
                if (meet(delta - diagonal)) return 2 * skips;
    }
}

// Source lines [top, bottom) against result lines [left, right).
struct Region {
    std::size_t top;
    std::size_t bottom;
    std::size_t left;
    std::size_t right;
};

struct Point {
    Offset row;
    Offset column;
};

// The skip from `from`, the extreme path's last point after h - 1 skips, to
// `to`, its first point after h.
struct Skip {
    Point from;
    Point to;
};

// Rows of one diagonal that optimal paths cross with the same skip count.
struct Rows {
    Offset low;
    Offset high;
};

// The extreme path's h-th skip, from a forward search that allowed h skips
// and a backward search that allowed D - h + 1.
//
// Every optimal path makes exactly D skips. After h of them it runs along one
// diagonal, and the points of that diagonal it can use are the rows where the
// forward reach after h skips and the backward reach after D - h overlap.
// The first path is the lowest-left optimal path: from each point it skips a
// source line when an optimal path allows it, else matches, else skips a
// result line. It is the path boundaryAlignment traces with last == false
// (the smallest split column at every level), and:
// - after every h skips it is on the lowest diagonal any optimal path uses;
// - between those diagonals at h - 1 and h, it skips a source line at the
//   first row that allows one, and a result line at the last row.
// The last path mirrors it: the highest diagonal after every h skips, a
// result-line skip at the first row and a source-line skip at the last row.
std::optional<Skip> extremeSkip(const Frontier &forward, const Frontier &backward, Offset rows, Offset columns,
                                Offset h, bool last)
{
    const auto delta = columns - rows;
    std::optional<std::pair<Offset, Rows>> before;
    std::optional<std::pair<Offset, Rows>> after;
    for (auto diagonal = std::max(-h, -rows); diagonal <= std::min(h, columns); ++diagonal) {
        // Diagonals of h's parity hold reaches after h forward and D - h
        // backward skips; the others after h - 1 and D - h + 1.
        const auto ahead = forward.reach(diagonal);
        const auto behind = backward.reach(delta - diagonal);
        if (ahead == kUnreached || behind == kUnreached || rows - behind > ahead) continue;
        auto &extreme = (diagonal - h) % 2 == 0 ? after : before;
        if (!extreme || last) extreme = {diagonal, {rows - behind, ahead}};
    }
    if (!before || !after) return std::nullopt;
    const auto [fromDiagonal, fromRows] = *before;
    const auto [toDiagonal, toRows] = *after;
    if (toDiagonal == fromDiagonal - 1) { // Skips source line r: (r, r + from) to (r + 1, r + from).
        const auto low = std::max(fromRows.low, toRows.low - 1);
        const auto high = std::min(fromRows.high, toRows.high - 1);
        if (low > high) return std::nullopt;
        const auto row = last ? high : low;
        return Skip{{row, row + fromDiagonal}, {row + 1, row + fromDiagonal}};
    }
    if (toDiagonal == fromDiagonal + 1) { // Skips result line r + from: (r, r + from) to (r, r + to).
        const auto low = std::max(fromRows.low, toRows.low);
        const auto high = std::min(fromRows.high, toRows.high);
        if (low > high) return std::nullopt;
        const auto row = last ? low : high;
        return Skip{{row, row + fromDiagonal}, {row, row + toDiagonal}};
    }
    return std::nullopt;
}

// Appends the matches of the first or last optimal path through `region`,
// whose changed-line count is `changes`. It splits at the path's middle skip,
// so each half has at most half the skips, and the extreme path of each half
// is that half of the whole one. Work is linear in the region's lines times
// its skips, and the reach rows are linear in its skips.
bool extremeMatches(std::span<const Line> source, std::span<const Line> result, Region region, Offset changes,
                    bool last, Workspace &workspace, WorkBudget &budget, std::vector<LineMatch> &matches)
{
    const auto rows = offsetOf(region.bottom - region.top);
    const auto columns = offsetOf(region.right - region.left);
    if (changes == rows + columns) return true; // Nothing matches.
    if (changes == 0) {
        for (std::size_t line = 0; line < region.bottom - region.top; ++line)
            matches.push_back({region.top + line, region.left + line});
        return true;
    }
    const auto h = (changes + 1) / 2;
    std::optional<Skip> skip;
    {
        const auto sourceLines = source.subspan(region.top, region.bottom - region.top);
        const auto resultLines = result.subspan(region.left, region.right - region.left);
        Frontier forward(sourceLines, resultLines, false, h, workspace.forward);
        Frontier backward(sourceLines, resultLines, true, changes - h + 1, workspace.backward);
        for (Offset step = 0; step <= h; ++step)
            if (!forward.advance(budget)) return false;
        for (Offset step = 0; step <= changes - h + 1; ++step)
            if (!backward.advance(budget)) return false;
        skip = extremeSkip(forward, backward, rows, columns, h, last);
    } // The halves reuse the workspace.
    // Unreachable for a correct D; failing leaves the pair to the fallback.
    if (!skip) return false;
    const Region before{region.top, region.top + sizeOf(skip->from.row), region.left,
                        region.left + sizeOf(skip->from.column)};
    const Region after{region.top + sizeOf(skip->to.row), region.bottom, region.left + sizeOf(skip->to.column),
                       region.right};
    return extremeMatches(source, result, before, h - 1, last, workspace, budget, matches)
        && extremeMatches(source, result, after, changes - h, last, workspace, budget, matches);
}

// Passages from the matches of the first and last optimal paths. A match both
// share is in every optimal alignment and is Equal. A gap is Ambiguous when
// the paths hold matches inside it that they do not share.
std::vector<Passage> passagesBetween(std::string_view source, std::string_view result, std::span<const Line> a,
                                     std::span<const Line> b, const std::vector<LineMatch> &first,
                                     const std::vector<LineMatch> &last)
{
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

std::vector<Passage> hirschbergAlignment(std::string_view source, std::string_view result, const LineCounts &counts)
{
    const auto band = optimalBand(counts.source, counts.result, counts.edges);
    std::map<std::string_view, std::size_t, std::less<>> tokens;
    const auto a = lines(source, tokens);
    const auto b = lines(result, tokens);
    std::vector<LineMatch> first;
    std::vector<LineMatch> last;
    boundaryAlignment(a, b, 0, 0, band, false, first);
    boundaryAlignment(a, b, 0, 0, band, true, last);
    return passagesBetween(source, result, a, b, first, last);
}

// Myers' work grows with the changed lines times D, so the changed lines set
// the cap on D. It reads only the line counts, so nothing is allocated for a
// pair over the cap's lower bound. D for the whole texts is D for their
// changed lines, but the extreme paths can slide into the shared edges, so the
// paths are found on the whole texts.
std::optional<std::vector<Passage>> myersAlignment(std::string_view source, std::string_view result,
                                                   const LineCounts &counts)
{
    const auto changedSource = counts.changedSource();
    const auto changedResult = counts.changedResult();
    const auto cap = offsetOf(kMaxAlignmentCells / std::max<std::size_t>(changedSource + changedResult, 1));
    if (std::abs(offsetOf(changedSource) - offsetOf(changedResult)) > cap) return std::nullopt;
    std::map<std::string_view, std::size_t, std::less<>> tokens;
    const auto a = lines(source, tokens);
    const auto b = lines(result, tokens);
    const std::span<const Line> sourceLines(a);
    const std::span<const Line> resultLines(b);
    Workspace workspace;
    WorkBudget budget;
    const auto changes = changedLines(sourceLines.subspan(counts.edges.prefix, changedSource),
                                      resultLines.subspan(counts.edges.prefix, changedResult), cap, workspace, budget);
    if (!changes) return std::nullopt;
    std::vector<LineMatch> first;
    std::vector<LineMatch> last;
    const Region whole{0, a.size(), 0, b.size()};
    if (!extremeMatches(a, b, whole, *changes, false, workspace, budget, first)
        || !extremeMatches(a, b, whole, *changes, true, workspace, budget, last))
        return std::nullopt;
    return passagesBetween(source, result, a, b, first, last);
}

std::vector<Passage> ambiguous(std::string_view source, std::string_view result)
{
    return {{{0, source.size()}, {0, result.size()}, PassageKind::Ambiguous}};
}

}

namespace detail {
std::vector<Passage> hirschbergPassages(std::string_view source, std::string_view result)
{
    const auto sourceLines = lineCount(source);
    const auto resultLines = lineCount(result);
    return hirschbergAlignment(source, result, {sourceLines, resultLines, sharedEdges(source, result)});
}

std::optional<std::vector<Passage>> myersPassages(std::string_view source, std::string_view result)
{
    const auto sourceLines = lineCount(source);
    const auto resultLines = lineCount(result);
    if (overIndexLimit(sourceLines, resultLines)) return std::nullopt;
    return myersAlignment(source, result, {sourceLines, resultLines, sharedEdges(source, result)});
}
}

// An admission estimate, never above the band cost of version 3: the band
// cost when Hirschberg's work is lower, else a lower bound on Myers' work.
// compare() caps its own Myers search, so this does not bound its work.
std::size_t ExactLineCorrespondence::cost(std::string_view source, std::string_view result) const
{
    if (source == result) return 0;
    const auto sourceLines = lineCount(source);
    const auto resultLines = lineCount(result);
    if (overIndexLimit(sourceLines, resultLines)) return std::numeric_limits<std::size_t>::max();
    const LineCounts counts{sourceLines, resultLines, sharedEdges(source, result)};
    return std::min(alignmentCost(sourceLines, resultLines, counts.edges), saturated(myersEstimate(counts)));
}

// Identical texts need no alignment. Otherwise Myers runs first; when it is
// over its cap, the banded Hirschberg runs if its band cost fits. Both find the
// same first and last optimal paths, so they give the same passages.
std::vector<Passage> ExactLineCorrespondence::compare(std::string_view source, std::string_view result) const
{
    requireComparable(source);
    requireComparable(result);
    if (source == result) return {{{0, source.size()}, {0, result.size()}, PassageKind::Equal}};
    const auto sourceLines = lineCount(source);
    const auto resultLines = lineCount(result);
    if (overIndexLimit(sourceLines, resultLines)) return ambiguous(source, result);
    const LineCounts counts{sourceLines, resultLines, sharedEdges(source, result)};
    if (auto passages = myersAlignment(source, result, counts)) return std::move(*passages);
    if (alignmentCost(sourceLines, resultLines, counts.edges) <= kMaxAlignmentCells)
        return hirschbergAlignment(source, result, counts);
    return ambiguous(source, result);
}

}
