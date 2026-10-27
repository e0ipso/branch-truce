#include "ConflictMarkers.h"

#include <algorithm>
#include <iterator>

namespace branch_truce {
namespace {
constexpr std::size_t markerSize = 7;

std::optional<MarkerKind> markerKind(std::string_view line)
{
    if (line.size() < markerSize) return std::nullopt;
    const char first = line.front();
    if (line.find_first_not_of(first) < markerSize) return std::nullopt;
    if (line.size() > markerSize) {
        const char next = line[markerSize];
        if (next != ' ' && next != '\t' && next != '\r' && next != '\n') return std::nullopt;
    }
    switch (first) {
    case '<': return MarkerKind::Begin;
    case '|': return MarkerKind::Base;
    case '=': return MarkerKind::Separator;
    case '>': return MarkerKind::End;
    default: return std::nullopt;
    }
}

// Lines of a block whose end has not been seen yet.
struct OpenBlock {
    MarkerLine begin;
    std::optional<MarkerLine> base;
    std::optional<MarkerLine> separator;
    // Where the separator line that closes the base region starts, once read.
    std::optional<std::size_t> baseEnd;
};

void abandon(std::optional<OpenBlock> &open, std::vector<MarkerLine> &orphans)
{
    if (!open) return;
    orphans.push_back(open->begin);
    if (open->base) orphans.push_back(*open->base);
    if (open->separator) orphans.push_back(*open->separator);
    open.reset();
}

// Merged into disjoint ranges, so one search finds the only candidate.
std::vector<ByteRange> disjoint(std::vector<ByteRange> ranges)
{
    std::sort(ranges.begin(), ranges.end(), [](const auto &a, const auto &b) { return a.begin < b.begin; });
    std::vector<ByteRange> result;
    for (const auto &range : ranges) {
        if (!result.empty() && range.begin <= result.back().end) result.back().end = std::max(result.back().end, range.end);
        else result.push_back(range);
    }
    return result;
}

bool covers(const std::vector<ByteRange> &known, ByteRange line)
{
    auto after = std::upper_bound(known.begin(), known.end(), line.begin,
                                  [](std::size_t offset, const auto &range) { return offset < range.begin; });
    return after != known.begin() && line.end <= std::prev(after)->end;
}
}

std::optional<MarkerLine> MarkerScan::first() const
{
    std::optional<MarkerLine> result;
    if (!blocks.empty()) result = blocks.front().begin;
    if (!orphans.empty() && (!result || orphans.front().line < result->line)) result = orphans.front();
    return result;
}

MarkerScan scanMarkers(std::string_view text, std::vector<ByteRange> content, std::vector<ByteRange> baseContent,
                       std::vector<ByteRange> baseUncertain, std::vector<ByteRange> rightContent)
{
    const auto known = disjoint(std::move(content));
    const auto earlier = disjoint(std::move(baseContent));
    const auto uncertain = disjoint(std::move(baseUncertain));
    const auto rightText = disjoint(std::move(rightContent));
    const auto lineAt = [&](std::size_t begin) {
        const auto newline = text.find('\n', begin);
        return ByteRange{begin, newline == std::string_view::npos ? text.size() : newline + 1};
    };
    // The marker kind of a line no known text covers. Earlier text counts only
    // in a base region.
    const auto markerAt = [&](ByteRange line, bool inBase) -> std::optional<MarkerKind> {
        if (covers(known, line) || (inBase && covers(earlier, line))) return std::nullopt;
        return markerKind(text.substr(line.begin, line.end - line.begin));
    };
    // Base text can end with a separator-like line that the earlier text could
    // pair with either of two identical lines, so neither is known content. Of
    // adjacent separator lines in a base region, each one the earlier text could
    // hold is base text when the right side region follows a later one: known
    // right-side text or the end marker. Otherwise the first one closes the base.
    const auto baseEnd = [&](ByteRange line) {
        auto last = line;
        for (auto next = lineAt(last.end); last.end < text.size() && covers(uncertain, last)
             && markerAt(next, true) == MarkerKind::Separator; next = lineAt(last.end))
            last = next;
        if (last == line || last.end == text.size()) return line.begin;
        const auto after = lineAt(last.end);
        return covers(rightText, after) || markerAt(after, false) == MarkerKind::End ? last.begin : line.begin;
    };
    MarkerScan result;
    std::optional<OpenBlock> open;
    std::size_t number = 0;
    for (std::size_t begin = 0; begin < text.size(); ++number) {
        const auto line = lineAt(begin);
        // Git's separator closes a base region, so earlier text counts only
        // between the base line and the first separator outside it.
        const bool inBase = open && open->base && !open->separator;
        auto kind = markerAt(line, inBase);
        if (inBase && kind == MarkerKind::Separator) {
            if (!open->baseEnd) open->baseEnd = baseEnd(line);
            if (line.begin < *open->baseEnd) kind.reset();
        }
        if (kind) {
            const MarkerLine marker{*kind, number, line};
            switch (*kind) {
            case MarkerKind::Begin:
                abandon(open, result.orphans);
                open = OpenBlock{marker, {}, {}, {}};
                break;
            case MarkerKind::Base:
                if (open && !open->base && !open->separator) open->base = marker;
                else result.orphans.push_back(marker);
                break;
            case MarkerKind::Separator:
                if (open && !open->separator) open->separator = marker;
                else result.orphans.push_back(marker);
                break;
            case MarkerKind::End:
                if (open && open->separator) {
                    result.blocks.push_back({open->begin, open->base, *open->separator, marker});
                    open.reset();
                } else {
                    abandon(open, result.orphans);
                    result.orphans.push_back(marker);
                }
                break;
            }
        }
        begin = line.end;
    }
    abandon(open, result.orphans);
    std::sort(result.orphans.begin(), result.orphans.end(),
              [](const auto &left, const auto &right) { return left.line < right.line; });
    return result;
}

}
