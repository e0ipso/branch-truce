// Unit tests for TextBlocks: where the cursor goes around folded lines and where the whitespace markers lie.
#include "TestHarness.h"
#include "ui/TextBlocks.h"
#include "ui/PaneMarks.h"
#include "ui/ConnectorBands.h"
#include <QAbstractTextDocumentLayout>
#include <QGuiApplication>
#include <QFont>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QSignalSpy>
#include <QRectF>
#include <QTextDocument>
#include <QTextCursor>
#include <QTextLayout>
#include <cmath>
#include <memory>

using branch_truce::TextBlocks;

namespace {
// A TextEdit holding text, with the lines listed in folded hidden.
class Pane {
public:
    Pane(const QString &text, const QList<int> &folded)
    {
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nTextEdit { width: 400; tabStopDistance: 40 }",
            QUrl(QStringLiteral("qrc:/TextBlocksTests/Pane.qml")));
        editor.reset(qobject_cast<QQuickItem *>(component.create()));
        if (!editor) throw std::runtime_error(component.errorString().toStdString());
        CHECK(editor->setProperty("text", text));
        auto *document = editor->property("textDocument").value<QQuickTextDocument *>();
        CHECK(document != nullptr);
        blocks.setDocument(document);
        QVariantList hidden;
        const auto lines = text.split(QLatin1Char('\n'));
        for (const int line : folded)
            hidden.append(QVariantMap{{QStringLiteral("line"), line}, {QStringLiteral("text"), lines.at(line - 1)}});
        blocks.setHidden(hidden);
        // Lay out every block, as showing the text would.
        auto *laidOut = document->textDocument();
        (void)laidOut->documentLayout()->blockBoundingRect(laidOut->lastBlock());
    }

    // The editor's cursor rectangle at position, which the old marker scan used.
    QRectF cursorAt(int position) const
    {
        QRectF rect;
        CHECK(QMetaObject::invokeMethod(editor.get(), "positionToRectangle", Q_RETURN_ARG(QRectF, rect),
            Q_ARG(int, position)));
        return rect;
    }

    QQmlEngine engine;
    std::unique_ptr<QQuickItem> editor;
    TextBlocks blocks;
};

bool near(qreal value, qreal expected)
{
    return std::abs(value - expected) < 1e-3;
}

// The marker covers the character at position, seen from area's top left.
bool covers(const Pane &pane, const QVariant &marker, int position, bool tab, QPointF area = {})
{
    const auto map = marker.toMap();
    const auto rect = pane.cursorAt(position);
    const auto next = pane.cursorAt(position + 1);
    return map.value("tab").toBool() == tab && near(map.value("x").toReal(), rect.x() - area.x())
        && near(map.value("y").toReal(), rect.y() - area.y()) && near(map.value("w").toReal(), next.x() - rect.x())
        && near(map.value("h").toReal(), rect.height());
}

// Folded lines 2 and 3 between two equal lines: line 1 starts at 0, line 2 at 7,
// line 3 at 20 and line 4 at 28.
const QString between = QStringLiteral("abcdef\n<<<<<<< HEAD\n=======\nabcdef\n");
// Folded lines 1 and 4 around two shown lines: line 2 starts at 13, line 3 at 20
// and line 4 at 27; the text ends at 41.
const QString around = QStringLiteral("<<<<<<< HEAD\nabcdef\nabcdef\n>>>>>>> theirs");
}

