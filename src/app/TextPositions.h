#pragma once
#include "core/FileSnapshot.h"
#include "core/Correspondence.h"
#include <QString>
#include <QVariantMap>
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string_view>
#include <vector>

namespace branch_truce {
// Line numbers and editor offsets for one valid UTF-8 text. One pass records
// each line start, so a lookup scans at most one line. Lines are 1-based, as in
// the scope labels, and a range covers lines [LineBegin, LineEnd).
class TextPositions {
public:
    explicit TextPositions(std::string_view text) : text_(text)
    {
        int units = 0;
        for (std::size_t i = 0; i < text_.size(); ++i) {
            units += utf16Units(text_[i]);
            if (text_[i] == '\n') starts_.push_back({i + 1, units});
        }
    }
    [[nodiscard]] bool contains(ByteRange range) const noexcept
    {
        return range.begin <= range.end && range.end <= text_.size();
    }
    [[nodiscard]] int line(std::size_t offset) const
    {
        return static_cast<int>(std::distance(starts_.begin(), lineAt(offset))) + 1;
    }
    [[nodiscard]] int utf16(std::size_t offset) const
    {
        const auto start = lineAt(offset);
        auto units = start->utf16;
        for (auto i = start->byte; i < offset; ++i) units += utf16Units(text_[i]);
        return units;
    }
    void insert(QVariantMap &map, const QString &prefix, ByteRange range) const
    {
        const auto first = line(range.begin);
        map.insert(prefix + QStringLiteral("LineBegin"), first);
        map.insert(prefix + QStringLiteral("LineEnd"), range.end > range.begin ? line(range.end - 1) + 1 : first);
        map.insert(prefix + QStringLiteral("Utf16Begin"), utf16(range.begin));
        map.insert(prefix + QStringLiteral("Utf16End"), utf16(range.end));
    }

private:
    struct Start {
        std::size_t byte;
        int utf16;
    };
    static int utf16Units(char byte) noexcept
    {
        const auto value = static_cast<unsigned char>(byte);
        return (value & 0xC0) == 0x80 ? 0 : value >= 0xF0 ? 2 : 1;
    }
    [[nodiscard]] std::vector<Start>::const_iterator lineAt(std::size_t offset) const
    {
        return std::prev(std::upper_bound(starts_.begin(), starts_.end(), offset,
            [](std::size_t value, const Start &start) { return value < start.byte; }));
    }
    std::string_view text_;
    std::vector<Start> starts_{{0, 0}};
};

// The captured source of one side.
inline const std::optional<SourceSnapshot> &snapshotFor(const FileSnapshots &snapshots, SourceSide side)
{
    return side == SourceSide::Earlier ? snapshots.earlier
        : side == SourceSide::Stage2 ? snapshots.stage2 : snapshots.stage3;
}
}
