// Conflict marker parsing identifies unresolved marker blocks in the result text.
#include "ConflictMarkers.h"
#include "ByteRanges.h"
#include "ResourceLimits.h"

#include <algorithm>
#include <iterator>

namespace branch_truce {
namespace {
std::optional<MarkerKind> markerKind(std::string_view line, std::size_t size)
{
    if (line.size() < size) return std::nullopt;
    const char first = line.front();
    if (line.find_first_not_of(first) < size) return std::nullopt;
    if (line.size() > size) {
        const char next = line[size];
        if (next != ' ' && next != '\t' && next != '\n') return std::nullopt;
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

bool covers(const std::vector<ByteRange> &known, ByteRange line)
{
    auto after = std::upper_bound(known.begin(), known.end(), line.begin,
                                  [](std::size_t offset, const auto &range) { return offset < range.begin; });
    return after != known.begin() && line.end <= std::prev(after)->end;
}

// Whether `part` is consecutive whole lines of `source`. Git ends a region's
// last line with a newline even where the source's last line has none.
bool wholeLines(std::string_view source, std::string_view part, std::size_t &budget, bool &limited)
{
    if (part.empty()) return true;
    // Bound every repeated find, including highly repetitive near-matches.
    const auto width = part.size() + 2;
    if (source.size() > budget / width || part.size() > budget - source.size() * width) {
        limited = true;
        return false;
    }
    budget -= source.size() * width + part.size();
    const auto lineStart = [&](std::size_t at) { return at == 0 || source[at - 1] == '\n'; };
    for (auto at = source.find(part); at != std::string_view::npos; at = source.find(part, at + 1))
        if (lineStart(at)) return true;
    const auto stem = part.substr(0, part.size() - 1);
    return part.back() == '\n' && !source.ends_with('\n') && source.ends_with(stem)
        && lineStart(source.size() - stem.size());
}

// Classifies the lines of one text against what the scan knows about it.
class MarkerLexer {
public:
    MarkerLexer(std::string_view text, MarkerContext context)
        : text_(text),
          size_(context.markerSize),
          known_(mergeRanges(std::move(context.known))),
          earlier_(mergeRanges(std::move(context.earlier))),
          uncertain_(mergeRanges(std::move(context.uncertain))),
          right_(mergeRanges(std::move(context.right))),
          sources_(std::move(context.sources)),
          budget_(context.analysisBudget)
    {}

    [[nodiscard]] std::size_t size() const noexcept { return text_.size(); }
    [[nodiscard]] ByteRange lineAt(std::size_t begin) const { return branch_truce::lineAt(text_, begin); }

    // The marker kind of a line no known text covers. Earlier text counts
    // only in a base region.
    [[nodiscard]] std::optional<MarkerKind> markerAt(ByteRange line, bool inBase) const
    {
        if (covers(known_, line) || (inBase && covers(earlier_, line))) return std::nullopt;
        return markerKind(slice(text_, line), size_);
    }

    // Git's separator and the right side's separator lines after it, from the
    // first separator line in the base region after the ||||||| line `base`.
    [[nodiscard]] ByteRange separatorLines(ByteRange base, ByteRange first)
    {
        std::vector<ByteRange> lines{first};
        for (auto next = lineAt(first.end); lines.back().end < size() && markerAt(next, true) == MarkerKind::Separator;
             next = lineAt(next.end))
            lines.push_back(next);
        if (const auto git = copiedAround(base, lines)) return ByteRange{lines[*git].begin, lines.back().end};
        // Unproved separator-like lines remain markers on admission failure.
        if (limited_) return first;
        const auto begin = baseEnd(first);
        return ByteRange{begin, lineAt(begin).end};
    }

private:
    // Base text can end with a separator-like line that the earlier text
    // could pair with either of two identical lines, so neither is known
    // content. Of adjacent separator lines in a base region, each one the
    // earlier text could hold is base text when the right side region follows
    // a later one: known right-side text or the end marker. Otherwise the
    // first one closes the base.
    [[nodiscard]] std::size_t baseEnd(ByteRange first) const
    {
        auto last = first;
        for (auto next = lineAt(last.end);
             last.end < size() && covers(uncertain_, last) && markerAt(next, true) == MarkerKind::Separator;
             next = lineAt(last.end))
            last = next;
        if (last == first || last.end == size()) return first.begin;
        const auto after = lineAt(last.end);
        return covers(right_, after) || markerAt(after, false) == MarkerKind::End ? last.begin : first.begin;
    }

    // Where the first end marker line at or after `offset` starts. One
    // forward pass finds them all, so blocks never rescan the rest of the text.
    [[nodiscard]] std::optional<std::size_t> endMarkerFrom(std::size_t offset)
    {
        if (!ends_) {
            ends_.emplace();
            for (std::size_t begin = 0; begin < size();) {
                const auto next = lineAt(begin);
                if (markerAt(next, false) == MarkerKind::End) ends_->push_back(next.begin);
                begin = next.end;
            }
        }
        const auto found = std::lower_bound(ends_->begin(), ends_->end(), offset);
        return found == ends_->end() ? std::nullopt : std::optional<std::size_t>{*found};
    }

    // Git copies the earlier version's text into the base region and the
    // right side's text into the right side region whole. So of adjacent
    // separator lines, Git's can only be one that leaves a known base region
    // before it, or whole earlier lines between the ||||||| line and it and
    // whole right side lines between it and the end marker. The index of the
    // only such line, if there is one.
    [[nodiscard]] std::optional<std::size_t> copiedAround(ByteRange base, const std::vector<ByteRange> &lines)
    {
        if (!sources_ || lines.size() < 2) return std::nullopt;
        const auto end = endMarkerFrom(lines.back().end);
        if (!end) return std::nullopt;
        std::optional<std::size_t> found;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const auto matches = copied(*sources_, base, lines[i], *end);
            if (limited_) return std::nullopt;
            if (!matches) continue;
            if (found) return std::nullopt;
            found = i;
        }
        return found;
    }

    [[nodiscard]] bool copied(const RegionSources &sources, ByteRange base, ByteRange separator, std::size_t end)
    {
        const auto region = slice(text_, {base.begin, separator.begin});
        for (const auto known : sources.bases) {
            const auto cost = std::min(known.size(), region.size()) + 1;
            if (cost > budget_) {
                limited_ = true;
                return false;
            }
            budget_ -= cost;
            if (known == region) return true;
        }
        return wholeLines(sources.earlier, slice(text_, {base.end, separator.begin}), budget_, limited_)
            && wholeLines(sources.right, slice(text_, {separator.end, end}), budget_, limited_);
    }

    std::string_view text_;
    std::size_t size_;
    std::vector<ByteRange> known_;
    std::vector<ByteRange> earlier_;
    std::vector<ByteRange> uncertain_;
    std::vector<ByteRange> right_;
    std::optional<RegionSources> sources_;
    std::size_t budget_;
    bool limited_ = false;
    std::optional<std::vector<std::size_t>> ends_;
};

}

std::optional<std::size_t> conflictMarkerSize(std::string_view attribute) noexcept
{
    std::size_t at = 0;
    while (at < attribute.size() && (attribute[at] == ' ' || (attribute[at] >= '\t' && attribute[at] <= '\r'))) ++at;
    bool negative = false;
    if (at < attribute.size() && (attribute[at] == '+' || attribute[at] == '-')) negative = attribute[at++] == '-';
    std::size_t size = 0;
    for (; at < attribute.size() && attribute[at] >= '0' && attribute[at] <= '9'; ++at) {
        size = size * 10 + static_cast<std::size_t>(attribute[at] - '0');
        if (size > kMaxConflictMarkerSize) return negative ? std::optional{kDefaultConflictMarkerSize} : std::nullopt;
    }
    return negative || size == 0 ? kDefaultConflictMarkerSize : size;
}

std::optional<MarkerLine> MarkerScan::first() const
{
    std::optional<MarkerLine> result;
    if (!blocks.empty()) result = blocks.front().begin;
    if (!orphans.empty() && (!result || orphans.front().line < result->line)) result = orphans.front();
    return result;
}

MarkerScan scanMarkers(std::string_view text, MarkerContext context)
{
    MarkerLexer lexer(text, std::move(context));
    MarkerScan result;
    std::optional<OpenBlock> open;
    // Incomplete blocks retain each marker as an orphan.
    const auto abandon = [&] {
        if (!open) return;
        result.orphans.push_back(open->begin);
        if (open->base) result.orphans.push_back(*open->base);
        if (open->separator) result.orphans.push_back(*open->separator);
        open.reset();
    };
    std::size_t number = 0;
    for (std::size_t begin = 0; begin < text.size(); ++number) {
        const auto line = lexer.lineAt(begin);
        begin = line.end;
        // Git's separator closes a base region, so earlier text counts only
        // between the base line and the first separator outside it.
        const bool inBase = open && open->base && !open->separator;
        auto kind = lexer.markerAt(line, inBase);
        if (inBase && kind == MarkerKind::Separator && !open->separatorLines)
            open->separatorLines = lexer.separatorLines(open->base->bytes, line);
        // Separator lines before Git's are base text, and the ones after it
        // that separatorLines found are right side text.
        if (open && open->separatorLines && kind == MarkerKind::Separator && line.begin < open->separatorLines->end
            && line.begin != open->separatorLines->begin)
            kind.reset();
        if (!kind) continue;
        const MarkerLine marker{*kind, number, line};
        switch (*kind) {
        case MarkerKind::Begin:
            abandon();
            open = OpenBlock{marker, {}, {}, {}};
            break;
        case MarkerKind::Base:
            if (open && !open->base && !open->separator)
                open->base = marker;
            else
                result.orphans.push_back(marker);
            break;
        case MarkerKind::Separator:
            if (open && !open->separator)
                open->separator = marker;
            else
                result.orphans.push_back(marker);
            break;
        case MarkerKind::End:
            if (open && open->separator) {
                result.blocks.push_back({open->begin, open->base, *open->separator, marker});
                open.reset();
            } else {
                abandon();
                result.orphans.push_back(marker);
            }
            break;
        }
    }
    abandon();
    std::sort(result.orphans.begin(), result.orphans.end(),
              [](const auto &left, const auto &right) { return left.line < right.line; });
    return result;
}

}
