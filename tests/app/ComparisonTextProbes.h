#pragma once

#include "ComparisonEditor.h"
#include <QTextBlock>

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
