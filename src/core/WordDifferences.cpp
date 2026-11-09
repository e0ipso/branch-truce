// Word comparisons locate changed tokens within corresponding source and result passages.
#include "WordDifferences.h"
#include <algorithm>
#include <cstdint>
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

bool blank(unsigned char c) { return c == ' ' || c == '\t'; }

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

// Adjacent unmatched, reported tokens as ranges, trimmed of blanks at the edges.
std::vector<ByteRange> ranges(std::string_view text, const std::vector<Token> &all, const std::vector<bool> &matched)
{
    std::vector<ByteRange> merged;
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (matched[i] || !all[i].reported) continue;
        if (!merged.empty() && merged.back().end == all[i].range.begin)
            merged.back().end = all[i].range.end;
        else
            merged.push_back(all[i].range);
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
    const auto same = [&](std::size_t i, std::size_t j) { return slice(left, a[i].range) == slice(right, b[j].range); };
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
        for (std::size_t i = 0; i < n; ++i) x[i] = intern(slice(left, a[prefix + i].range));
        for (std::size_t j = 0; j < m; ++j) y[j] = intern(slice(right, b[prefix + j].range));
        // lcs[i][j] is the common length of x[i..] and y[j..]. That length is
        // at most min(n, m), and min(n, m) is at most 1000 because the
        // product is at most kMaxWordDifferencePairCells, so 16 bits hold it.
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

}
