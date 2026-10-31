// Word comparisons locate changed tokens within corresponding source and result passages.
#pragma once

#include "Correspondence.h"
#include <cstddef>
#include <map>
#include <string_view>
#include <vector>

namespace branch_truce {

class FileSession;

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
// bytes (ASCII letters, digits, underscore and every non-ASCII byte, so UTF-8
// sequences stay whole), a run of spaces and tabs, one newline, or one other
// byte. Adjacent differing tokens form one range, trimmed of spaces and tabs
// at either edge unless it holds nothing else. Newlines are never part of a
// range. When the pair needs more comparisons than budget or
// kMaxWordDifferencePairCells allow, both lists are empty and budget is
// unchanged; otherwise the comparisons used are taken from budget.
[[nodiscard]] WordDifferences wordDifferences(std::string_view left, std::string_view right,
                                              std::size_t &budget);

// Word ranges for the current revision of a file, as byte ranges into each
// source's worktree text and into the correspondence result text.
//
// A changed passage pairs a source range with its result range. While either
// side of a choice section is undecided, its two competing source sections
// pair with each other instead, and so do their mapped result ranges when
// both are mapped. Ranges a competing pair covers come from that pair only.
// Earlier-version passages decorate the earlier pane, never the result.
struct FileWordDifferences {
    std::map<SourceSide, std::vector<ByteRange>> sources;
    std::vector<ByteRange> result;
    bool operator==(const FileWordDifferences &) const = default;
};

[[nodiscard]] FileWordDifferences fileWordDifferences(const FileSession &file);

}
