#pragma once

#include "ComparisonScene.h"
#include <QMap>
#include <QQmlExpression>
#include <QTextBlock>
#include <QTextLayout>

namespace comparison_scene {
// Evaluates a QML expression on item, such as one of its attached properties.
inline QVariant qmlValue(QQuickItem *item, const QString &expression)
{
    QQmlExpression evaluated(qmlContext(item), item, expression);
    const auto value = evaluated.evaluate();
    if (evaluated.hasError()) throw std::runtime_error(evaluated.error().toString().toStdString());
    return value;
}

// Where the editor draws the character at position, in scene coordinates.
inline QRectF sceneRectangle(QQuickItem *editor, int position)
{
    QRectF rect;
    CHECK(QMetaObject::invokeMethod(editor, "positionToRectangle", Q_RETURN_ARG(QRectF, rect), Q_ARG(int, position)));
    return {editor->mapToScene(rect.topLeft()), rect.size()};
}

// The line numbers a pane's gutter shows, each with its scene y.
inline QMap<int, qreal> gutterNumbers(QQuickItem *gutter)
{
    QMap<int, qreal> shown;
    for (auto *child : gutter->childItems()) {
        bool number = false;
        const auto value = child->property("text").toString().toInt(&number);
        if (number && child->isVisible()) shown.insert(value, child->mapToScene(QPointF()).y());
    }
    return shown;
}

// The first line in the pane's view whose number is missing or not on the
// line's first row, or empty when every line in view has its number there.
inline std::string gutterMisalignment(Fixture &fixture, const std::string &pane)
{
    auto *editor = fixture.item((pane + "Text").c_str());
    auto *viewport = fixture.item((pane + "Viewport").c_str());
    const auto shown = gutterNumbers(fixture.item((pane + "Gutter").c_str()));
    const auto top = viewport->mapToScene(QPointF()).y();
    const auto bottom = top + viewport->height();
    const auto *document = editor->property("textDocument").value<QQuickTextDocument *>()->textDocument();
    int inView = 0;
    for (auto block = document->begin(); block.isValid(); block = block.next()) {
        const auto row = sceneRectangle(editor, block.position());
        if (row.bottom() <= top || row.top() >= bottom) continue;
        ++inView;
        const auto number = block.blockNumber() + 1;
        if (!shown.contains(number)) return pane + " shows no number " + std::to_string(number);
        if (std::abs(shown.value(number) - row.top()) > 0.5)
            return pane + " shows number " + std::to_string(number) + " at y " + std::to_string(shown.value(number))
                + " for a line at y " + std::to_string(row.top());
    }
    return inView > 0 ? std::string() : pane + " has no line in view";
}

// How far right of the start of the editor's line holding text the character at column is drawn.
inline qreal columnOffset(QQuickItem *editor, const QString &text, int column)
{
    const auto start = static_cast<int>(editor->property("text").toString().indexOf(text));
    CHECK(start >= 0);
    return sceneRectangle(editor, start + column).x() - sceneRectangle(editor, start).x();
}

} // namespace comparison_scene
