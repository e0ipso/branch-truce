// Word comparisons locate changed tokens within corresponding source and result passages.
#include "WordDifferences.h"
#include "Correspondence.h"
#include "ResourceLimits.h"
#include <algorithm>
#include <cstdint>
#include <iterator>
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

// Unicode License V3: https://www.unicode.org/license.txt
// Copyright © 1991-2026 Unicode, Inc.
//
// NOTICE TO USER: Carefully read the following legal agreement. BY DOWNLOADING, INSTALLING, COPYING OR OTHERWISE USING
// DATA FILES, AND/OR SOFTWARE, YOU UNEQUIVOCALLY ACCEPT, AND AGREE TO BE BOUND BY, ALL OF THE TERMS AND CONDITIONS OF
// THIS AGREEMENT. IF YOU DO NOT AGREE, DO NOT DOWNLOAD, INSTALL, COPY, DISTRIBUTE OR USE THE DATA FILES OR SOFTWARE.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of data files and any associated
// documentation (the "Data Files") or software and any associated documentation (the "Software") to deal in the Data
// Files or Software without restriction, including without limitation the rights to use, copy, modify, merge, publish,
// distribute, and/or sell copies of the Data Files or Software, and to permit persons to whom the Data Files or
// Software are furnished to do so, provided that either (a) this copyright and permission notice appear with all
// copies of the Data Files or Software, or (b) this copyright and permission notice appear in associated
// Documentation.
//
// THE DATA FILES AND SOFTWARE ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
// NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF THIRD
// PARTY RIGHTS. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS INCLUDED IN THIS NOTICE BE LIABLE FOR ANY CLAIM, OR
// ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
// PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH
// THE USE OR PERFORMANCE OF THE DATA FILES OR SOFTWARE.
//
// Except as contained in this notice, the name of a copyright holder shall not be used in advertising or otherwise to
// promote the sale, use or other dealings in these Data Files or Software without prior written authorization of the
// copyright holder.
//
// Unicode 17.0.0 General_Category P* and S*, as half-open intervals.
// Source: https://www.unicode.org/Public/17.0.0/ucd/UnicodeData.txt
// SHA-256: 2e1efc1dcb59c575eedf5ccae60f95229f706ee6d031835247d843c11d96470c
// Regenerate: select field 0 where field 2 starts with P or S, merge adjacent
// code points, then emit each interval's start and exclusive end in order.
// No P*/S* record uses UnicodeData's First/Last range notation.
bool punctuationOrSymbol(char32_t c) noexcept
{
    static constexpr char32_t bounds[]{
        0x21,    0x30,    0x3a,    0x41,    0x5b,    0x61,    0x7b,    0x7f,    0xa1,    0xaa,    0xab,    0xad,
        0xae,    0xb2,    0xb4,    0xb5,    0xb6,    0xb9,    0xbb,    0xbc,    0xbf,    0xc0,    0xd7,    0xd8,
        0xf7,    0xf8,    0x2c2,   0x2c6,   0x2d2,   0x2e0,   0x2e5,   0x2ec,   0x2ed,   0x2ee,   0x2ef,   0x300,
        0x375,   0x376,   0x37e,   0x37f,   0x384,   0x386,   0x387,   0x388,   0x3f6,   0x3f7,   0x482,   0x483,
        0x55a,   0x560,   0x589,   0x58b,   0x58d,   0x590,   0x5be,   0x5bf,   0x5c0,   0x5c1,   0x5c3,   0x5c4,
        0x5c6,   0x5c7,   0x5f3,   0x5f5,   0x606,   0x610,   0x61b,   0x61c,   0x61d,   0x620,   0x66a,   0x66e,
        0x6d4,   0x6d5,   0x6de,   0x6df,   0x6e9,   0x6ea,   0x6fd,   0x6ff,   0x700,   0x70e,   0x7f6,   0x7fa,
        0x7fe,   0x800,   0x830,   0x83f,   0x85e,   0x85f,   0x888,   0x889,   0x964,   0x966,   0x970,   0x971,
        0x9f2,   0x9f4,   0x9fa,   0x9fc,   0x9fd,   0x9fe,   0xa76,   0xa77,   0xaf0,   0xaf2,   0xb70,   0xb71,
        0xbf3,   0xbfb,   0xc77,   0xc78,   0xc7f,   0xc80,   0xc84,   0xc85,   0xd4f,   0xd50,   0xd79,   0xd7a,
        0xdf4,   0xdf5,   0xe3f,   0xe40,   0xe4f,   0xe50,   0xe5a,   0xe5c,   0xf01,   0xf18,   0xf1a,   0xf20,
        0xf34,   0xf35,   0xf36,   0xf37,   0xf38,   0xf39,   0xf3a,   0xf3e,   0xf85,   0xf86,   0xfbe,   0xfc6,
        0xfc7,   0xfcd,   0xfce,   0xfdb,   0x104a,  0x1050,  0x109e,  0x10a0,  0x10fb,  0x10fc,  0x1360,  0x1369,
        0x1390,  0x139a,  0x1400,  0x1401,  0x166d,  0x166f,  0x169b,  0x169d,  0x16eb,  0x16ee,  0x1735,  0x1737,
        0x17d4,  0x17d7,  0x17d8,  0x17dc,  0x1800,  0x180b,  0x1940,  0x1941,  0x1944,  0x1946,  0x19de,  0x1a00,
        0x1a1e,  0x1a20,  0x1aa0,  0x1aa7,  0x1aa8,  0x1aae,  0x1b4e,  0x1b50,  0x1b5a,  0x1b6b,  0x1b74,  0x1b80,
        0x1bfc,  0x1c00,  0x1c3b,  0x1c40,  0x1c7e,  0x1c80,  0x1cc0,  0x1cc8,  0x1cd3,  0x1cd4,  0x1fbd,  0x1fbe,
        0x1fbf,  0x1fc2,  0x1fcd,  0x1fd0,  0x1fdd,  0x1fe0,  0x1fed,  0x1ff0,  0x1ffd,  0x1fff,  0x2010,  0x2028,
        0x2030,  0x205f,  0x207a,  0x207f,  0x208a,  0x208f,  0x20a0,  0x20c2,  0x2100,  0x2102,  0x2103,  0x2107,
        0x2108,  0x210a,  0x2114,  0x2115,  0x2116,  0x2119,  0x211e,  0x2124,  0x2125,  0x2126,  0x2127,  0x2128,
        0x2129,  0x212a,  0x212e,  0x212f,  0x213a,  0x213c,  0x2140,  0x2145,  0x214a,  0x214e,  0x214f,  0x2150,
        0x218a,  0x218c,  0x2190,  0x242a,  0x2440,  0x244b,  0x249c,  0x24ea,  0x2500,  0x2776,  0x2794,  0x2b74,
        0x2b76,  0x2c00,  0x2ce5,  0x2ceb,  0x2cf9,  0x2cfd,  0x2cfe,  0x2d00,  0x2d70,  0x2d71,  0x2e00,  0x2e2f,
        0x2e30,  0x2e5e,  0x2e80,  0x2e9a,  0x2e9b,  0x2ef4,  0x2f00,  0x2fd6,  0x2ff0,  0x3000,  0x3001,  0x3005,
        0x3008,  0x3021,  0x3030,  0x3031,  0x3036,  0x3038,  0x303d,  0x3040,  0x309b,  0x309d,  0x30a0,  0x30a1,
        0x30fb,  0x30fc,  0x3190,  0x3192,  0x3196,  0x31a0,  0x31c0,  0x31e6,  0x31ef,  0x31f0,  0x3200,  0x321f,
        0x322a,  0x3248,  0x3250,  0x3251,  0x3260,  0x3280,  0x328a,  0x32b1,  0x32c0,  0x3400,  0x4dc0,  0x4e00,
        0xa490,  0xa4c7,  0xa4fe,  0xa500,  0xa60d,  0xa610,  0xa673,  0xa674,  0xa67e,  0xa67f,  0xa6f2,  0xa6f8,
        0xa700,  0xa717,  0xa720,  0xa722,  0xa789,  0xa78b,  0xa828,  0xa82c,  0xa836,  0xa83a,  0xa874,  0xa878,
        0xa8ce,  0xa8d0,  0xa8f8,  0xa8fb,  0xa8fc,  0xa8fd,  0xa92e,  0xa930,  0xa95f,  0xa960,  0xa9c1,  0xa9ce,
        0xa9de,  0xa9e0,  0xaa5c,  0xaa60,  0xaa77,  0xaa7a,  0xaade,  0xaae0,  0xaaf0,  0xaaf2,  0xab5b,  0xab5c,
        0xab6a,  0xab6c,  0xabeb,  0xabec,  0xfb29,  0xfb2a,  0xfbb2,  0xfbd3,  0xfd3e,  0xfd50,  0xfd90,  0xfd92,
        0xfdc8,  0xfdd0,  0xfdfc,  0xfe00,  0xfe10,  0xfe1a,  0xfe30,  0xfe53,  0xfe54,  0xfe67,  0xfe68,  0xfe6c,
        0xff01,  0xff10,  0xff1a,  0xff21,  0xff3b,  0xff41,  0xff5b,  0xff66,  0xffe0,  0xffe7,  0xffe8,  0xffef,
        0xfffc,  0xfffe,  0x10100, 0x10103, 0x10137, 0x10140, 0x10179, 0x1018a, 0x1018c, 0x1018f, 0x10190, 0x1019d,
        0x101a0, 0x101a1, 0x101d0, 0x101fd, 0x1039f, 0x103a0, 0x103d0, 0x103d1, 0x1056f, 0x10570, 0x10857, 0x10858,
        0x10877, 0x10879, 0x1091f, 0x10920, 0x1093f, 0x10940, 0x10a50, 0x10a59, 0x10a7f, 0x10a80, 0x10ac8, 0x10ac9,
        0x10af0, 0x10af7, 0x10b39, 0x10b40, 0x10b99, 0x10b9d, 0x10d6e, 0x10d6f, 0x10d8e, 0x10d90, 0x10ead, 0x10eae,
        0x10ed0, 0x10ed9, 0x10f55, 0x10f5a, 0x10f86, 0x10f8a, 0x11047, 0x1104e, 0x110bb, 0x110bd, 0x110be, 0x110c2,
        0x11140, 0x11144, 0x11174, 0x11176, 0x111c5, 0x111c9, 0x111cd, 0x111ce, 0x111db, 0x111dc, 0x111dd, 0x111e0,
        0x11238, 0x1123e, 0x112a9, 0x112aa, 0x113d4, 0x113d6, 0x113d7, 0x113d9, 0x1144b, 0x11450, 0x1145a, 0x1145c,
        0x1145d, 0x1145e, 0x114c6, 0x114c7, 0x115c1, 0x115d8, 0x11641, 0x11644, 0x11660, 0x1166d, 0x116b9, 0x116ba,
        0x1173c, 0x11740, 0x1183b, 0x1183c, 0x11944, 0x11947, 0x119e2, 0x119e3, 0x11a3f, 0x11a47, 0x11a9a, 0x11a9d,
        0x11a9e, 0x11aa3, 0x11b00, 0x11b0a, 0x11be1, 0x11be2, 0x11c41, 0x11c46, 0x11c70, 0x11c72, 0x11ef7, 0x11ef9,
        0x11f43, 0x11f50, 0x11fd5, 0x11ff2, 0x11fff, 0x12000, 0x12470, 0x12475, 0x12ff1, 0x12ff3, 0x16a6e, 0x16a70,
        0x16af5, 0x16af6, 0x16b37, 0x16b40, 0x16b44, 0x16b46, 0x16d6d, 0x16d70, 0x16e97, 0x16e9b, 0x16fe2, 0x16fe3,
        0x1bc9c, 0x1bc9d, 0x1bc9f, 0x1bca0, 0x1cc00, 0x1ccf0, 0x1ccfa, 0x1ccfd, 0x1cd00, 0x1ceb4, 0x1ceba, 0x1ced1,
        0x1cee0, 0x1cef1, 0x1cf50, 0x1cfc4, 0x1d000, 0x1d0f6, 0x1d100, 0x1d127, 0x1d129, 0x1d165, 0x1d16a, 0x1d16d,
        0x1d183, 0x1d185, 0x1d18c, 0x1d1aa, 0x1d1ae, 0x1d1eb, 0x1d200, 0x1d242, 0x1d245, 0x1d246, 0x1d300, 0x1d357,
        0x1d6c1, 0x1d6c2, 0x1d6db, 0x1d6dc, 0x1d6fb, 0x1d6fc, 0x1d715, 0x1d716, 0x1d735, 0x1d736, 0x1d74f, 0x1d750,
        0x1d76f, 0x1d770, 0x1d789, 0x1d78a, 0x1d7a9, 0x1d7aa, 0x1d7c3, 0x1d7c4, 0x1d800, 0x1da00, 0x1da37, 0x1da3b,
        0x1da6d, 0x1da75, 0x1da76, 0x1da84, 0x1da85, 0x1da8c, 0x1e14f, 0x1e150, 0x1e2ff, 0x1e300, 0x1e5ff, 0x1e600,
        0x1e95e, 0x1e960, 0x1ecac, 0x1ecad, 0x1ecb0, 0x1ecb1, 0x1ed2e, 0x1ed2f, 0x1eef0, 0x1eef2, 0x1f000, 0x1f02c,
        0x1f030, 0x1f094, 0x1f0a0, 0x1f0af, 0x1f0b1, 0x1f0c0, 0x1f0c1, 0x1f0d0, 0x1f0d1, 0x1f0f6, 0x1f10d, 0x1f1ae,
        0x1f1e6, 0x1f203, 0x1f210, 0x1f23c, 0x1f240, 0x1f249, 0x1f250, 0x1f252, 0x1f260, 0x1f266, 0x1f300, 0x1f6d9,
        0x1f6dc, 0x1f6ed, 0x1f6f0, 0x1f6fd, 0x1f700, 0x1f7da, 0x1f7e0, 0x1f7ec, 0x1f7f0, 0x1f7f1, 0x1f800, 0x1f80c,
        0x1f810, 0x1f848, 0x1f850, 0x1f85a, 0x1f860, 0x1f888, 0x1f890, 0x1f8ae, 0x1f8b0, 0x1f8bc, 0x1f8c0, 0x1f8c2,
        0x1f8d0, 0x1f8d9, 0x1f900, 0x1fa58, 0x1fa60, 0x1fa6e, 0x1fa70, 0x1fa7d, 0x1fa80, 0x1fa8b, 0x1fa8e, 0x1fac7,
        0x1fac8, 0x1fac9, 0x1facd, 0x1fadd, 0x1fadf, 0x1faeb, 0x1faef, 0x1faf9, 0x1fb00, 0x1fb93, 0x1fb94, 0x1fbf0,
        0x1fbfa, 0x1fbfb};
    const auto end = std::upper_bound(std::begin(bounds), std::end(bounds), c);
    return (end - std::begin(bounds)) % 2 != 0;
}

