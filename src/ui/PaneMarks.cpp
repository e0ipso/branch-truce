// Computes comparison pane line marks, folds and navigation ranges.
#include "PaneMarks.h"
#include "TextBlocks.h"
#include <QJSValue>
#include <QMap>
#include <algorithm>

namespace branch_truce {
namespace {
int number(const QVariantMap &map, const char *key) { return map.value(QLatin1String(key)).toInt(); }
QString string(const QVariantMap &map, const char *key) { return map.value(QLatin1String(key)).toString(); }
}
int PaneMarks::rank(const QString &kind)
{
    if (kind == QLatin1String("markers")) return 6;
    if (kind == QLatin1String("conflict")) return 5;
    if (kind == QLatin1String("manual")) return 4;
    if (kind == QLatin1String("accepted") || kind == QLatin1String("ignored")) return 3;
    if (kind == QLatin1String("changed")) return 2;
    if (kind == QLatin1String("added")) return 1;
    return 0;
}
namespace {
QVariantList range(int first, int end) { return {first, end}; }
}
QVariantList PaneMarks::stylingRegions(const QVariantList &regions)
{
    QSet<QString> lineSections;
    for (const auto &value : regions) {
        const auto r = value.toMap();
        if (string(r, "scope") == QLatin1String("line"))
            lineSections.insert(string(r, "side") + QLatin1Char(':') + string(r, "section"));
    }
    QVariantList styled;
    for (const auto &value : regions) {
        const auto r = value.toMap();
        const auto state = string(r, "state");
        // A fully decided section may contain both accepted and ignored lines.
        // Its aggregate state must not repaint those precise decisions.
        if (string(r, "scope") == QLatin1String("section")
            && (state == QLatin1String("accepted") || state == QLatin1String("ignored"))
            && lineSections.contains(string(r, "side") + QLatin1Char(':') + string(r, "section"))) continue;
        styled.append(value);
    }
    return styled;
}
QVariantList PaneMarks::decoratedPassages(const QVariantList &passages)
{
    // A source can already match an automatically merged result. Its equal
    // correspondence still carries the result's change against the earlier
    // version. Keep that change separate from source-decision state.
    QMap<int, QString> resultKinds;
    for (const auto &value : passages) {
        const auto p = value.toMap();
        if (string(p, "side") != QLatin1String("earlier")) continue;
        const auto kind = string(p, "kind");
        if (kind == QLatin1String("equal") || kind == QLatin1String("source-only")) continue;
        const auto decoration = kind == QLatin1String("result-only") ? QStringLiteral("added") : QStringLiteral("changed");
        for (int line = number(p, "resultLineBegin"); line < number(p, "resultLineEnd"); ++line)
            resultKinds.insert(line, decoration);
    }
    QVariantList decorated;
    for (const auto &value : passages) {
        const auto p = value.toMap();
        const int begin = number(p, "resultLineBegin"), end = number(p, "resultLineEnd");
        if (string(p, "side") == QLatin1String("earlier") || string(p, "kind") != QLatin1String("equal")
            || begin >= end || resultKinds.isEmpty()) {
            decorated.append(value);
            continue;
        }
        int first = begin;
        while (first < end) {
            const auto kind = resultKinds.value(first, QStringLiteral("equal"));
            int stop = first + 1;
            while (stop < end && resultKinds.value(stop, QStringLiteral("equal")) == kind) ++stop;
            // These are line-only decoration ranges, never editing targets.
            decorated.append(QVariantMap{{QStringLiteral("side"), p.value(QStringLiteral("side"))},
                {QStringLiteral("kind"), kind}, {QStringLiteral("comparisonKind"), QStringLiteral("equal")},
                {QStringLiteral("sourceLineBegin"), number(p, "sourceLineBegin") + first - begin},
                {QStringLiteral("sourceLineEnd"), number(p, "sourceLineBegin") + stop - begin},
                {QStringLiteral("resultLineBegin"), first}, {QStringLiteral("resultLineEnd"), stop}});
            first = stop;
        }
    }
    return decorated;
}
QSet<int> PaneMarks::conflictingLines(const QVariantList &passages)
{
    // Where the result still equals the earlier version, changes from both
    // sources are competing. An undecided choice alone proves no conflict.
    QMap<int, int> evidence;
    for (const auto &value : passages) {
        const auto p = value.toMap();
        const auto side = string(p, "side"), kind = string(p, "kind");
        const bool earlier = side == QLatin1String("earlier");
        const bool changed = kind == QLatin1String("changed") || kind == QLatin1String("result-only");
        const int bit = earlier && kind == QLatin1String("equal") ? 1
            : changed && side == QLatin1String("stage2") ? 2
            : changed && side == QLatin1String("stage3") ? 4 : 0;
        if (!bit) continue;
        for (int line = number(p, "resultLineBegin"); line < number(p, "resultLineEnd"); ++line)
            evidence[line] |= bit;
    }
    QSet<int> conflicts;
    for (auto it = evidence.cbegin(); it != evidence.cend(); ++it)
        if (it.value() == 7) conflicts.insert(it.key());
    return conflicts;
}
bool PaneMarks::conflictingRegion(const QVariantMap &region, const QSet<int> &lines)
{
    if (region.value(QStringLiteral("markersRemain")).toBool()) return true;
    if (!region.value(QStringLiteral("mapped")).toBool()) return false;
    for (int line = number(region, "resultLineBegin"); line < number(region, "resultLineEnd"); ++line)
        if (lines.contains(line)) return true;
    return false;
}
QVariantMap PaneMarks::marks(const QString &pane, const QVariantList &passages, const QVariantList &regions,
                             const QVariantList &edited, const QVariantList &blocks, const QVariantList &orphans) const
{
    QVariantMap lines, gaps, letters;
    QMap<int, int> from;
    const bool result = pane == QLatin1String("result");
    const auto mark = [&](int first, int end, const QString &kind) {
        for (int line = first; line < end; ++line) {
            const auto key = QString::number(line);
            if (!lines.contains(key) || rank(lines.value(key).toString()) < rank(kind)) lines.insert(key, kind);
        }
    };
    for (const auto &value : decoratedPassages(passages)) {
        const auto p = value.toMap();
        const auto side = string(p, "side"), kind = string(p, "kind");
        if (result ? side == QLatin1String("earlier") : side != pane) continue;
        const int first = number(p, result ? "resultLineBegin" : "sourceLineBegin");
        const int end = number(p, result ? "resultLineEnd" : "sourceLineEnd");
        if (p.value(QStringLiteral("comparisonKind"), kind).toString() == QLatin1String("equal")) {
            if (result) for (int line = first; line < end; ++line) from[line] |= side == QLatin1String("stage2") ? 1 : 2;
            if (kind != QLatin1String("equal")) mark(first, end, kind);
        } else if (kind == QLatin1String(result ? "source-only" : "result-only")) gaps.insert(QString::number(first), true);
        else mark(first, end, kind == QLatin1String("changed") || kind == QLatin1String("ambiguous")
                  ? QStringLiteral("changed") : QStringLiteral("added"));
    }
    const auto conflicts = conflictingLines(passages);
    for (const auto &value : stylingRegions(regions)) {
        const auto r = value.toMap();
        const auto state = string(r, "state");
        // Optional contributions remain inspectable in regions and choices.
        if (state == QLatin1String("undecided") && !conflictingRegion(r, conflicts)) continue;
        if (result && r.value(QStringLiteral("mapped")).toBool()) {
            const int first = number(r, "resultLineBegin"), end = number(r, "resultLineEnd");
            if (state == QLatin1String("undecided")) mark(first, end, QStringLiteral("conflict"));
            else if (state == QLatin1String("manual")) mark(first, end, QStringLiteral("manual"));
            else if (state == QLatin1String("accepted")) mark(first, end, QStringLiteral("accepted"));
            if (r.value(QStringLiteral("handEdited")).toBool()) for (int line = first; line < end; ++line) from[line] = 4;
        } else if (!result && string(r, "side") == pane && state != QLatin1String("manual")) {
            mark(number(r, "sourceLineBegin"), number(r, "sourceLineEnd"),
                state == QLatin1String("undecided") ? QStringLiteral("conflict") : state);
        }
    }
    if (result) {
        for (const auto &value : edited) {
            const auto e = value.toMap();
            const int first = number(e, "resultLineBegin"), end = std::max(number(e, "resultLineEnd"), first + 1);
            mark(first, end, QStringLiteral("manual"));
            for (int line = first; line < end; ++line) from[line] = 4;
        }
        for (const auto &value : blocks) {
            const auto b = value.toMap();
            mark(number(b, "resultLineBegin"), number(b, "resultLineEnd"), QStringLiteral("markers"));
        }
        for (const auto &value : orphans) {
            const int line = number(value.toMap(), "line");
            mark(line, line + 1, QStringLiteral("markers"));
        }
    }
    for (auto it = from.cbegin(); it != from.cend(); ++it) {
        const char *const glyph[] = {"", "L", "R", "B", "m"};
        letters.insert(QString::number(it.key()), QString::fromLatin1(glyph[it.value()]));
    }
    return {{QStringLiteral("lines"), lines}, {QStringLiteral("gaps"), gaps}, {QStringLiteral("letters"), letters}};
}
QVariantMap PaneMarks::folds(const QVariantList &blocks, bool shown) const
{
    QVariantList hidden;
    QVariantMap rows;
    if (!shown) for (const auto &value : blocks) {
        const auto block = value.toMap();
        const auto lines = block.value(QStringLiteral("markerLines")).toList();
        if (!block.value(QStringLiteral("folded")).toBool() || lines.size() < 2) continue;
        const auto first = lines.first().toMap(), last = lines.last().toMap();
        hidden.append(QVariantMap{{QStringLiteral("line"), first.value(QStringLiteral("line"))},
            {QStringLiteral("text"), first.value(QStringLiteral("text"))}, {QStringLiteral("last"), last.value(QStringLiteral("line"))},
            {QStringLiteral("lastText"), last.value(QStringLiteral("text"))}});
        rows.insert(QString::number(number(first, "line")), QVariantMap{{QStringLiteral("last"), last.value(QStringLiteral("line"))},
            {QStringLiteral("sections"), block.value(QStringLiteral("sections"))}});
    }
    std::sort(hidden.begin(), hidden.end(), [](const QVariant &a, const QVariant &b) { return number(a.toMap(), "line") < number(b.toMap(), "line"); });
    return {{QStringLiteral("hidden"), hidden}, {QStringLiteral("rows"), rows}};
}
QVariantList PaneMarks::current(const QString &pane, const QVariant &targetValue, const QString &fileId,
                                const QVariantList &passages, const QVariantList &regions, TextBlocks *blocks) const
{
    QVariantList ranges;
    const auto target = targetValue.metaType() == QMetaType::fromType<QJSValue>()
        ? targetValue.value<QJSValue>().toVariant().toMap() : targetValue.toMap();
    if (target.isEmpty()) return ranges;
    const auto scope = string(target, "scope");
    if (scope == QLatin1String("decoration")) {
        if (pane == string(target, "side")) return {QVariant::fromValue(range(number(target, "sourceBegin"), number(target, "sourceEnd")))};
        if (pane == QLatin1String("result")) return number(target, "resultBegin") > 0
            ? QVariantList{QVariant::fromValue(range(number(target, "resultBegin"), number(target, "resultEnd")))} : ranges;
    } else if (string(target, "fileId") != fileId || number(target, "count") <= 0) return ranges;
    int first = scope == QLatin1String("decoration") ? number(target, "resultBegin") : -1;
    int end = scope == QLatin1String("decoration") ? number(target, "resultEnd") : -1;
    if (scope == QLatin1String("result") && number(target, "utf16Begin") >= 0 && blocks) {
        const int begin = number(target, "utf16Begin"), stop = number(target, "utf16End");
        first = blocks->lineAt(begin);
        end = stop > begin ? blocks->lineAt(stop - 1) + 1 : first;
    }
    if (pane == QLatin1String("result")) {
        if (first > 0) ranges.append(QVariant::fromValue(range(first, end)));
        return ranges;
    }
    for (const auto &value : regions) {
        const auto r = value.toMap();
        if (string(r, "id") == string(target, "contributionId") && string(r, "side") == pane) {
            ranges.append(QVariant::fromValue(range(number(r, "sourceLineBegin"), number(r, "sourceLineEnd"))));
            return ranges;
        }
    }
    if (first <= 0) return ranges;
    for (const auto &value : passages) {
        const auto p = value.toMap();
        if (string(p, "side") != pane) continue;
        const int begin = number(p, "resultLineBegin"), stop = number(p, "resultLineEnd");
        const bool meets = stop > begin ? begin < std::max(end, first + 1) && first < stop : begin >= first && begin <= end;
        if (!meets) continue;
        const int source = number(p, "sourceLineBegin");
        ranges.append(QVariant::fromValue(string(p, "kind") == QLatin1String("equal")
            ? range(source + std::max(first, begin) - begin, source + std::min(end, stop) - begin)
            : range(source, number(p, "sourceLineEnd"))));
    }
    return ranges;
}
bool PaneMarks::contains(const QVariantList &ranges, int line) const
{
    for (const auto &value : ranges) { const auto r = value.toList(); if (r.size() >= 2 && line >= r[0].toInt() && line < r[1].toInt()) return true; }
    return false;
}
bool PaneMarks::gap(const QVariantList &ranges, int line) const
{
    for (const auto &value : ranges) { const auto r = value.toList(); if (r.size() >= 2 && r[0].toInt() == line && r[1].toInt() == line) return true; }
    return false;
}
bool PaneMarks::overlaps(const QVariantList &ranges, int first, int last) const
{
    for (const auto &value : ranges) { const auto r = value.toList(); if (r.size() >= 2 && r[0].toInt() <= last && first < std::max(r[1].toInt(), r[0].toInt() + 1)) return true; }
    return false;
}
}
