#pragma once

#include "ComparisonMarks.h"
#include <cmath>

namespace comparison_scene {
// The result's document, whose blocks the view folds.
inline const QTextDocument &resultDocument(Fixture &fixture)
{
    return *fixture.item("resultText")->property("textDocument").value<QQuickTextDocument *>()->textDocument();
}

// The 1-based numbers of the result lines the view hides.
inline QList<int> foldedLines(Fixture &fixture)
{
    QList<int> hidden;
    const auto &document = resultDocument(fixture);
    for (auto block = document.begin(); block.isValid(); block = block.next())
        if (!block.isVisible()) hidden.push_back(block.blockNumber() + 1);
    return hidden;
}

// The 1-based lines of the result's text that start with one of the given prefixes.
inline QList<int> linesStarting(Fixture &fixture, const QStringList &prefixes)
{
    QList<int> found;
    const auto lines = fixture.text().split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i)
        if (std::any_of(prefixes.begin(), prefixes.end(),
                        [&](const QString &prefix) { return lines[i].startsWith(prefix); }))
            found.push_back(i + 1);
    return found;
}

// The visible items of one part of a pane layer, by line, such as the "row"s of
// the result's folded rows layer.
inline QMap<int, QQuickItem *> foldParts(QQuickItem *layer)
{
    QMap<int, QQuickItem *> found;
    auto *rows = layer->findChild<QQuickItem *>(layer->objectName() + QStringLiteral("Rows"));
    if (!rows) throw std::runtime_error("missing folded rows");
    for (auto *child : rows->childItems())
        if (child->isVisible() && child->property("line").isValid())
            found.insert(child->property("line").toInt(), child);
    return found;
}

inline QList<int> numbers(int first, int end, const QList<int> &except = {})
{
    QList<int> list;
    for (int line = first; line < end; ++line)
        if (!except.contains(line)) list.push_back(line);
    return list;
}

inline QString listed(const QList<int> &lines)
{
    QStringList shown;
    for (const auto line : lines) shown << QString::number(line);
    return shown.join(QLatin1Char(' '));
}

// How a result with conflict markers looks: the lines it hides, the lines
// numbered, the lines painted and marked as conflicts with markers remaining,
// the folded rows, and the y distance from line 1 to the line holding
// "bridge", in lines.
struct MarkerLook {
    QList<int> hidden;
    QList<int> numbered;
    QList<int> marked;
    QList<int> rows;
    qreal rowsToBridge = 0;
    bool operator==(const MarkerLook &) const = default;
};

inline MarkerLook markerLook(Fixture &fixture)
{
    MarkerLook look;
    look.hidden = foldedLines(fixture);
    look.numbered = gutterNumbers(fixture.item("resultGutter")).keys();
    const auto marks = paintedMarks(fixture, "result");
    for (auto it = marks.backgrounds.begin(); it != marks.backgrounds.end(); ++it)
        if (it.value() == "markers" && marks.glyphs.value(it.key()) == "markers") look.marked.push_back(it.key());
    look.rows = foldParts(fixture.item("resultFoldedRows")).keys();
    auto *editor = fixture.item("resultText");
    const auto first = sceneRectangle(editor, 0);
    const auto bridge = sceneRectangle(editor, static_cast<int>(fixture.text().indexOf("bridge")));
    look.rowsToBridge =
        std::round((bridge.top() - first.top()) / fixture.item("comparisonView")->property("rowHeight").toReal() * 100)
        / 100;
    return look;
}

inline std::string describeLook(const MarkerLook &look)
{
    return ("hidden [" + listed(look.hidden) + "] numbered [" + listed(look.numbered) + "] marked ["
            + listed(look.marked) + "] rows [" + listed(look.rows) + "] bridge at row "
            + QString::number(look.rowsToBridge))
        .toStdString();
}

// markedFile() with both blocks folded: lines 2-8 and 10-14 show as rows on lines 2 and 10.
inline MarkerLook markedFolded()
{
    const auto hidden = numbers(3, 9) + numbers(11, 15);
    return {hidden, numbers(1, 17, hidden), {2, 10}, {2, 10}, 2};
}

inline void expectLook(Fixture &fixture, const std::string &when, const MarkerLook &expected)
{
    test_support::waitUntil([&] { return markerLook(fixture) == expected; },
                            [&] {
                                return when + ", the result shows " + describeLook(markerLook(fixture)) + ", not "
                                    + describeLook(expected);
                            });
}

} // namespace comparison_scene