// Preserve individual CJK and kana tokens in scripts written without spaces.
bool singleCharacter(char32_t c) noexcept
{
    return punctuationOrSymbol(c) || within(c, 0x3001, 0x30ff) || within(c, 0x3400, 0x4dbf) || within(c, 0x4e00, 0x9fff)
        || within(c, 0xf900, 0xfaff) || within(c, 0x20000, 0x3ffff);
}

TokenKind kindOf(char32_t c) noexcept
{
    if (blankCharacter(c)) return TokenKind::Blank;
    if (c == '_' || within(c, '0', '9') || within(c, 'a', 'z') || within(c, 'A', 'Z')) return TokenKind::Word;
    if (c < 0x80 || singleCharacter(c)) return TokenKind::Single;
    return TokenKind::Word;
}

// The tokens of one line up to `lineEnd`, appended to `result`.
bool lineTokens(std::string_view text, std::size_t at, std::size_t lineEnd, std::vector<Token> &result,
                MetadataBudget &budget)
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
        // Includes vector growth, match flags, interned IDs and hash nodes.
        if (!budget.take(1, 128 + 2 * sizeof(Token))) return false;
        result.push_back({{at, end}, kind});
        at = end;
    }
    return true;
}

bool tokens(std::string_view text, std::vector<Token> &result, MetadataBudget &budget)
{
    std::size_t at = 0;
    while (at < text.size()) {
        const auto lineEnd = std::min(text.find('\n', at), text.size());
        if (lineEnd - at > kMaxWordDifferenceLineBytes) {
            if (!budget.take(1, 128 + 2 * sizeof(Token))) return false;
            result.push_back({{at, lineEnd}, TokenKind::Unreported});
        } else if (!lineTokens(text, at, lineEnd, result, budget))
            return false;
        at = lineEnd;
        if (at < text.size()) {
            if (!budget.take(1, 128 + 2 * sizeof(Token))) return false;
            result.push_back({{at, at + 1}, TokenKind::Unreported});
            ++at;
        }
    }
    return true;
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

WordDifferences wordDifferences(std::string_view left, std::string_view right, std::size_t &budget,
                                WordDifferenceLimits limits)
{
    WordDifferences refused{{}, {}, WordDifferenceState::ResourceLimit};
    if (left.size() > limits.inputBytes || right.size() > limits.inputBytes - left.size()) return refused;
    MetadataBudget admission{limits.tokens, limits.retainedBytes};
    std::vector<Token> a, b;
    if (!tokens(left, a, admission) || !tokens(right, b, admission)) return refused;
    const auto same = [&](std::size_t i, std::size_t j) { return slice(left, a[i].range) == slice(right, b[j].range); };
    std::size_t prefix = 0;
    while (prefix < a.size() && prefix < b.size() && same(prefix, prefix)) ++prefix;
    std::size_t suffix = 0;
    while (suffix < a.size() - prefix && suffix < b.size() - prefix
           && same(a.size() - 1 - suffix, b.size() - 1 - suffix))
        ++suffix;
    const auto n = a.size() - prefix - suffix;
    const auto m = b.size() - prefix - suffix;
    if (n && m && (n > kMaxWordDifferencePairCells / m || n * m > budget)) return refused;
    std::vector<bool> leftMatched(a.size(), true);
    std::vector<bool> rightMatched(b.size(), true);
    for (std::size_t i = 0; i < n; ++i) leftMatched[prefix + i] = false;
    for (std::size_t j = 0; j < m; ++j) rightMatched[prefix + j] = false;
    if (n > 0 && m > 0) {
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
