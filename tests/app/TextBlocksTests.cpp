// Unit tests for TextBlocks: where the cursor goes around folded lines.
#include "TestHarness.h"
#include "ui/TextBlocks.h"
#include <QAbstractTextDocumentLayout>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QSignalSpy>
#include <QTextDocument>
#include <memory>

using branch_truce::TextBlocks;

namespace {
// A TextEdit holding text, with the lines listed in folded hidden.
class Pane {
public:
    Pane(const QString &text, const QList<int> &folded)
    {
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nTextEdit { width: 400 }", QUrl(QStringLiteral("qrc:/TextBlocksTests/Pane.qml")));
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

    QQmlEngine engine;
    std::unique_ptr<QQuickItem> editor;
    TextBlocks blocks;
};

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
    return suite.report();
}
