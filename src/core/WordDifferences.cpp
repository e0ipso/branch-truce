#include "WordDifferences.h"
#include "EditingCommands.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace branch_truce {
namespace {
struct Token {
    ByteRange range;
    // Newlines and over-long lines are compared but never reported.
    bool reported = true;
};

bool wordByte(unsigned char c)
{
    return c >= 0x80 || c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool blank(unsigned char c)
{
    return c == ' ' || c == '\t';
}

std::vector<Token> tokens(std::string_view text)
{
    std::vector<Token> result;
    std::size_t at = 0;
    while (at < text.size()) {
        const auto lineEnd = std::min(text.find('\n', at), text.size());
        if (lineEnd - at > kMaxWordDifferenceLineBytes) {
            result.push_back({{at, lineEnd}, false});
            at = lineEnd;
        }
        while (at < lineEnd) {
            const auto c = static_cast<unsigned char>(text[at]);
            auto end = at + 1;
            if (wordByte(c))
                while (end < lineEnd && wordByte(static_cast<unsigned char>(text[end]))) ++end;
            else if (blank(c))
                while (end < lineEnd && blank(static_cast<unsigned char>(text[end]))) ++end;
            result.push_back({{at, end}, true});
            at = end;
        }
        if (at < text.size()) {
            result.push_back({{at, at + 1}, false});
            ++at;
        }
    }
    return result;
}

std::string_view view(std::string_view text, ByteRange range)
{
    return text.substr(range.begin, range.end - range.begin);
}

// Adjacent unmatched, reported tokens as ranges, trimmed of blanks at the edges.
std::vector<ByteRange> ranges(std::string_view text, const std::vector<Token> &all, const std::vector<bool> &matched)
{
    std::vector<ByteRange> merged;
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (matched[i] || !all[i].reported) continue;
        if (!merged.empty() && merged.back().end == all[i].range.begin) merged.back().end = all[i].range.end;
        else merged.push_back(all[i].range);
    }
    for (auto &range : merged) {
        auto begin = range.begin;
        auto end = range.end;
        while (begin < end && blank(static_cast<unsigned char>(text[begin]))) ++begin;
        while (end > begin && blank(static_cast<unsigned char>(text[end - 1]))) --end;
        if (begin < end) range = {begin, end};
    }
    return merged;
}
}

WordDifferences wordDifferences(std::string_view left, std::string_view right, std::size_t &budget)
{
    const auto a = tokens(left);
    const auto b = tokens(right);
    const auto same = [&](std::size_t i, std::size_t j) {
        return view(left, a[i].range) == view(right, b[j].range);
    };
    std::size_t prefix = 0;
    while (prefix < a.size() && prefix < b.size() && same(prefix, prefix)) ++prefix;
    std::size_t suffix = 0;
    while (suffix < a.size() - prefix && suffix < b.size() - prefix
           && same(a.size() - 1 - suffix, b.size() - 1 - suffix))
        ++suffix;
    const auto n = a.size() - prefix - suffix;
    const auto m = b.size() - prefix - suffix;
    std::vector<bool> leftMatched(a.size(), true);
    std::vector<bool> rightMatched(b.size(), true);
    for (std::size_t i = 0; i < n; ++i) leftMatched[prefix + i] = false;
    for (std::size_t j = 0; j < m; ++j) rightMatched[prefix + j] = false;
    if (n > 0 && m > 0) {
        if (n > kMaxWordDifferencePairCells / m || n * m > budget) return {};
        budget -= n * m;
        // Interned token ids keep the table loop to integer comparisons.
        std::unordered_map<std::string_view, std::uint32_t> ids;
        const auto intern = [&](std::string_view token) {
            return ids.emplace(token, static_cast<std::uint32_t>(ids.size())).first->second;
        };
        std::vector<std::uint32_t> x(n), y(m);
        for (std::size_t i = 0; i < n; ++i) x[i] = intern(view(left, a[prefix + i].range));
        for (std::size_t j = 0; j < m; ++j) y[j] = intern(view(right, b[prefix + j].range));
        // lcs[i][j] is the common length of x[i..] and y[j..]. Both sides are
        // at most 1000 tokens when the product is, so 16 bits hold it.
        const auto width = m + 1;
        std::vector<std::uint16_t> lcs((n + 1) * width, 0);
        for (auto i = n; i-- > 0;)
            for (auto j = m; j-- > 0;)
                lcs[i * width + j] = x[i] == y[j] ? static_cast<std::uint16_t>(lcs[(i + 1) * width + j + 1] + 1)
                                                  : std::max(lcs[(i + 1) * width + j], lcs[i * width + j + 1]);
        std::size_t i = 0;
        std::size_t j = 0;
        while (i < n && j < m) {
            if (x[i] == y[j]) {
                leftMatched[prefix + i++] = true;
                rightMatched[prefix + j++] = true;
            } else if (lcs[(i + 1) * width + j] >= lcs[i * width + j + 1]) {
                ++i;
            } else {
                ++j;
            }
        }
    }
    return {ranges(left, a, leftMatched), ranges(right, b, rightMatched)};
}

