#include "ComparisonPanes.h"
#include "TextBlocks.h"
#include <QScopedValueRollback>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <limits>

namespace branch_truce {
namespace {
const QString leftName = QStringLiteral("stage2");
const QString resultName = QStringLiteral("result");
const QString rightName = QStringLiteral("stage3");
const QString earlierName = QStringLiteral("earlier");

template<typename Type>
Type *partOf(QQuickItem *pane, const char *name)
{
    return qobject_cast<Type *>(pane->property(name).value<QObject *>());
}
}

ComparisonPanes::ComparisonPanes(QObject *parent) : QObject(parent) {}

QQuickItem *ComparisonPanes::left() const { return left_; }
void ComparisonPanes::setLeft(QQuickItem *pane) { setPane(left_, pane); }
QQuickItem *ComparisonPanes::result() const { return result_; }
void ComparisonPanes::setResult(QQuickItem *pane) { setPane(result_, pane); }
QQuickItem *ComparisonPanes::right() const { return right_; }
void ComparisonPanes::setRight(QQuickItem *pane) { setPane(right_, pane); }
QQuickItem *ComparisonPanes::earlier() const { return earlier_; }
void ComparisonPanes::setEarlier(QQuickItem *pane) { setPane(earlier_, pane); }

void ComparisonPanes::setPane(QPointer<QQuickItem> &slot, QQuickItem *pane)
{
    if (slot == pane) return;
    slot = pane;
    Q_EMIT panesChanged();
}

QVariantList ComparisonPanes::passages() const { return passages_; }

void ComparisonPanes::setPassages(const QVariantList &passages)
{
    if (passages == passages_) return;
    passages_ = passages;
    spans_.clear();
    for (const auto &entry : passages) {
        const auto passage = entry.toMap();
        const auto side = passage.value(QStringLiteral("side")).toString();
        if (side != leftName && side != rightName && side != earlierName) continue;
        spans_[side].append({passage.value(QStringLiteral("sourceLineBegin")).toReal(),
            passage.value(QStringLiteral("sourceLineEnd")).toReal(),
            passage.value(QStringLiteral("resultLineBegin")).toReal(),
            passage.value(QStringLiteral("resultLineEnd")).toReal()});
    }
    Q_EMIT passagesChanged();
}

qreal ComparisonPanes::lineSpacing() const { return lineSpacing_; }

void ComparisonPanes::setLineSpacing(qreal spacing)
{
    if (qFuzzyCompare(spacing, lineSpacing_)) return;
    lineSpacing_ = spacing;
    Q_EMIT lineSpacingChanged();
}

qreal ComparisonPanes::mapLine(const QList<Span> &spans, qreal line, bool toResult, int fromLines, int toLines)
{
    if (spans.isEmpty()) return fromLines > 0 ? 1 + (line - 1) * toLines / fromLines : 1;
    const auto fromBegin = [toResult](const Span &span) { return toResult ? span.sourceBegin : span.resultBegin; };
    const auto fromEnd = [toResult](const Span &span) { return toResult ? span.sourceEnd : span.resultEnd; };
    const auto toBegin = [toResult](const Span &span) { return toResult ? span.resultBegin : span.sourceBegin; };
    const auto toEnd = [toResult](const Span &span) { return toResult ? span.resultEnd : span.sourceEnd; };
    // The first passage that ends below the line.
    const auto found = std::upper_bound(spans.cbegin(), spans.cend(), line,
        [&](qreal value, const Span &span) { return value < fromEnd(span); });
    if (found == spans.cend()) return toEnd(spans.last()) + line - fromEnd(spans.last());
    if (line <= fromBegin(*found)) return toBegin(*found);
    return toBegin(*found) + (line - fromBegin(*found)) * (toEnd(*found) - toBegin(*found))
        / (fromEnd(*found) - fromBegin(*found));
}

qreal ComparisonPanes::toResult(const QString &side, qreal line, int sideLines, int resultLines) const
{
    return side == resultName ? line : mapLine(spans_.value(side), line, true, sideLines, resultLines);
}

qreal ComparisonPanes::fromResult(const QString &side, qreal line, int resultLines, int sideLines) const
{
    return side == resultName ? line : mapLine(spans_.value(side), line, false, resultLines, sideLines);
}

qreal ComparisonPanes::mapBetween(const QString &from, const QString &to, qreal line, int fromLines,
                                  int resultLines, int toLines) const
{
    return fromResult(to, toResult(from, line, fromLines, resultLines), resultLines, toLines);
}

QQuickItem *ComparisonPanes::pane(const QString &name) const
{
    if (name == leftName) return left_;
    if (name == rightName) return right_;
    if (name == earlierName) return earlier_;
    return result_;
}

QQuickItem *ComparisonPanes::editor(const QString &name) const
{
    auto *item = pane(name);
    return item ? partOf<QQuickItem>(item, "editor") : nullptr;
}

std::optional<ComparisonPanes::Pane> ComparisonPanes::partsOf(const QString &name, QQuickItem *pane)
{
    if (!pane) return std::nullopt;
    Pane parts{name, partOf<QQuickItem>(pane, "viewport"), partOf<QQuickItem>(pane, "editor"),
        partOf<TextBlocks>(pane, "blocks")};
    if (!parts.viewport || !parts.editor || !parts.blocks) return std::nullopt;
    return parts;
}

QList<ComparisonPanes::Pane> ComparisonPanes::shownPanes() const
{
    QList<Pane> panes;
    for (const auto &name : {leftName, resultName, rightName, earlierName}) {
        auto *item = pane(name);
        if (name == earlierName && (!item || !item->isVisible())) continue;
        if (auto parts = partsOf(name, item)) panes.append(*parts);
    }
    return panes;
}

// Up to three lines of context above the row, and at most a quarter of the shortest pane.
qreal ComparisonPanes::referenceRow(const QList<Pane> &panes) const
{
    auto shortest = std::numeric_limits<qreal>::infinity();
    for (const auto &pane : panes) shortest = std::min(shortest, pane.viewport->height());
    return std::max<qreal>(0, std::min(3 * lineSpacing_, shortest / 4));
}

qreal ComparisonPanes::lineOnRow(const Pane &pane, qreal row)
{
    const auto contentY = pane.viewport->property("contentY").toReal();
    const auto padding = pane.editor->property("topPadding").toReal();
    return pane.blocks->lineAtY(contentY + row - pane.editor->y() - padding);
}

void ComparisonPanes::putLineOnRow(const Pane &pane, qreal line, qreal row)
{
    const auto padding = pane.editor->property("topPadding").toReal();
    const auto y = pane.editor->y() + padding + pane.blocks->yAtLine(line) - row;
    const auto contentHeight = pane.viewport->property("contentHeight").toReal();
    const auto bottom = std::max<qreal>(0, contentHeight - pane.viewport->height());
    pane.viewport->setProperty("contentY", std::max<qreal>(0, std::min(bottom, y)));
}

// A selection stays where the user made it.
void ComparisonPanes::keepCursorOnLine(const Pane &pane, qreal line)
{
    const auto *editor = pane.editor;
    if (editor->property("selectionStart").toInt() != editor->property("selectionEnd").toInt()) return;
    pane.editor->setProperty("cursorPosition", pane.blocks->positionOf(static_cast<int>(line)));
}

void ComparisonPanes::follow(const QString &leader, const QString &only)
{
    if (following_) return;
    const auto panes = shownPanes();
    const auto lead = std::find_if(panes.cbegin(), panes.cend(), [&](const Pane &pane) { return pane.name == leader; });
    const auto resultPane = std::find_if(panes.cbegin(), panes.cend(), [](const Pane &pane) { return pane.name == resultName; });
    if (lead == panes.cend() || resultPane == panes.cend()) return;
    const auto row = referenceRow(panes);
    const int resultLines = resultPane->blocks->blockCount();
    const auto resultLine = toResult(leader, lineOnRow(*lead, row), lead->blocks->blockCount(), resultLines);
    QScopedValueRollback guard(following_, true);
    for (const auto &pane : panes) {
        if (pane.name == leader || (!only.isEmpty() && pane.name != only)) continue;
        putLineOnRow(pane, fromResult(pane.name, resultLine, resultLines, pane.blocks->blockCount()), row);
    }
}

void ComparisonPanes::paneScrolled(const QString &name, bool byUser)
{
    if (byUser) follow(name);
}

void ComparisonPanes::place(const QString &lead, qreal line)
{
    const auto panes = shownPanes();
    const auto leader = std::find_if(panes.cbegin(), panes.cend(), [&](const Pane &pane) { return pane.name == lead; });
    if (leader == panes.cend()) return;
    const auto row = referenceRow(panes);
    putLineOnRow(*leader, line, row);
    follow(lead);
    for (const auto &pane : panes) {
        if (pane.name != resultName) keepCursorOnLine(pane, std::floor(lineOnRow(pane, row)));
    }
}

void ComparisonPanes::paneShown(const QString &name)
{
    const auto panes = shownPanes();
    const auto shown = std::find_if(panes.cbegin(), panes.cend(), [&](const Pane &pane) { return pane.name == name; });
    if (shown == panes.cend()) return;
    follow(resultName, name);
    keepCursorOnLine(*shown, std::ceil(lineOnRow(*shown, referenceRow(panes))));
}
}
