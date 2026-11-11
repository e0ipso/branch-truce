#pragma once

#include "ComparisonEditor.h"
#include <QTextBlock>
#include <QWheelEvent>

namespace comparison_scene {
// count lines "<prefix> <n>", numbered from first.
inline std::string numberedLines(const std::string &prefix, int first, int count)
{
    std::string lines;
    for (int line = first; line < first + count; ++line) lines += prefix + ' ' + std::to_string(line) + '\n';
    return lines;
}

inline qreal paneLineHeight(QQuickItem *editor)
{
    auto *document = editor->property("textDocument").value<QQuickTextDocument *>()->textDocument();
    settle([&] {
        return document->begin().blockFormat().lineHeightType() == QTextBlockFormat::LineDistanceHeight
            && document->lastBlock().blockFormat().lineHeightType() == QTextBlockFormat::LineDistanceHeight;
    });
    const auto lines = editor->property("lineCount").toInt();
    CHECK(lines > 0);
    return editor->property("contentHeight").toReal() / lines;
}

inline qreal contentY(QQuickItem *viewport) { return viewport->property("contentY").toReal(); }

// Waits until the panes keep their place, so the window has finished its
// layout and the view has placed the panes for the selected file.
inline void settlePanes(Fixture &fixture)
{
    for (const auto *name : {"stage2Viewport", "resultViewport", "stage3Viewport"})
        test_support::waitForLayout(fixture.window, QString::fromLatin1(name), 60);
}

// Turns the mouse wheel down over the named viewport, a notch at a time, until
// it has scrolled to at least target, and waits for each movement to end. A
// Flickable does not scroll for a wheel event stamped with the previous time.
inline void wheelTo(Fixture &fixture, const char *name, qreal target)
{
    static quint64 timestamp = 0;
    auto *viewport = test_support::waitForLayout(fixture.window, QString::fromLatin1(name), 60);
    for (int notch = 0; notch < 400 && contentY(viewport) < target; ++notch) {
        const auto point = viewport->mapToScene(QPointF(viewport->width() / 2, viewport->height() / 2));
        QWheelEvent wheel(point, fixture.window->mapToGlobal(point), QPoint(), QPoint(0, -120), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
        timestamp += 250;
        wheel.setTimestamp(timestamp);
        QCoreApplication::sendEvent(fixture.window, &wheel);
        settle([&] { return !viewport->property("moving").toBool(); });
    }
    CHECK(contentY(viewport) >= target);
}

// For the scroll tests: the right source and the result hold 30 lines after
// line 50 that the left source lacks.
inline FileSession insertionFile()
{
    const auto left = numberedLines("shared", 1, 400);
    const auto right =
        numberedLines("shared", 1, 50) + numberedLines("right only", 1, 30) + numberedLines("shared", 51, 350);
    return test_support::choiceFile(left, right, right);
}

// Opens insertionFile with the result scrolled to top and its cursor at the
// start of the line holding marker, with keyboard focus in the result.
inline void placeResultCursor(Fixture &fixture, const QString &marker, qreal top)
{
    auto *editor = fixture.item("resultText");
    auto *result = fixture.item("resultViewport");
    settle([&] { return editor->property("lineCount").toInt() > 400; });
    settlePanes(fixture);
    const auto offset = static_cast<int>(fixture.text().indexOf(marker));
    CHECK(offset >= 0);
    select(fixture, offset, offset);
    editor->forceActiveFocus();
    CHECK(result->setProperty("contentY", top));
    test_support::waitForFrames(fixture.window, "the result is scrolled to its start");
    CHECK(editor->property("cursorPosition").toInt() == offset && contentY(result) == top);
}

// Lets the replaced text lay out before reading the panes.
inline void settleReplacement(Fixture &fixture)
{
    test_support::waitForFrames(fixture.window, "the replaced text is laid out");
}

// A cursor rectangle within the result viewport.
inline bool cursorInView(Fixture &fixture)
{
    auto *editor = fixture.item("resultText");
    auto *result = fixture.item("resultViewport");
    const auto cursor = editor->property("cursorRectangle").toRectF();
    const auto top = editor->y() + cursor.top() - contentY(result);
    return top >= 0 && top + cursor.height() <= result->height();
}

} // namespace comparison_scene