namespace {
const std::optional<SourceSnapshot> &snapshot(const FileSnapshots &snapshots, SourceSide side)
{
    switch (side) {
    case SourceSide::Earlier: return snapshots.earlier;
    case SourceSide::Stage2: return snapshots.stage2;
    case SourceSide::Stage3: break;
    }
    return snapshots.stage3;
}

void append(std::vector<ByteRange> &into, const std::vector<ByteRange> &ranges, std::size_t offset)
{
    for (const auto &range : ranges) into.push_back({range.begin + offset, range.end + offset});
}

bool inside(const std::vector<ByteRange> &covered, ByteRange range)
{
    return std::any_of(covered.begin(), covered.end(), [&](const auto &cover) {
        return cover.begin <= range.begin && range.end <= cover.end;
    });
}

// Ordered and disjoint: the result can differ from both sources at one place.
void normalize(std::vector<ByteRange> &ranges)
{
    std::sort(ranges.begin(), ranges.end(), [](const auto &x, const auto &y) {
        return x.begin != y.begin ? x.begin < y.begin : x.end < y.end;
    });
    std::vector<ByteRange> merged;
    for (const auto &range : ranges) {
        if (!merged.empty() && range.begin <= merged.back().end) merged.back().end = std::max(merged.back().end, range.end);
        else merged.push_back(range);
    }
    ranges = std::move(merged);
}

bool undecided(const FileSession &file, const ChoiceTarget &target)
{
    const auto &decisions = file.current().draft.decisions;
    return std::any_of(target.decisions.begin(), target.decisions.end(), [&](const auto &id) {
        const auto found = decisions.find(id);
        return found == decisions.end() || found->second.value == DecisionValue::Undecided;
    });
}

std::optional<ByteRange> mappedResult(const CorrespondenceMap &map, const ChoiceTarget &target)
{
    const auto *anchor = map.anchor(target.source.contributionId);
    if (!anchor || anchor->state != AnchorState::Mapped || !anchor->result
        || !validByteRange(map.text(), *anchor->result))
        return std::nullopt;
    return anchor->result;
}
}

FileWordDifferences fileWordDifferences(const FileSession &file)
{
    FileWordDifferences found;
    const auto *snapshots = file.snapshots();
    const auto *map = file.correspondence();
    if (!snapshots || !map || file.support() != Support::Supported) return found;
    const std::string_view result = map->text();
    auto budget = kMaxWordDifferenceCells;
    std::map<SourceSide, std::vector<ByteRange>> covered;
    std::vector<ByteRange> resultCovered;

    if (snapshots->stage2 && snapshots->stage3) {
        const std::string_view left = snapshots->stage2->worktree.utf8;
        const std::string_view right = snapshots->stage3->worktree.utf8;
        const auto &targets = file.choices();
        for (const auto &ours : targets) {
            if (ours.scope != ChoiceScope::Section || ours.source.side != SourceSide::Stage2) continue;
            const auto theirs = std::find_if(targets.begin(), targets.end(), [&](const auto &target) {
                return target.scope == ChoiceScope::Section && target.source.side == SourceSide::Stage3
                    && target.section == ours.section;
            });
            if (theirs == targets.end() || (!undecided(file, ours) && !undecided(file, *theirs))) continue;
            const auto l = ours.source.range;
            const auto r = theirs->source.range;
            if (!validByteRange(left, l) || !validByteRange(right, r)) continue;
            const auto words = wordDifferences(view(left, l), view(right, r), budget);
            append(found.sources[SourceSide::Stage2], words.left, l.begin);
            append(found.sources[SourceSide::Stage3], words.right, r.begin);
            covered[SourceSide::Stage2].push_back(l);
            covered[SourceSide::Stage3].push_back(r);
            const auto mappedLeft = mappedResult(*map, ours);
            const auto mappedRight = mappedResult(*map, *theirs);
            // An empty side, or one result range for both, leaves nothing to set
            // side by side; the changed passages cover the result instead.
            if (!mappedLeft || !mappedRight || mappedLeft->begin == mappedLeft->end
                || mappedRight->begin == mappedRight->end
                || (mappedLeft->begin < mappedRight->end && mappedRight->begin < mappedLeft->end))
                continue;
            const auto inResult = wordDifferences(view(result, *mappedLeft), view(result, *mappedRight), budget);
            append(found.result, inResult.left, mappedLeft->begin);
            append(found.result, inResult.right, mappedRight->begin);
            resultCovered.push_back(*mappedLeft);
            resultCovered.push_back(*mappedRight);
        }
    }

    for (const auto side : {SourceSide::Stage2, SourceSide::Stage3, SourceSide::Earlier}) {
        const auto &source = snapshot(*snapshots, side);
        const auto *passages = map->passages(side);
        if (!source || !passages) continue;
        const std::string_view text = source->worktree.utf8;
        for (const auto &passage : *passages) {
            if (passage.kind != PassageKind::Changed || !validByteRange(text, passage.source)
                || !validByteRange(result, passage.result))
                continue;
            const auto words = wordDifferences(view(text, passage.source), view(result, passage.result), budget);
            for (const auto &range : words.left) {
                const ByteRange at{range.begin + passage.source.begin, range.end + passage.source.begin};
                if (!inside(covered[side], at)) found.sources[side].push_back(at);
            }
            if (side == SourceSide::Earlier) continue;
            for (const auto &range : words.right) {
                const ByteRange at{range.begin + passage.result.begin, range.end + passage.result.begin};
                if (!inside(resultCovered, at)) found.result.push_back(at);
            }
        }
    }
    for (auto &[side, ranges] : found.sources) normalize(ranges);
    normalize(found.result);
    std::erase_if(found.sources, [](const auto &entry) { return entry.second.empty(); });
    return found;
}

}
