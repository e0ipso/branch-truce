// Computes connector passages and their visible geometry for a pair of panes.
#include "ConnectorBands.h"
#include "PaneMarks.h"
#include <QMap>
#include <QPainterPath>
#include <algorithm>

namespace branch_truce {
namespace {
int number(const QVariantMap &m, const char *key) { return m.value(QLatin1String(key)).toInt(); }
QString string(const QVariantMap &m, const char *key) { return m.value(QLatin1String(key)).toString(); }
bool meets(int begin, int end, int first, int stop)
{
    return begin < end ? begin < stop && first < end : begin > first && begin < stop;
}
}
ConnectorBands::ConnectorBands(QObject *parent) : QObject(parent) {}
void ConnectorBands::setSourceBlocks(TextBlocks *blocks)
{
    if (sourceBlocks_ == blocks) return;
    disconnect(sourceConnection_);
    sourceBlocks_ = blocks;
    if (blocks) sourceConnection_ = connect(blocks, &TextBlocks::layoutChanged, this, &ConnectorBands::update, Qt::QueuedConnection);
    update();
    Q_EMIT inputsChanged();
}
void ConnectorBands::setResultBlocks(TextBlocks *blocks)
{
    if (resultBlocks_ == blocks) return;
    disconnect(resultConnection_);
    resultBlocks_ = blocks;
    if (blocks) resultConnection_ = connect(blocks, &TextBlocks::layoutChanged, this, &ConnectorBands::update, Qt::QueuedConnection);
    update();
    Q_EMIT inputsChanged();
}
QVariantMap ConnectorBands::bandForLine(int line, bool result) const
{
    for (const auto &value : bands_) {
        const auto band = value.toMap();
        const int begin = number(band, result ? "resultBegin" : "sourceBegin");
        const int end = number(band, result ? "resultEnd" : "sourceEnd");
        if (line >= begin && line < std::max(begin + 1, end)) return band;
    }
    return {};
}
QVariantMap ConnectorBands::bandAt(qreal x, qreal y, qreal width, bool leftSide) const
{
    if (width <= 0 || x < 0 || x > width) return {};
    const qreal sourceX = leftSide ? x : width - x;
    y -= sourceParentY_;
    if (y < 0 || y > viewportHeight_) return {};
    for (const auto &value : visibleBands_) {
        const auto band = value.toMap();
        const qreal sourceTop = band.value(QStringLiteral("sourceTop")).toReal();
        const qreal sourceBottom = band.value(QStringLiteral("sourceBottom")).toReal();
        const qreal resultTop = band.value(QStringLiteral("resultTop")).toReal();
        const qreal resultBottom = band.value(QStringLiteral("resultBottom")).toReal();
        // Keep the attachment edges inclusive, as with the former straight bands.
        if ((sourceX == 0 && y >= sourceTop && y <= sourceBottom)
            || (sourceX == width && y >= resultTop && y <= resultBottom)) return band;
        // Match ConnectorHandle's cubic edges in source-to-result coordinates.
        // Clipping happens after constructing the full, scrolled passage bounds.
        QPainterPath ribbon;
        ribbon.moveTo(0, sourceTop);
        ribbon.cubicTo(width / 2, sourceTop, width / 2, resultTop, width, resultTop);
        ribbon.lineTo(width, resultBottom);
        ribbon.cubicTo(width / 2, resultBottom, width / 2, sourceBottom, 0, sourceBottom);
        ribbon.closeSubpath();
        if (ribbon.contains(QPointF(sourceX, y))) return band;
    }
    return {};
}
void ConnectorBands::update()
{
    QVariantList all = bands_;
    if (projectionDirty_) {
        all.clear();
        QMap<int, QString> lines, boundaries;
        const auto decide = [](QMap<int, QString> &states, int line, const QString &kind) {
            if (!states.contains(line) || PaneMarks::rank(states.value(line)) < PaneMarks::rank(kind)) states[line] = kind;
        };
        const auto conflicts = PaneMarks::conflictingLines(passages_);
        for (const auto &value : PaneMarks::stylingRegions(regions_)) {
            const auto region = value.toMap();
            if (string(region, "side") != side_) continue;
            const auto state = string(region, "state");
            if (state == QLatin1String("undecided") && !PaneMarks::conflictingRegion(region, conflicts)) continue;
            const auto kind = state == QLatin1String("undecided") ? QStringLiteral("conflict") : state;
            const int begin = number(region, "sourceLineBegin"), end = number(region, "sourceLineEnd");
            if (begin == end) decide(boundaries, begin, kind);
            for (int line = begin; line < end; ++line) decide(lines, line, kind);
        }
        const auto rows = fold_.value(QStringLiteral("rows")).toMap();
        QMap<int, int> folded;
        QVariantList projected;
        for (const auto &value : PaneMarks::decoratedPassages(passages_)) {
            const auto passage = value.toMap();
            if (string(passage, "side") != side_) continue;
            const int sb = number(passage, "sourceLineBegin"), se = number(passage, "sourceLineEnd");
            const int rb = number(passage, "resultLineBegin"), re = number(passage, "resultLineEnd");
            const auto kind = string(passage, "kind");
            const bool equal = passage.value(QStringLiteral("comparisonKind"), kind).toString() == QLatin1String("equal");
            if (!equal || sb >= se || se - sb != re - rb) {
                projected.append(value);
                continue;
            }
            // Equal correspondence is exact line by line. Split it at decision
            // boundaries so accepting one line never tints its equal neighbours.
            const auto kindAt = [&](int line) {
                const auto state = lines.value(line);
                return PaneMarks::rank(state) > PaneMarks::rank(kind) ? state : kind;
            };
            for (int first = sb; first < se;) {
                const auto state = kindAt(first);
                int stop = first + 1;
                while (stop < se && kindAt(stop) == state) ++stop;
                auto part = passage;
                part[QStringLiteral("kind")] = state;
                part[QStringLiteral("sourceLineBegin")] = first;
                part[QStringLiteral("sourceLineEnd")] = stop;
                part[QStringLiteral("resultLineBegin")] = rb + first - sb;
                part[QStringLiteral("resultLineEnd")] = rb + stop - sb;
                projected.append(part);
                first = stop;
            }
        }
        for (const auto &value : projected) {
            const auto passage = value.toMap();
            if (string(passage, "side") != side_) continue;
            const auto passageKind = string(passage, "kind");
            const int sb = number(passage, "sourceLineBegin"), se = number(passage, "sourceLineEnd");
            const int rb = number(passage, "resultLineBegin"), re = number(passage, "resultLineEnd");
            int row = 0, stop = 0;
            for (auto it = rows.cbegin(); it != rows.cend(); ++it) {
                const int first = it.key().toInt(), last = number(it.value().toMap(), "last") + 1;
                if (meets(rb, re, first, last)) { row = first; stop = last; break; }
            }
            if (row > 0 && (passageKind != QLatin1String("equal") || (rb > row && re <= stop))) {
                if (folded.contains(row)) {
                    const int i = folded.value(row);
                    auto band = all.at(i).toMap();
                    band[QStringLiteral("sourceBegin")] = std::min(number(band, "sourceBegin"), sb);
                    band[QStringLiteral("sourceEnd")] = std::max(number(band, "sourceEnd"), se);
                    band[QStringLiteral("resultBegin")] = std::min(number(band, "resultBegin"), rb);
                    band[QStringLiteral("resultEnd")] = std::max(number(band, "resultEnd"), re);
                    all[i] = band;
                } else {
                    folded[row] = static_cast<int>(all.size());
                    all.append(QVariantMap{{"kind", "conflict"}, {"sourceBegin", sb}, {"sourceEnd", se},
                        {"resultBegin", std::min(row, rb)}, {"resultEnd", std::max(stop, re)}});
                }
                continue;
            }
            if (passageKind == QLatin1String("equal")) continue;
            QString kind = passageKind == QLatin1String("result-only") || passageKind == QLatin1String("added") ? QStringLiteral("added")
                : passageKind == QLatin1String("source-only") ? QStringLiteral("removed") : QStringLiteral("changed");
            const auto &states = sb == se ? boundaries : lines;
            for (int line = sb; line < std::max(se, sb + 1); ++line)
                if (states.contains(line) && PaneMarks::rank(states.value(line)) > PaneMarks::rank(kind)) kind = states.value(line);
            for (const auto &blockValue : markerBlocks_) {
                const auto block = blockValue.toMap();
                if (meets(rb, re, number(block, "resultLineBegin"), number(block, "resultLineEnd"))) {
                    kind = QStringLiteral("conflict");
                    break;
                }
            }
            all.append(QVariantMap{{"kind", kind}, {"sourceBegin", sb}, {"sourceEnd", se}, {"resultBegin", rb}, {"resultEnd", re}});
        }
        projectionDirty_ = false;
    }
    QVariantList visible;
    if (sourceBlocks_ && resultBlocks_ && viewportHeight_ > 0) {
        const auto edge = [&](bool source, int begin, int end, bool bottom) {
            const auto *blocks = source ? sourceBlocks_.data() : resultBlocks_.data();
            const qreal parent = source ? 0 : resultParentY_ - sourceParentY_;
            const qreal offset = source ? sourceEditorY_ + sourcePadding_ - sourceScroll_
                                        : resultEditorY_ + resultPadding_ - resultScroll_;
            // Half-open endpoints use the laid-out block boundaries, including
            // wrapped rows and the single visible row of a folded marker block.
            return parent + offset + blocks->yAtLine(bottom ? end : begin)
                + (begin < end ? 0 : (bottom ? rule_ : -rule_) / 2);
        };
        const auto shownIndex = [&](bool below) {
            const qreal extent = below ? viewportHeight_ : 0;
            const qreal sourceLine = sourceBlocks_->lineAtY(sourceScroll_ + extent - sourceEditorY_ - sourcePadding_);
            const qreal resultLine = resultBlocks_->lineAtY(resultScroll_ + extent - resultEditorY_ - resultPadding_
                - resultParentY_ + sourceParentY_);
            qsizetype low = 0, high = all.size();
            while (low < high) {
                const qsizetype middle = low + (high - low) / 2;
                const auto band = all.at(middle).toMap();
                const bool found = below ? number(band, "sourceBegin") > sourceLine && number(band, "resultBegin") > resultLine
                    : number(band, "sourceEnd") >= sourceLine || number(band, "resultEnd") >= resultLine;
                if (found) high = middle; else low = middle + 1;
            }
            return low;
        };
        const qsizetype first = shownIndex(false), last = shownIndex(true);
        for (qsizetype i = first; i < last; ++i) {
            auto band = all.at(i).toMap();
            const int sb = number(band, "sourceBegin"), se = number(band, "sourceEnd");
            const int rb = number(band, "resultBegin"), re = number(band, "resultEnd");
            const qreal st = edge(true, sb, se, false), sl = edge(true, sb, se, true);
            const qreal rt = edge(false, rb, re, false), rl = edge(false, rb, re, true);
            if (std::max(sl, rl) < 0 || std::min(st, rt) > viewportHeight_) continue;
            bool current = false;
            for (const auto &rangeValue : current_) {
                const auto range = rangeValue.toList();
                if (range.size() >= 2 && sb < std::max(range[1].toInt(), range[0].toInt() + 1)
                    && range[0].toInt() < std::max(se, sb + 1)) { current = true; break; }
            }
            band.insert(QStringLiteral("current"), current);
            band.insert(QStringLiteral("sourceTop"), st); band.insert(QStringLiteral("sourceBottom"), sl);
            band.insert(QStringLiteral("resultTop"), rt); band.insert(QStringLiteral("resultBottom"), rl);
            visible.append(band);
        }
    }
    if (bands_ == all && visibleBands_ == visible) return;
    bands_ = all;
    visibleBands_ = visible;
    Q_EMIT changed();
}
}