int main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    test_support::Suite suite("text-blocks");

    suite.run("row spacing follows wrapped lines, edits, folds and document replacement", [] {
        const QString original = QStringLiteral("abcdefghijabcdefghijabcdefghijabcdefghij\nnext\u00a0é");
        Pane pane(original, {});
        CHECK(pane.editor->setProperty("width", 80));
        CHECK(pane.editor->setProperty("wrapMode", 3));
        auto *quick = pane.editor->property("textDocument").value<QQuickTextDocument *>();
        auto *doc = quick->textDocument();
        const auto raw = doc->toRawText();
        const qreal baseStep = pane.cursorAt(static_cast<int>(original.indexOf(QLatin1Char('\n'))) + 1).y()
            - pane.cursorAt(static_cast<int>(original.indexOf(QLatin1Char('\n')))).y();
        CHECK(baseStep > 0);
        CHECK(QMetaObject::invokeMethod(pane.editor.get(), "select", Q_ARG(int, 2), Q_ARG(int, 20)));
        const auto selectionStart = pane.editor->property("selectionStart").toInt();
        const auto selectionEnd = pane.editor->property("selectionEnd").toInt();
        QSignalSpy changes(doc, &QTextDocument::contentsChange);
        const int documentRevision = doc->revision();
        pane.blocks.setRowSpacing(6);
        CHECK(changes.count() > 0 && doc->revision() > documentRevision);
        CHECK(doc->toRawText() == raw && !doc->isModified());
        CHECK(pane.editor->property("selectionStart").toInt() == selectionStart
            && pane.editor->property("selectionEnd").toInt() == selectionEnd);
        CHECK(!doc->isUndoRedoEnabled() && doc->availableUndoSteps() == 0);
        auto block = doc->begin();
        (void)doc->documentLayout()->blockBoundingRect(block);
        CHECK(block.layout()->lineCount() > 1);
        CHECK(near(pane.blocks.rowHeight(), baseStep + 6));
        const auto laidOut = pane.blocks.between(0, pane.blocks.yAtLine(2));
        CHECK(!laidOut.isEmpty());
        CHECK(near(laidOut.first().toMap().value("rowHeight").toReal(), pane.blocks.rowHeight()));
        for (int row = 1; row < block.layout()->lineCount(); ++row) {
            const auto previous = block.layout()->lineAt(row - 1);
            const auto next = block.layout()->lineAt(row);
            CHECK(near(next.y() - previous.y(), baseStep + 6));
            CHECK(near(pane.cursorAt(block.position() + next.textStart()).y()
                - pane.cursorAt(block.position() + previous.textStart()).y(), baseStep + 6));
        }
        const auto second = doc->begin().next();
        CHECK(near(pane.cursorAt(second.position()).y() - pane.cursorAt(second.position() - 1).y(), baseStep + 6));
        CHECK(near(pane.blocks.yAtLine(2) - pane.blocks.yAtLine(1), baseStep * block.layout()->lineCount()
            + 6 * block.layout()->lineCount()));
        const auto middleOfGap = pane.cursorAt(second.position()).y() - 3;
        CHECK(pane.blocks.lineAtY(middleOfGap) < 2 && pane.blocks.lineAtY(middleOfGap) > 1);
        const auto gap = pane.blocks.between(middleOfGap, middleOfGap + 1);
        CHECK(gap.size() == 1 && gap.first().toMap().value("number").toInt() == 1);
        const auto afterFormat = changes.count();
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        CHECK(changes.count() == afterFormat);
        auto font = pane.editor->property("font").value<QFont>();
        font.setPointSizeF(font.pointSizeF() * 1.5);
        CHECK(pane.editor->setProperty("font", font));
        (void)doc->documentLayout()->blockBoundingRect(doc->begin());
        const auto firstRow = doc->begin().layout()->lineAt(0);
        const auto secondRow = doc->begin().layout()->lineAt(1);
        CHECK(near(secondRow.y() - firstRow.y(), firstRow.height() + 6));
        CHECK(near(pane.blocks.rowHeight(), secondRow.y() - firstRow.y()));
        QTextCursor edit(doc);
        const int insertAt = second.position();
        edit.setPosition(insertAt);
        edit.insertText(QStringLiteral("new\n"));
        QCoreApplication::processEvents();
        CHECK(doc->toRawText() == QString(raw).insert(insertAt, QStringLiteral("new\u2029")));
        CHECK(doc->isModified() && doc->availableUndoSteps() == 0);
        for (auto line = doc->begin(); line.isValid(); line = line.next())
            CHECK(line.blockFormat().lineHeightType() == QTextBlockFormat::LineDistanceHeight
                && near(line.blockFormat().lineHeight(), 6));
        doc->setPlainText(QStringLiteral("first\nsecond\nthird"));
        QCoreApplication::processEvents();
        for (auto line = doc->begin(); line.isValid(); line = line.next())
            CHECK(near(line.blockFormat().lineHeight(), 6));
        pane.blocks.setHidden({QVariantMap{{"line", 2}, {"text", "second"}}});
        CHECK(near(pane.blocks.yAtLine(2), pane.blocks.yAtLine(3)));
        pane.blocks.setHidden({});
        CHECK(pane.blocks.yAtLine(3) > pane.blocks.yAtLine(2));
        pane.blocks.setRowSpacing(0);
        CHECK(!doc->begin().blockFormat().hasProperty(QTextFormat::LineHeight));
        CHECK(near(pane.cursorAt(doc->begin().next().position()).y() - pane.cursorAt(0).y(),
            pane.cursorAt(0).height()));
        CHECK(doc->toRawText() == QStringLiteral("first\u2029second\u2029third"));
    });
    suite.run("row spacing moves between documents and clears only its format", [] {
        Pane first(QStringLiteral("one\ntwo"), {});
        Pane second(QStringLiteral("three\nfour"), {});
        auto *firstQuick = first.editor->property("textDocument").value<QQuickTextDocument *>();
        auto *secondQuick = second.editor->property("textDocument").value<QQuickTextDocument *>();
        auto *firstDoc = firstQuick->textDocument();
        auto *secondDoc = secondQuick->textDocument();
        first.blocks.setRowSpacing(5);
        CHECK(near(firstDoc->begin().blockFormat().lineHeight(), 5));
        first.blocks.setDocument(secondQuick);
        CHECK(!firstDoc->begin().blockFormat().hasProperty(QTextFormat::LineHeight));
        CHECK(near(secondDoc->begin().blockFormat().lineHeight(), 5));
        CHECK(firstDoc->toRawText() == QStringLiteral("one\u2029two"));
        CHECK(secondDoc->toRawText() == QStringLiteral("three\u2029four"));
        CHECK(!firstDoc->isModified() && !secondDoc->isModified());
        first.blocks.setRowSpacing(0);
        CHECK(!secondDoc->begin().blockFormat().hasProperty(QTextFormat::LineHeight));
    });
    suite.run("pane marks rank passages, regions and marker blocks", [] {
        branch_truce::PaneMarks marks;
        const QVariantList passages{QVariantMap{{"side", "stage2"}, {"kind", "changed"},
            {"resultLineBegin", 1}, {"resultLineEnd", 5}, {"sourceLineBegin", 1}, {"sourceLineEnd", 5}}};
        const QVariantList regions{QVariantMap{{"side", "stage2"}, {"state", "undecided"}, {"mapped", true}, {"markersRemain", true},
            {"resultLineBegin", 2}, {"resultLineEnd", 4}, {"sourceLineBegin", 2}, {"sourceLineEnd", 4}}};
        const QVariantList blocks{QVariantMap{{"resultLineBegin", 3}, {"resultLineEnd", 4}, {"folded", true},
            {"markerLines", QVariantList{QVariantMap{{"line", 3}, {"text", "<<<<<<<"}},
                                          QVariantMap{{"line", 4}, {"text", ">>>>>>>"}}}}}};
        const auto result = marks.marks("result", passages, regions, {}, blocks, {});
        const auto lines = result.value("lines").toMap();
        CHECK(lines.value("1").toString() == "changed");
        CHECK(lines.value("2").toString() == "conflict");
        CHECK(lines.value("3").toString() == "markers");
        CHECK(lines.value("4").toString() == "changed");
        const auto source = marks.marks("stage2", passages, regions, {}, blocks, {}).value("lines").toMap();
        CHECK(source.value("2").toString() == "conflict");
        CHECK(marks.folds(blocks, false).value("hidden").toList().size() == 1);
        CHECK(marks.folds(blocks, true).value("hidden").toList().isEmpty());
        auto ineligible = blocks;
        auto block = ineligible.first().toMap();
        block.insert("folded", false);
        ineligible[0] = block;
        CHECK(marks.folds(ineligible, false).value("hidden").toList().isEmpty());
    });
    suite.run("an undecided nonconflicting contribution retains its passage kind", [] {
        branch_truce::PaneMarks marks;
        const QVariantList passages{QVariantMap{{"side", "stage2"}, {"kind", "changed"},
            {"resultLineBegin", 2}, {"resultLineEnd", 3}, {"sourceLineBegin", 2}, {"sourceLineEnd", 3}}};
        const QVariantList regions{QVariantMap{{"side", "stage2"}, {"state", "undecided"}, {"mapped", true},
            {"resultLineBegin", 2}, {"resultLineEnd", 3}, {"sourceLineBegin", 2}, {"sourceLineEnd", 3}}};
        for (const auto &pane : {QStringLiteral("stage2"), QStringLiteral("result")})
            CHECK(marks.marks(pane, passages, regions, {}, {}, {}).value("lines").toMap().value("2").toString() == "changed");
        CHECK(regions.first().toMap().value("state").toString() == "undecided");
    });
    suite.run("connector bands clip offscreen and follow layout", [] {
        Pane source(QStringLiteral("one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten"), {});
        Pane result(QStringLiteral("one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten"), {});
        branch_truce::ConnectorBands bands;
        bands.setSide(QStringLiteral("stage2"));
        bands.setSourceBlocks(&source.blocks);
        bands.setResultBlocks(&result.blocks);
        bands.setViewportHeight(source.blocks.yAtLine(4));
        bands.setPassages({QVariantMap{{"side", "stage2"}, {"kind", "changed"},
            {"sourceLineBegin", 2}, {"sourceLineEnd", 3}, {"resultLineBegin", 2}, {"resultLineEnd", 3}},
            QVariantMap{{"side", "stage2"}, {"kind", "changed"},
            {"sourceLineBegin", 9}, {"sourceLineEnd", 10}, {"resultLineBegin", 9}, {"resultLineEnd", 10}}});
        bands.setRegions({QVariantMap{{"side", "stage2"}, {"state", "accepted"},
            {"sourceLineBegin", 2}, {"sourceLineEnd", 3}}});
        CHECK(bands.bands().size() == 2);
        CHECK(bands.visibleBands().size() == 1);
        CHECK(bands.visibleBands().first().toMap().value("kind").toString() == "accepted");
        bands.setMarkerBlocks({QVariantMap{{"resultLineBegin", 2}, {"resultLineEnd", 4}}});
        bands.setFold(QVariantMap{{"rows", QVariantMap{{"2", QVariantMap{{"last", 3}}}}}});
        const auto folded = bands.bands().first().toMap();
        CHECK(folded.value("kind").toString() == "conflict");
        CHECK(folded.value("resultBegin").toInt() == 2 && folded.value("resultEnd").toInt() == 4);
        bands.setFold({});
        bands.setMarkerBlocks({});
        const qreal scroll = source.blocks.yAtLine(8);
        bands.setSourceScroll(scroll);
        bands.setResultScroll(scroll);
        CHECK(bands.visibleBands().size() == 1);
        CHECK(bands.visibleBands().first().toMap().value("sourceBegin").toInt() == 9);
        const qreal before = bands.visibleBands().first().toMap().value("sourceTop").toReal();
        auto font = source.editor->property("font").value<QFont>();
        font.setPointSizeF(font.pointSizeF() * 1.5);
        CHECK(source.editor->setProperty("font", font));
        QCoreApplication::processEvents();
        CHECK(!bands.visibleBands().isEmpty());
        CHECK(!near(before, bands.visibleBands().first().toMap().value("sourceTop").toReal()));
    });
    suite.run("curved ribbon hover follows full unequal attachment ends", [] {
        Pane source(QStringLiteral("one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten"), {});
        Pane result(QStringLiteral("one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten"), {});
        branch_truce::ConnectorBands bands;
        bands.setSide("stage2");
        bands.setSourceBlocks(&source.blocks);
        bands.setResultBlocks(&result.blocks);
        bands.setViewportHeight(result.blocks.yAtLine(10));
        bands.setPassages({QVariantMap{{"side", "stage2"}, {"kind", "changed"},
            {"sourceLineBegin", 2}, {"sourceLineEnd", 4}, {"resultLineBegin", 7}, {"resultLineEnd", 8}}});
        CHECK(bands.visibleBands().size() == 1);
        const auto band = bands.visibleBands().first().toMap();
        const auto st = band.value("sourceTop").toReal(), sb = band.value("sourceBottom").toReal();
        const auto rt = band.value("resultTop").toReal(), rb = band.value("resultBottom").toReal();
        CHECK(sb > st && rb > rt && rt > sb);
        // At t=1/4, midpoint-control cubic x is 0.296875w and y
        // interpolation is 0.15625. This point is above the straight wedge.
        const qreal width = 40, x = width * 0.296875;
        const qreal top = st + (rt - st) * 0.15625;
        const qreal bottom = sb + (rb - sb) * 0.15625;
        for (const bool left : {true, false}) {
            const auto at = [&](qreal px, qreal py) { return bands.bandAt(left ? px : width - px, py, width, left); };
            CHECK(!at(x, top + (bottom - top) * 0.1).isEmpty());
            CHECK(at(x, top - 1).isEmpty());
            CHECK(!at(0, st).isEmpty() && !at(0, sb).isEmpty());
            CHECK(!at(width, rt).isEmpty() && !at(width, rb).isEmpty());
            CHECK(at(-1, st).isEmpty() && at(width + 1, rt).isEmpty());
            CHECK(at(x, -1).isEmpty());
        }
    });
    suite.run("Down crosses folded lines and keeps the column", [] {
        Pane pane(between, {2, 3});
        CHECK(pane.blocks.hiddenAt(10) && pane.blocks.hiddenAt(23) && !pane.blocks.hiddenAt(31));
        CHECK(pane.blocks.settledPosition(3, 3) == 3);
        pane.blocks.keyPressed(Qt::Key_Down);
        CHECK(pane.blocks.settledPosition(10, 10) == 31);
        // The next check starts from there, with no key left over.
        CHECK(pane.blocks.settledPosition(31, 31) == 31);
    });
    suite.run("Up crosses folded lines and keeps the column", [] {
        Pane pane(between, {2, 3});
        CHECK(pane.blocks.settledPosition(32, 32) == 32);
        pane.blocks.keyPressed(Qt::Key_Up);
        CHECK(pane.blocks.settledPosition(24, 24) == 4);
        CHECK(pane.blocks.settledPosition(30, 30) == 30);
        pane.blocks.keyPressed(Qt::Key_PageUp);
        CHECK(pane.blocks.settledPosition(22, 22) == 2);
    });
    suite.run("Home at a folded first line goes to the first shown line", [] {
        Pane pane(around, {1, 4});
        CHECK(pane.blocks.settledPosition(20, 20) == 20);
        pane.blocks.keyPressed(Qt::Key_Home);
        // Nothing is shown above, so the cursor goes on to the start of line 2.
        CHECK(pane.blocks.settledPosition(0, 0) == 13);
    });
    suite.run("End at a folded last line goes to the last shown line", [] {
        Pane pane(around, {1, 4});
        CHECK(pane.blocks.settledPosition(13, 13) == 13);
        pane.blocks.keyPressed(Qt::Key_End);
        // Nothing is shown below, so the cursor goes back to the end of line 3.
        CHECK(pane.blocks.settledPosition(41, 41) == 26);
    });
    suite.run("another key goes on the way the cursor went", [] {
        Pane pane(between, {2, 3});
        CHECK(pane.blocks.settledPosition(28, 28) == 28);
        pane.blocks.keyPressed(Qt::Key_Left);
        CHECK(pane.blocks.settledPosition(27, 27) == 6);
        pane.blocks.keyPressed(Qt::Key_Right);
        CHECK(pane.blocks.settledPosition(7, 7) == 28);
    });
    suite.run("cursors and selections set by other code", [] {
        Pane pane(between, {2, 3});
        // A cursor goes to the next shown line.
        CHECK(pane.blocks.settledPosition(22, 22) == 28);
        // A selection ends on its anchor's side.
        CHECK(pane.blocks.settledPosition(22, 2) == 6);
        CHECK(pane.blocks.settledPosition(22, 33) == 28);
    });
    suite.run("cursor checks wait for the change to complete and come once", [] {
        Pane pane(between, {2, 3});
        QSignalSpy due(&pane.blocks, &TextBlocks::cursorCheckDue);
        pane.blocks.queueCursorCheck();
        pane.blocks.keyPressed(Qt::Key_Down);
        pane.blocks.queueCursorCheck();
        CHECK(due.count() == 0);
        QCoreApplication::processEvents();
        CHECK(due.count() == 1);
        pane.blocks.queueCursorCheck();
        QCoreApplication::processEvents();
        CHECK(due.count() == 2);
    });
    suite.run("markers lie on the spaces and tabs of the text", [] {
        // Line 2 starts at 6; its spaces are at 7 and 8.
        Pane pane(QStringLiteral("a b\tc\nd  e\n"), {});
        const auto markers = pane.blocks.whitespaceWithin(QRectF(0, 0, 1000, 1000), 2, 8);
        CHECK(markers.size() == 4);
        CHECK(covers(pane, markers.at(0), 1, false));
        CHECK(covers(pane, markers.at(1), 3, true));
        CHECK(covers(pane, markers.at(2), 7, false));
        CHECK(covers(pane, markers.at(3), 8, false));
        // The tab reaches on to its tab stop, past a space's width.
        CHECK(markers.at(1).toMap().value("w").toReal() > markers.at(0).toMap().value("w").toReal());
    });
    suite.run("markers are relative to the area and leave out folded and outside lines", [] {
        // Line 2 is folded and holds a space; line 3 starts at 17, its space is at 18.
        Pane pane(QStringLiteral("x x\n<<<<<<< HEAD\ny y\n"), {2});
        CHECK(pane.blocks.whitespaceWithin(QRectF(0, 0, 1000, 1000), 2, 8).size() == 2);
        const QPointF origin(1, pane.cursorAt(18).y() + 1);
        const auto below = pane.blocks.whitespaceWithin(QRectF(origin, QSizeF(1000, 1000)), 2, 8);
        CHECK(below.size() == 1 && covers(pane, below.at(0), 18, false, origin));
        CHECK(pane.blocks.whitespaceWithin(QRectF(0, 1000, 1000, 1000), 2, 8).isEmpty());
        CHECK(pane.blocks.whitespaceWithin(QRectF(0, 0, 0, 0), 2, 8).isEmpty());
    });
    return suite.report();
}
