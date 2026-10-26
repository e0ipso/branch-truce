#include "ConflictMarkers.h"

#include <algorithm>

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
};

void abandon(std::optional<OpenBlock> &open, std::vector<MarkerLine> &orphans)
{
    if (!open) return;
    orphans.push_back(open->begin);
    if (open->base) orphans.push_back(*open->base);
    if (open->separator) orphans.push_back(*open->separator);
    open.reset();
}
}

std::optional<MarkerLine> MarkerScan::first() const
{
    std::optional<MarkerLine> result;
    if (!blocks.empty()) result = blocks.front().begin;
    if (!orphans.empty() && (!result || orphans.front().line < result->line)) result = orphans.front();
    return result;
}

MarkerScan scanMarkers(std::string_view text)
{
    MarkerScan result;
    std::optional<OpenBlock> open;
    std::size_t number = 0;
    for (std::size_t begin = 0; begin < text.size(); ++number) {
        const auto newline = text.find('\n', begin);
        const auto end = newline == std::string_view::npos ? text.size() : newline + 1;
        const auto kind = markerKind(text.substr(begin, end - begin));
        if (kind) {
            const MarkerLine line{*kind, number, {begin, end}};
            switch (*kind) {
            case MarkerKind::Begin:
                abandon(open, result.orphans);
                open = OpenBlock{line, {}, {}};
                break;
            case MarkerKind::Base:
                if (open && !open->base && !open->separator) open->base = line;
                else result.orphans.push_back(line);
                break;
            case MarkerKind::Separator:
                if (open && !open->separator) open->separator = line;
                else result.orphans.push_back(line);
                break;
            case MarkerKind::End:
                if (open && open->separator) {
                    result.blocks.push_back({open->begin, open->base, *open->separator, line});
                    open.reset();
                } else {
                    abandon(open, result.orphans);
                    result.orphans.push_back(line);
                }
                break;
            }
        }
        begin = end;
    }
    abandon(open, result.orphans);
    std::sort(result.orphans.begin(), result.orphans.end(),
              [](const auto &left, const auto &right) { return left.line < right.line; });
    return result;
}

}
