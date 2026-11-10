// Exact line correspondence aligns two texts by their lines, keeping only matches every optimal alignment shares.
#include "ExactLineCorrespondence.h"
#include "ResourceLimits.h"
#include "Utf8.h"

#include <algorithm>
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

std::vector<std::size_t> lcsLengths(std::span<const Line> source, std::span<const Line> result, bool reverse)
{
    std::vector<std::size_t> row(result.size() + 1);
    for (std::size_t i = 0; i < source.size(); ++i) {
        const auto token = source[reverse ? source.size() - 1 - i : i].token;
        std::size_t diagonal = 0;
        for (std::size_t j = 0; j < result.size(); ++j) {
            const auto previous = row[j + 1];
            row[j + 1] =
                token == result[reverse ? result.size() - 1 - j : j].token ? diagonal + 1 : std::max(row[j], previous);
            diagonal = previous;
        }
    }
    return row;
}

// Hirschberg's divide and conquer keeps only two length rows. Choosing the
// first/last optimal crossing at every split traces the two boundary paths of
// the optimal-alignment graph, including skips at equal lines. A match shared
// by both boundaries is shared by every optimal alignment.
void boundaryAlignment(std::span<const Line> source, std::span<const Line> result, std::size_t sourceOffset,
                       std::size_t resultOffset, bool last, std::vector<LineMatch> &matches)
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
        const auto forward = lcsLengths(source.first(middle), result, false);
        const auto backward = lcsLengths(source.subspan(middle), result, true);
        std::size_t best = 0;
        for (std::size_t j = 0; j <= result.size(); ++j) {
            const auto length = forward[j] + backward[result.size() - j];
            if (length > best || (last && length == best)) {
                best = length;
                split = j;
            }
        }
    } // Release the rows before descending, keeping workspace linear.
    boundaryAlignment(source.first(middle), result.first(split), sourceOffset, resultOffset, last, matches);
    boundaryAlignment(source.subspan(middle), result.subspan(split), sourceOffset + middle, resultOffset + split, last,
                      matches);
}

}

std::vector<Passage> ExactLineCorrespondence::compare(std::string_view source, std::string_view result) const
{
    requireComparable(source);
    requireComparable(result);
    if (source == result) return {{{0, source.size()}, {0, result.size()}, PassageKind::Equal}};
    std::map<std::string_view, std::size_t, std::less<>> tokens;
    const auto a = lines(source, tokens);
    const auto b = lines(result, tokens);
    // Over the budget, no alignment is verified, which is never a guess.
    if (!a.empty() && b.size() > kMaxAlignmentCells / a.size())
        return {{{0, source.size()}, {0, result.size()}, PassageKind::Ambiguous}};
    std::vector<LineMatch> first;
    std::vector<LineMatch> last;
    boundaryAlignment(a, b, 0, 0, false, first);
    boundaryAlignment(a, b, 0, 0, true, last);
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
