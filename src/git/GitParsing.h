// Git parsing preserves exact repository paths while decoding conflict and index records.
#pragma once

#include "GitAdapter.h"
#include <algorithm>
#include <optional>

namespace branch_truce::git::detail {
inline bool relativeFile(const QByteArray &path)
{
    // ':' is an ordinary Linux filename byte, not a Qt resource path here.
    if (path.isEmpty() || path.startsWith('/') || path.contains('\0')) return false;
    for (const auto &part : path.split('/')) {
        if (part.isEmpty() || part == "." || part == "..") return false;
    }
    return true;
}
inline bool objectId(const QByteArray &id)
{
    return (id.size() == 40 || id.size() == 64)
        && std::all_of(id.cbegin(), id.cend(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        });
}
inline std::optional<QList<IndexEntry>> parseIndex(const QByteArray &bytes)
{
    QList<IndexEntry> entries;
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const auto end = bytes.indexOf('\0', offset);
        if (end < 0) return std::nullopt;
        const auto record = bytes.mid(offset, end - offset);
        const auto tab = record.indexOf('\t');
        if (tab < 0) return std::nullopt;
        const auto fields = record.first(tab).split(' ');
        const auto path = record.mid(tab + 1);
        if (fields.size() != 3 || fields[0].size() != 6
            || !std::all_of(fields[0].cbegin(), fields[0].cend(), [](char c) { return c >= '0' && c <= '7'; })
            || !objectId(fields[1]) || fields[2].size() != 1 || fields[2][0] < '0' || fields[2][0] > '3'
            || !relativeFile(path)) return std::nullopt;
        entries.append({path, fields[0], fields[1], static_cast<unsigned int>(fields[2][0] - '0')});
        offset = end + 1;
    }
    return entries;
}
} // namespace branch_truce::git::detail
