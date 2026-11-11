#pragma once

#include "ComparisonGeometry.h"
#include <QColor>
#include <QSet>

namespace comparison_scene {
// What one pane paints, keyed by line: the kind of each line background, the
// kind of each gutter glyph, in the result the provenance letter, and
// "ignored" where the whole text takes the ignored side's tint. A line with
// no mark is absent.
struct PaneMarks {
    QMap<int, QString> backgrounds;
    QMap<int, QString> glyphs;
    QMap<int, QString> letters;
    QMap<int, QString> tinted;
    bool operator==(const PaneMarks &) const = default;
};

// The nonempty lines of an editor whose every character is drawn in its
// Kirigami.Theme.disabledTextColor, as "ignored".
inline QMap<int, QString> tintedLines(QQuickItem *editor)
{
    const auto disabled = qmlValue(editor, QStringLiteral("Kirigami.Theme.disabledTextColor")).value<QColor>();
    CHECK(disabled.isValid());
    const auto *document = editor->property("textDocument").value<QQuickTextDocument *>()->textDocument();
    QMap<int, QString> lines;
    for (auto block = document->begin(); block.isValid(); block = block.next()) {
        const int length = block.length() - 1;
        if (length <= 0) continue;
        QList<bool> covered(length, false);
        for (const auto &range : block.layout()->formats()) {
            const auto brush = range.format.foreground();
            if (brush.style() == Qt::NoBrush || brush.color() != disabled) continue;
            for (int at = std::max(range.start, 0); at < std::min(range.start + range.length, length); ++at)
                covered[at] = true;
        }
        if (!covered.contains(false)) lines.insert(block.blockNumber() + 1, QStringLiteral("ignored"));
    }
    return lines;
}

inline PaneMarks paintedMarks(Fixture &fixture, const std::string &pane)
{
    PaneMarks found;
    QSet<int> glyphLines;
    for (auto *fill : fixture.item((pane + "BackgroundsLines").c_str())->childItems()) {
        if (fill->isVisible() && fill->property("line").isValid())
            found.backgrounds.insert(fill->property("line").toInt(), fill->property("kind").toString());
    }
    for (auto *glyph : fixture.item((pane + "GutterMarkerSlot").c_str())->childItems()) {
        const auto kind = glyph->property("kind").toString();
        if (glyph->isVisible() && !kind.isEmpty()) {
            const auto line = glyph->property("line").toInt();
            CHECK(!glyphLines.contains(line));
            glyphLines.insert(line);
        }
        if (glyph->isVisible() && !kind.isEmpty() && kind != "removed")
            found.glyphs.insert(glyph->property("line").toInt(), kind);
    }
    // Provenance is inspectable on the real line-number accessibility node.
    // No permanent B/L/R/m text or second glyph column may reserve code width.
    for (auto *child : fixture.item((pane + "Gutter").c_str())->childItems()) {
        const auto text = child->property("text").toString();
        bool number = false;
        const auto line = text.toInt(&number);
        if (!child->isVisible() || text.isEmpty()) continue;
        CHECK(number);
        const auto description = qmlValue(child, QStringLiteral("Accessible.description")).toString();
        if (description.contains("Matches both captured sources"))
            found.letters.insert(line, "B");
        else if (description.contains("Matches the captured left source"))
            found.letters.insert(line, "L");
        else if (description.contains("Matches the captured right source"))
            found.letters.insert(line, "R");
        else if (description.contains("Edited by hand"))
            found.letters.insert(line, "m");
    }
    CHECK(
        !test_support::visualItem(fixture.window->contentItem(), QString::fromStdString(pane + "GutterDecisionSlot")));
    found.tinted = tintedLines(fixture.item((pane + "Text").c_str()));
    return found;
}

inline std::string describeMarks(const PaneMarks &marks)
{
    const auto list = [](const QMap<int, QString> &lines) {
        std::string shown;
        for (auto it = lines.begin(); it != lines.end(); ++it)
            shown += " " + std::to_string(it.key()) + "=" + it.value().toStdString();
        return shown;
    };
    return "backgrounds" + list(marks.backgrounds) + "; glyphs" + list(marks.glyphs) + "; letters" + list(marks.letters)
        + "; tinted" + list(marks.tinted);
}

inline void expectMarks(Fixture &fixture, const std::string &when, const std::string &pane, const PaneMarks &expected)
{
    test_support::waitUntil([&] { return paintedMarks(fixture, pane) == expected; },
                            [&] {
                                return when + ", " + pane + " paints " + describeMarks(paintedMarks(fixture, pane))
                                    + ", not " + describeMarks(expected);
                            });
    if (pane != "stage2" && pane != "stage3") return;
    // The remaining number/state gutter shares the full viewport's row fills,
    // including accepted rows and after history changes.
    QMap<int, QQuickItem *> strips;
    for (auto *fill : fixture.item((pane + "GutterBackgroundsLines").c_str())->childItems())
        if (fill->isVisible() && fill->property("line").isValid()) strips.insert(fill->property("line").toInt(), fill);
    CHECK(strips.keys() == expected.backgrounds.keys());
    for (auto *fill : fixture.item((pane + "BackgroundsLines").c_str())->childItems()) {
        const auto line = fill->property("line").toInt();
        if (!fill->isVisible() || !strips.contains(line)) continue;
        auto *strip = strips.value(line);
        const auto codeRect = fill->mapRectToScene(fill->boundingRect());
        const auto stripRect = strip->mapRectToScene(strip->boundingRect());
        CHECK(std::abs(codeRect.top() - stripRect.top()) < 0.5);
        CHECK(std::abs(codeRect.bottom() - stripRect.bottom()) < 0.5);
        CHECK(strip->property("color") == fill->property("color"));
        CHECK(strip->property("kind") == fill->property("kind"));
    }
}

// The marks of a pane whose regions on lines 2, 4 and 6 have the given kinds;
// each gets a glyph of its kind, and a background of its kind unless it is
// ignored, whose text is tinted instead. The other lines carry no mark.
inline PaneMarks regionMarks(const QString &second, const QString &fourth, const QString &sixth,
                             const QMap<int, QString> &letters = {})
{
    PaneMarks marks;
    marks.letters = letters;
    for (const auto &[line, kind] : {std::pair{2, second}, std::pair{4, fourth}, std::pair{6, sixth}}) {
        if (kind.isEmpty()) continue;
        marks.glyphs.insert(line, kind);
        (kind == "ignored" ? marks.tinted : marks.backgrounds).insert(line, kind);
    }
    return marks;
}

// A connector band as its handle draws it: its kind, the source and result
// lines it joins as [begin, end), and the scene y of the top and bottom of
// each end.
struct Band {
    QString kind;
    int sourceBegin = 0;
    int sourceEnd = 0;
    int resultBegin = 0;
    int resultEnd = 0;
    qreal sourceTop = 0;
    qreal sourceBottom = 0;
    qreal resultTop = 0;
    qreal resultBottom = 0;
};

inline std::vector<Band> paintedBands(Fixture &fixture, const std::string &side)
{
    std::vector<Band> bands;
    for (auto *band : fixture.item((side + "Bands").c_str())->childItems()) {
        if (!band->property("kind").isValid()) continue;
        const auto line = [&](const char *name) {
            return qmlValue(band, QStringLiteral("modelData.") + QLatin1String(name)).toInt();
        };
        const auto y = [&](const char *name) {
            return band->mapToScene(QPointF(0, band->property(name).toReal())).y();
        };
        bands.push_back({band->property("kind").toString(), line("sourceBegin"), line("sourceEnd"), line("resultBegin"),
                         line("resultEnd"), y("sourceTop"), y("sourceBottom"), y("resultTop"), y("resultBottom")});
    }
    std::sort(bands.begin(), bands.end(), [](const Band &a, const Band &b) { return a.sourceBegin < b.sourceBegin; });
    return bands;
}

// Each band as "kind source begin-end > result begin-end", such as "conflict 2-3>2-3".
inline std::vector<std::string> bandLines(Fixture &fixture, const std::string &side)
{
    std::vector<std::string> lines;
    for (const auto &band : paintedBands(fixture, side)) {
        lines.push_back(band.kind.toStdString() + " " + std::to_string(band.sourceBegin) + "-"
                        + std::to_string(band.sourceEnd) + ">" + std::to_string(band.resultBegin) + "-"
                        + std::to_string(band.resultEnd));
    }
    return lines;
}

// The scene top and bottom of the pane's laid-out lines [begin, end).
// Ignored lines have tinted text and no background fill, so their geometry
// must come from the gutter's rows rather than from a visible fill. Empty
// ranges instead use the gap rule actually painted at the boundary.
inline std::optional<std::pair<qreal, qreal>> paintedSpan(Fixture &fixture, const std::string &pane, int begin, int end)
{
    if (begin == end) {
        for (auto *fill : fixture.item((pane + "BackgroundsGaps").c_str())->childItems()) {
            if (fill->isVisible() && fill->property("line").toInt() == begin) {
                const auto top = fill->mapToScene(QPointF()).y();
                return std::pair{top, top + fill->height()};
            }
        }
        return std::nullopt;
    }
    auto *gutter = fixture.item((pane + "Gutter").c_str());
    const auto rowIndex = [&](int line) {
        return qmlValue(gutter, QStringLiteral("rows.findIndex(row => row.number === %1)").arg(line)).toInt();
    };
    const int first = rowIndex(begin), last = rowIndex(end - 1);
    if (first < 0 || last < 0) return std::nullopt;
    const auto rowValue = [&](int index, const char *field) {
        return qmlValue(gutter, QStringLiteral("rows[%1].%2").arg(index).arg(QLatin1String(field))).toReal();
    };
    const auto top = gutter->mapToScene(QPointF(0, rowValue(first, "top"))).y();
    const auto bottom = gutter->mapToScene(QPointF(0, rowValue(last, "top") + rowValue(last, "extent"))).y();
    return std::pair{top, bottom};
}

// Waits until one side's handle draws the expected bands, each with its ends
// on the lines that the source pane and the result paint for its passage.
inline void expectBands(Fixture &fixture, const std::string &when, const std::string &side,
                        const std::vector<std::string> &expected)
{
    const auto misplaced = [&]() -> std::string {
        if (bandLines(fixture, side) != expected) return "the wrong bands";
        for (const auto &band : paintedBands(fixture, side)) {
            const auto source = paintedSpan(fixture, side, band.sourceBegin, band.sourceEnd);
            const auto result = paintedSpan(fixture, "result", band.resultBegin, band.resultEnd);
            if (!source || !result) return "line geometry not ready";
            const auto off = [](qreal a, qreal b) { return std::abs(a - b) > 0.5; };
            if (off(band.sourceTop, source->first) || off(band.sourceBottom, source->second)
                || off(band.resultTop, result->first) || off(band.resultBottom, result->second))
                return "a band at " + std::to_string(band.sourceTop) + "-" + std::to_string(band.sourceBottom) + ">"
                    + std::to_string(band.resultTop) + "-" + std::to_string(band.resultBottom) + " over lines at "
                    + std::to_string(source->first) + "-" + std::to_string(source->second) + ">"
                    + std::to_string(result->first) + "-" + std::to_string(result->second);
        }
        return {};
    };
    test_support::waitUntil([&] { return misplaced().empty(); },
                            [&] {
                                std::string drawn;
                                for (const auto &line : bandLines(fixture, side)) drawn += " " + line;
                                return when + ", the " + side + " handle draws " + misplaced() + ":" + drawn;
                            });
}

} // namespace comparison_scene
