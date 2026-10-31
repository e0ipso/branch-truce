// Text changes identify the edited range and preserve line positions across a result edit.
#include "TextChange.h"
#include "core/Correspondence.h"
#include <QByteArray>
#include <exception>
#include <string_view>

namespace branch_truce {
TextChange changedRange(const QString &old, const QString &next)
{
    TextChange change;
    while (change.begin < old.size() && change.begin < next.size() && old.at(change.begin) == next.at(change.begin))
        ++change.begin;
    if (change.begin > 0 && change.begin < old.size() && old.at(change.begin).isLowSurrogate()) --change.begin;
    change.oldEnd = old.size();
    change.newEnd = next.size();
    while (change.oldEnd > change.begin && change.newEnd > change.begin
        && old.at(change.oldEnd - 1) == next.at(change.newEnd - 1)) {
        --change.oldEnd;
        --change.newEnd;
    }
    if (change.oldEnd < old.size() && old.at(change.oldEnd).isLowSurrogate()) {
        ++change.oldEnd;
        ++change.newEnd;
    }
    return change;
}

std::vector<TextChange> changedRanges(const QString &old, const QString &next)
{
    const auto outer = changedRange(old, next);
    const auto before = old.mid(outer.begin, outer.oldEnd - outer.begin).toUtf8();
    const auto after = next.mid(outer.begin, outer.newEnd - outer.begin).toUtf8();
    const auto view = [](const QByteArray &bytes) {
        return std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    };
    std::vector<Passage> passages;
    try {
        passages = ExactLineCorrespondence().compare(view(before), view(after));
    } catch (const std::exception &) {
        return {outer};
    }
    // Passages split at line ends, so each one decodes on its own. Their lengths
    // locate the parts of the original texts.
    const auto length = [](const QByteArray &bytes, ByteRange range) {
        return QString::fromUtf8(bytes.constData() + range.begin, static_cast<qsizetype>(range.end - range.begin)).size();
    };
    std::vector<TextChange> changes;
    auto oldAt = outer.begin;
    auto newAt = outer.begin;
    for (const auto &passage : passages) {
        const auto oldPart = old.mid(oldAt, length(before, passage.source));
        const auto newPart = next.mid(newAt, length(after, passage.result));
        if (oldPart != newPart) {
            const auto change = changedRange(oldPart, newPart);
            changes.push_back({oldAt + change.begin, oldAt + change.oldEnd, newAt + change.newEnd});
        }
        oldAt += oldPart.size();
        newAt += newPart.size();
    }
    return changes;
}

int changedOffset(const std::vector<TextChange> &changes, int offset)
{
    qsizetype shift = 0;
    for (const auto &change : changes) {
        if (offset < change.begin || (offset == change.begin && change.oldEnd > change.begin)) break;
        if (offset < change.oldEnd) return static_cast<int>(change.newEnd);
        shift = change.newEnd - change.oldEnd;
    }
    return static_cast<int>(offset + shift);
}
}
