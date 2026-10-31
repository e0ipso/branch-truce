// Conflict marker parsing identifies unresolved marker blocks in the result text.
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
    // Once read, from where the separator line that closes the base region
    // starts to where the right side's separator lines after it end.
    std::optional<ByteRange> separatorLines;
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

// Whether `part` is consecutive whole lines of `source`. Git ends a region's
// last line with a newline even where the source's last line has none.
bool wholeLines(std::string_view source, std::string_view part)
{
    if (part.empty()) return true;
    const auto lineStart = [&](std::size_t at) { return at == 0 || source[at - 1] == '\n'; };
    for (auto at = source.find(part); at != std::string_view::npos; at = source.find(part, at + 1))
        if (lineStart(at)) return true;
    const auto stem = part.substr(0, part.size() - 1);
    return part.back() == '\n' && !source.ends_with('\n') && source.ends_with(stem)
        && lineStart(source.size() - stem.size());
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
                       std::vector<ByteRange> baseUncertain, std::vector<ByteRange> rightContent,
                       std::optional<RegionSources> sources)
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
    // Git copies the earlier version's text into the base region and the right
    // side's text into the right side region whole. So of adjacent separator
    // lines, Git's can only be one that leaves a known base region before it,
    // or whole earlier lines between the ||||||| line and it and whole right
    // side lines between it and the end marker. The index of the only such
    // line, if there is one.
    const auto copiedAround = [&](ByteRange base, const std::vector<ByteRange> &lines) {
        std::optional<std::size_t> found;
        if (!sources || lines.size() < 2) return found;
        std::optional<std::size_t> end;
        for (auto next = lineAt(lines.back().end); next.begin < text.size() && !end; next = lineAt(next.end))
            if (markerAt(next, false) == MarkerKind::End) end = next.begin;
        if (!end) return found;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const auto region = text.substr(base.begin, lines[i].begin - base.begin);
            if (std::find(sources->bases.begin(), sources->bases.end(), region) == sources->bases.end()
                && (!wholeLines(sources->earlier, text.substr(base.end, lines[i].begin - base.end))
                    || !wholeLines(sources->right, text.substr(lines[i].end, *end - lines[i].end)))) continue;
            if (found) return std::optional<std::size_t>{};
            found = i;
        }
        return found;
    };
    // Git's separator and the right side's separator lines after it, from the
    // first separator line in the base region after the ||||||| line `base`.
    const auto separatorLines = [&](ByteRange base, ByteRange line) {
        std::vector<ByteRange> lines{line};
        for (auto next = lineAt(line.end); lines.back().end < text.size()
             && markerAt(next, true) == MarkerKind::Separator; next = lineAt(next.end))
            lines.push_back(next);
        if (const auto git = copiedAround(base, lines)) return ByteRange{lines[*git].begin, lines.back().end};
        const auto begin = baseEnd(line);
        return ByteRange{begin, lineAt(begin).end};
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
        if (inBase && kind == MarkerKind::Separator && !open->separatorLines)
            open->separatorLines = separatorLines(open->base->bytes, line);
        // Separator lines before Git's are base text, and the ones after it
        // that separatorLines found are right side text.
        if (open && open->separatorLines && kind == MarkerKind::Separator && line.begin < open->separatorLines->end
            && line.begin != open->separatorLines->begin)
            kind.reset();
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
