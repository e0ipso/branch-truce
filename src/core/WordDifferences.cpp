// Word comparisons locate changed tokens within corresponding source and result passages.
#include "WordDifferences.h"
#include "Correspondence.h"
#include <algorithm>
#include <cstdint>
#include <unordered_map>

namespace branch_truce {
namespace {
enum class TokenKind { Word, Blank, Single, Unreported };

struct Token {
    ByteRange range;
    TokenKind kind = TokenKind::Word;
};

// One decoded character. Bytes that are not valid UTF-8 decode one at a
// time and are not characters.
struct Character {
    char32_t value = 0;
    std::size_t size = 1;
    bool valid = true;
};

Character decode(std::string_view text, std::size_t at)
{
    const auto first = static_cast<unsigned char>(text[at]);
    if (first < 0x80) return {first, 1, true};
    const std::size_t size = first >= 0xf0 ? 4 : first >= 0xe0 ? 3 : first >= 0xc0 ? 2 : 1;
    if (size == 1 || at + size > text.size() || !validUtf8(text.substr(at, size))) return {first, 1, false};
    char32_t value = first & (0xffU >> (size + 1));
    for (std::size_t i = 1; i < size; ++i) value = (value << 6U) | (static_cast<unsigned char>(text[at + i]) & 0x3fU);
    return {value, size, true};
}

bool within(char32_t c, char32_t first, char32_t last) noexcept { return first <= c && c <= last; }

// Unicode White_Space other than line breaks.
bool blankCharacter(char32_t c) noexcept
{
    return c == ' ' || c == '\t' || c == 0xa0 || c == 0x1680 || within(c, 0x2000, 0x200a) || c == 0x2028 || c == 0x2029
        || c == 0x202f || c == 0x205f || c == 0x3000;
}

// Punctuation and symbols outside ASCII, and characters of scripts written
// without spaces between words. Each is a token on its own.
bool singleCharacter(char32_t c) noexcept
{
    return within(c, 0xa1, 0xbf) || c == 0xd7 || c == 0xf7 || within(c, 0x2010, 0x2027) || within(c, 0x2030, 0x205e)
        || within(c, 0x3001, 0x303f) || within(c, 0x3040, 0x30ff) || within(c, 0x3400, 0x4dbf)
        || within(c, 0x4e00, 0x9fff) || within(c, 0xf900, 0xfaff) || within(c, 0xff01, 0xff0f)
        || within(c, 0xff1a, 0xff20) || within(c, 0xff3b, 0xff40) || within(c, 0xff5b, 0xff65)
        || within(c, 0x20000, 0x3ffff);
}

TokenKind kindOf(char32_t c) noexcept
{
    if (blankCharacter(c)) return TokenKind::Blank;
    if (c == '_' || within(c, '0', '9') || within(c, 'a', 'z') || within(c, 'A', 'Z')) return TokenKind::Word;
    if (c < 0x80 || singleCharacter(c)) return TokenKind::Single;
    return TokenKind::Word;
}

// The tokens of one line up to `lineEnd`, appended to `result`.
void lineTokens(std::string_view text, std::size_t at, std::size_t lineEnd, std::vector<Token> &result)
{
    while (at < lineEnd) {
        const auto first = decode(text, at);
        const auto kind = first.valid ? kindOf(first.value) : TokenKind::Single;
        auto end = at + first.size;
        while ((kind == TokenKind::Word || kind == TokenKind::Blank) && end < lineEnd) {
            const auto next = decode(text, end);
            if (!next.valid || kindOf(next.value) != kind) break;
            end += next.size;
        }
        result.push_back({{at, end}, kind});
        at = end;
    }
}

std::vector<Token> tokens(std::string_view text)
{
    std::vector<Token> result;
    std::size_t at = 0;
    while (at < text.size()) {
        const auto lineEnd = std::min(text.find('\n', at), text.size());
        if (lineEnd - at > kMaxWordDifferenceLineBytes)
            result.push_back({{at, lineEnd}, TokenKind::Unreported});
        else
            lineTokens(text, at, lineEnd, result);
        at = lineEnd;
        if (at < text.size()) {
            result.push_back({{at, at + 1}, TokenKind::Unreported});
            ++at;
        }
    }
    return result;
}

// Adjacent unmatched, reported tokens as ranges, without blank tokens at
// either edge unless the range holds nothing else.
std::vector<ByteRange> ranges(const std::vector<Token> &all, const std::vector<bool> &matched)
{
    std::vector<ByteRange> merged;
    for (std::size_t i = 0; i < all.size();) {
        if (matched[i] || all[i].kind == TokenKind::Unreported) {
            ++i;
            continue;
        }
        auto last = i;
        while (last + 1 < all.size() && !matched[last + 1] && all[last + 1].kind != TokenKind::Unreported
               && all[last + 1].range.begin == all[last].range.end)
            ++last;
        auto first = i;
        while (first < last && all[first].kind == TokenKind::Blank) ++first;
        auto end = last;
        while (end > first && all[end].kind == TokenKind::Blank) --end;
        merged.push_back(all[first].kind == TokenKind::Blank ? ByteRange{all[i].range.begin, all[last].range.end}
                                                             : ByteRange{all[first].range.begin, all[end].range.end});
        i = last + 1;
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
    return {ranges(a, leftMatched), ranges(b, rightMatched)};
}

}
