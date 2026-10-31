// Text changes identify the edited range and preserve line positions across a result edit.
#pragma once
#include <QString>
#include <QtGlobal>
#include <vector>

namespace branch_truce {
// A range in which two texts differ, in UTF-16 units. begin and oldEnd are
// offsets into the old text and newEnd an offset into the new text.
struct TextChange {
    qsizetype begin = 0;
    qsizetype oldEnd = 0;
    qsizetype newEnd = 0;
};

// The one range in which two texts differ, widened to whole surrogate pairs.
[[nodiscard]] TextChange changedRange(const QString &old, const QString &next);

// The separate ranges in which two texts differ, in order. Lines that the line
// correspondence keeps equal split the one outer range, so text between two
// changes is not part of either. Each part is narrowed to its own changed range.
[[nodiscard]] std::vector<TextChange> changedRanges(const QString &old, const QString &next);

// An offset before a change keeps its place and one after it moves with the
// text. An offset inside a replaced range, or at an insertion, goes to its end.
// A change's begin and old end are in the old text and its new end in the new
// text, so the shift after it is its new end less its old end.
[[nodiscard]] int changedOffset(const std::vector<TextChange> &changes, int offset);
}
