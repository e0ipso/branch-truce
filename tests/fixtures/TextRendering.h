#pragma once

#include "fixtures/QuickScene.h"

namespace test_support {
inline int visualLines(QQuickItem *editor, int blockNumber)
{
    auto *quick = editor->property("textDocument").value<QQuickTextDocument *>();
    if (!quick || !quick->textDocument()) throw std::runtime_error("missing text document");
    const auto *layout = quick->textDocument()->findBlockByNumber(blockNumber).layout();
    return layout ? layout->lineCount() : 0;
}

inline int paintedMarkers(QQuickItem *markers)
{
    int count = 0;
    auto *spots = markers->findChild<QQuickItem *>(markers->objectName() + QStringLiteral("Spots"));
    if (!spots) throw std::runtime_error("missing whitespace spots");
    for (auto *child : spots->childItems()) {
        if (child->isVisible() && child->width() >= 2.0 && child->height() >= 2.0) ++count;
    }
    return count;
}

inline int markerKind(QQuickItem *markers, bool tab)
{
    const auto expression = tab ? QStringLiteral("spots.filter(function(spot) { return spot.tab; }).length")
                                : QStringLiteral("spots.filter(function(spot) { return !spot.tab; }).length");
    QQmlExpression query(qmlContext(markers), markers, expression);
    const auto value = query.evaluate();
    if (query.hasError()) throw std::runtime_error(query.error().toString().toStdString());
    return value.toInt();
}
} // namespace test_support
