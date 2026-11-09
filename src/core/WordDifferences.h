// Word comparisons locate changed tokens within corresponding source and result passages.
#pragma once

#include "ByteRanges.h"
#include <cstddef>
#include <string_view>
#include <vector>

namespace branch_truce {

// Word-level differences inside changed lines. They only decorate the panes:
// nothing here changes text, decisions or revisions. docs/resource-limits.md
// records the limits below.
//
// A line longer than this many bytes, newline excluded, is compared as one
// token and never gets word ranges, so it keeps its line colouring only.
inline constexpr std::size_t kMaxWordDifferenceLineBytes = 1000;
// Token comparisons one pair of passages may use. A larger pair gets no word
// ranges.
inline constexpr std::size_t kMaxWordDifferencePairCells = 1000000;
// Token comparisons all pairs of one revision may use together. Pairs are
// compared in text order; once this is spent the rest get no word ranges.
inline constexpr std::size_t kMaxWordDifferenceCells = 4000000;

struct WordDifferences {
    std::vector<ByteRange> left;
    std::vector<ByteRange> right;
    bool operator==(const WordDifferences &) const = default;
};

// The tokens of left and right outside their longest common token sequence,
// as ordered, disjoint byte ranges into each text. A token is a run of word
// characters (ASCII letters, digits, underscore and the letters of other
// scripts), a run of blanks (Unicode white space other than line breaks), one
// newline, or one other character: ASCII punctuation, Unicode punctuation and
// symbols, and each character of scripts written without spaces, such as CJK
// ideographs and kana. Bytes that are not UTF-8 count one at a time. Adjacent
// differing tokens form one range, without blanks at either edge unless it
// holds nothing else. Newlines are never part of a range. When the pair needs
// more comparisons than budget or kMaxWordDifferencePairCells allow, both
// lists are empty and budget is unchanged; otherwise the comparisons used are
// taken from budget.
[[nodiscard]] WordDifferences wordDifferences(std::string_view left, std::string_view right, std::size_t &budget);

}
