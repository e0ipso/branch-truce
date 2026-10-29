// Unit tests for TextBlocks: where the cursor goes around folded lines and where the whitespace markers lie.
#include "TestHarness.h"
#include "ui/TextBlocks.h"
#include "ui/PaneMarks.h"
#include <QAbstractTextDocumentLayout>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QSignalSpy>
#include <QRectF>
#include <QTextDocument>
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

    suite.run("pane marks rank passages, regions and marker blocks", [] {
        branch_truce::PaneMarks marks;
        const QVariantList passages{QVariantMap{{"side", "stage2"}, {"kind", "changed"},
            {"resultLineBegin", 1}, {"resultLineEnd", 5}, {"sourceLineBegin", 1}, {"sourceLineEnd", 5}}};
        const QVariantList regions{QVariantMap{{"side", "stage2"}, {"state", "undecided"}, {"mapped", true},
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
    suite.run("TRUCE-0221 Down crosses folded lines and keeps the column", [] {
        Pane pane(between, {2, 3});
        CHECK(pane.blocks.hiddenAt(10) && pane.blocks.hiddenAt(23) && !pane.blocks.hiddenAt(31));
        CHECK(pane.blocks.settledPosition(3, 3) == 3);
        pane.blocks.keyPressed(Qt::Key_Down);
        CHECK(pane.blocks.settledPosition(10, 10) == 31);
        // The next check starts from there, with no key left over.
        CHECK(pane.blocks.settledPosition(31, 31) == 31);
    });
    suite.run("TRUCE-0221 Up crosses folded lines and keeps the column", [] {
        Pane pane(between, {2, 3});
        CHECK(pane.blocks.settledPosition(32, 32) == 32);
        pane.blocks.keyPressed(Qt::Key_Up);
        CHECK(pane.blocks.settledPosition(24, 24) == 4);
        CHECK(pane.blocks.settledPosition(30, 30) == 30);
        pane.blocks.keyPressed(Qt::Key_PageUp);
        CHECK(pane.blocks.settledPosition(22, 22) == 2);
    });
    suite.run("TRUCE-0221 Home at a folded first line goes to the first shown line", [] {
        Pane pane(around, {1, 4});
        CHECK(pane.blocks.settledPosition(20, 20) == 20);
        pane.blocks.keyPressed(Qt::Key_Home);
        // Nothing is shown above, so the cursor goes on to the start of line 2.
        CHECK(pane.blocks.settledPosition(0, 0) == 13);
    });
    suite.run("TRUCE-0221 End at a folded last line goes to the last shown line", [] {
        Pane pane(around, {1, 4});
        CHECK(pane.blocks.settledPosition(13, 13) == 13);
        pane.blocks.keyPressed(Qt::Key_End);
        // Nothing is shown below, so the cursor goes back to the end of line 3.
        CHECK(pane.blocks.settledPosition(41, 41) == 26);
    });
    suite.run("TRUCE-0221 another key goes on the way the cursor went", [] {
        Pane pane(between, {2, 3});
        CHECK(pane.blocks.settledPosition(28, 28) == 28);
        pane.blocks.keyPressed(Qt::Key_Left);
        CHECK(pane.blocks.settledPosition(27, 27) == 6);
        pane.blocks.keyPressed(Qt::Key_Right);
        CHECK(pane.blocks.settledPosition(7, 7) == 28);
    });
    suite.run("TRUCE-0221 cursors and selections set by other code", [] {
        Pane pane(between, {2, 3});
        // A cursor goes to the next shown line.
        CHECK(pane.blocks.settledPosition(22, 22) == 28);
        // A selection ends on its anchor's side.
        CHECK(pane.blocks.settledPosition(22, 2) == 6);
        CHECK(pane.blocks.settledPosition(22, 33) == 28);
    });
    suite.run("TRUCE-0221 cursor checks wait for the change to complete and come once", [] {
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
    suite.run("TRUCE-0222 markers lie on the spaces and tabs of the text", [] {
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
    suite.run("TRUCE-0222 markers are relative to the area and leave out folded and outside lines", [] {
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
