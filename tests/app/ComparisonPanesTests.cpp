// Unit tests for ComparisonPanes: its line mapping through the passages and the scrolling of panes by role.
#include "TestHarness.h"
#include "ui/ComparisonPanes.h"
#include "ui/TextBlocks.h"
#include <QEventLoop>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <utility>

using branch_truce::ComparisonPanes;
using branch_truce::TextBlocks;

namespace {
bool near(qreal value, qreal expected)
{
    return std::abs(value - expected) < 1e-3;
}

QVariantMap passage(const QString &side, int sourceBegin, int sourceEnd, int resultBegin, int resultEnd)
{
    return {{QStringLiteral("side"), side}, {QStringLiteral("sourceLineBegin"), sourceBegin},
        {QStringLiteral("sourceLineEnd"), sourceEnd}, {QStringLiteral("resultLineBegin"), resultBegin},
        {QStringLiteral("resultLineEnd"), resultEnd}};
}

QString numberedLines(int count)
{
    QString text;
    for (int line = 1; line <= count; ++line) text += QStringLiteral("line %1\n").arg(line);
    return text;
}

// The left source has 40 lines. Its first 10 equal the result's, its next 10 became the
// result's 30 lines 11 to 40, and its last 20 equal the result's lines 41 to 60. The
// right source has no passages. The earlier version equals the result.
QVariantList unequalPassages()
{
    return {passage(QStringLiteral("stage2"), 1, 11, 1, 11), passage(QStringLiteral("stage2"), 11, 21, 11, 41),
        passage(QStringLiteral("stage2"), 21, 41, 41, 61), passage(QStringLiteral("earlier"), 1, 61, 1, 61)};
}

// The panes are declared in the reverse of their order on screen, so only their roles tie
// them to the sides of the passages.
const char *const reorderedScene = R"(
import QtQuick
import BranchTruce

Item {
    width: 800
    height: 400
    property alias registry: panes
    component Pane: Item {
        property alias editor: text
        property alias viewport: view
        property alias blocks: lines
        width: 200
        height: 100
        Flickable {
            id: view
            anchors.fill: parent
            contentWidth: text.width
            contentHeight: text.height
            TextEdit {
                id: text
                width: view.width
            }
        }
        TextBlocks {
            id: lines
            document: text.textDocument
        }
    }
    Pane { id: earlierPane }
    Pane { id: rightPane }
    Pane { id: resultPane }
    Pane { id: leftPane }
    ComparisonPanes {
        id: panes
        earlier: earlierPane
        right: rightPane
        result: resultPane
        left: leftPane
        lineSpacing: 10
    }
}
)";

class Scene {
public:
    explicit Scene(const char *source = reorderedScene)
    {
        QQmlComponent component(&engine);
        component.setData(source, QUrl(QStringLiteral("qrc:/ComparisonPanesTests/Panes.qml")));
        root.reset(qobject_cast<QQuickItem *>(component.create()));
        if (!root) throw std::runtime_error(component.errorString().toStdString());
        panes = root->property("registry").value<ComparisonPanes *>();
        CHECK(panes != nullptr);
    }
    QQuickItem *pane(const char *role) const { return panes->property(role).value<QQuickItem *>(); }
    static QQuickItem *viewport(QQuickItem *pane) { return pane->property("viewport").value<QQuickItem *>(); }
    static TextBlocks *blocks(QQuickItem *pane) { return pane->property("blocks").value<TextBlocks *>(); }
    // The line on the reference row: a quarter of the 100-pixel panes, under three 10-pixel lines.
    static qreal lineOnRow(QQuickItem *pane)
    {
        return blocks(pane)->lineAtY(viewport(pane)->property("contentY").toReal() + row);
    }
    static void putOnRow(QQuickItem *pane, qreal line)
    {
        viewport(pane)->setProperty("contentY", blocks(pane)->yAtLine(line) - row);
    }
    // Gives the left source 40 lines, the result and the earlier version 60 and the right source 30.
    void fill() const
    {
        const std::initializer_list<std::pair<const char *, int>> texts{
            {"left", 40}, {"result", 60}, {"right", 30}, {"earlier", 60}};
        for (const auto &[role, lines] : texts)
            CHECK(pane(role)->property("editor").value<QObject *>()->setProperty("text", numberedLines(lines)));
        panes->setPassages(unequalPassages());
    }
    static constexpr qreal row = 25;

    QQmlEngine engine;
    std::unique_ptr<QQuickItem> root;
    ComparisonPanes *panes = nullptr;
};

// Exercise the actual source pane and native TextArea cursor scrolling rather
// than the bare TextEdit used by the correspondence-only fixtures above.
const char *const nativePlacementScene = R"(
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import BranchTruce

Item {
    id: root
    width: 960
    height: 540
    property alias registry: panes
    property var leftSource: ({})
    property var rightSource: ({})
    property alias resultText: resultEditor.text
    property bool wrapLines: false
    Row {
        component Source: SourcePane {
            width: root.width / 3
            height: root.height
            marks: ({lines: {}, gaps: {}, letters: {}})
            current: []
            tabStop: 40
            wrapLines: root.wrapLines
            showWhitespace: false
            minimumTextHeight: 0
            onScrolled: byUser => panes.paneScrolled(paneName, byUser)
        }
        Source {
            id: left
            paneName: "stage2"
            paneRole: "Left source"
            source: root.leftSource
        }
        Item {
            id: result
            width: root.width / 3
            height: root.height
            property alias editor: resultEditor
            property alias viewport: resultView
            property alias blocks: resultBlocks
            Flickable {
                id: resultView
                anchors.fill: parent
                onContentYChanged: panes.paneScrolled("result", moving || resultEditor.activeFocus)
                TextArea.flickable: TextArea {
                    id: resultEditor
                    textFormat: TextEdit.PlainText
                    font: left.editor.font
                    wrapMode: root.wrapLines ? TextEdit.Wrap : TextEdit.NoWrap
                    onTextChanged: resultBlocks.flushSpacing()
                }
            }
            CodeRows { id: resultBlocks; document: resultEditor.textDocument }
        }
        Source {
            id: right
            paneName: "stage3"
            paneRole: "Right source"
            source: root.rightSource
        }
    }
    ComparisonPanes {
        id: panes
        left: left
        result: result
        right: right
        lineSpacing: resultBlocks.rowHeight
    }
}
)";

void renderedFrame(QQuickWindow &window)
{
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    bool rendered = false;
    QObject::connect(&window, &QQuickWindow::frameSwapped, &loop, [&] {
        rendered = true;
        loop.quit();
    }, Qt::QueuedConnection);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    deadline.start(5000);
    window.update();
    loop.exec();
    CHECK(rendered);
}

void wholeLineInView(QQuickItem *pane, int line, const char *role)
{
    auto *view = Scene::viewport(pane);
    auto *editor = pane->property("editor").value<QQuickItem *>();
    auto *blocks = Scene::blocks(pane);
    const auto rows = blocks->between(blocks->yAtLine(line), blocks->yAtLine(line) + 1);
    CHECK(!rows.isEmpty());
    const auto block = rows.first().toMap();
    CHECK(block.value(QStringLiteral("number")).toInt() == line);
    const auto top = editor->y() + editor->property("topPadding").toReal()
        + block.value(QStringLiteral("top")).toReal() - view->property("contentY").toReal();
    const auto bottom = top + block.value(QStringLiteral("height")).toReal();
    if (top < -0.01 || bottom > view->height() + 0.01)
        throw std::runtime_error(QStringLiteral("%1 line %2: full block [%3, %4], viewport %5x%6, contentY %7, "
                                                "editorY %8, padding %9, rowHeight %10, cursor %11, wrapMode %12")
            .arg(QString::fromLatin1(role)).arg(line).arg(top).arg(bottom).arg(view->width()).arg(view->height())
            .arg(view->property("contentY").toReal()).arg(editor->y()).arg(editor->property("topPadding").toReal())
            .arg(blocks->rowHeight()).arg(editor->property("cursorPosition").toInt())
            .arg(editor->property("wrapMode").toInt()).toStdString());
}
}

int main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    test_support::Suite suite("comparison-panes");
    using Spans = QList<ComparisonPanes::Span>;

    suite.run("an equal passage maps line by line and an unequal one proportionally", [] {
        const Spans spans{{1, 3, 1, 3}, {3, 4, 3, 6}, {4, 10, 6, 12}};
        CHECK(near(ComparisonPanes::mapLine(spans, 2, true, 9, 11), 2));
        CHECK(near(ComparisonPanes::mapLine(spans, 3.5, true, 9, 11), 4.5));
        CHECK(near(ComparisonPanes::mapLine(spans, 5, true, 9, 11), 7));
        CHECK(near(ComparisonPanes::mapLine(spans, 4.5, false, 11, 9), 3.5));
        CHECK(near(ComparisonPanes::mapLine(spans, 7, false, 11, 9), 5));
        // A line on a boundary goes to the boundary on the other side.
        CHECK(near(ComparisonPanes::mapLine(spans, 3, true, 9, 11), 3));
        CHECK(near(ComparisonPanes::mapLine(spans, 6, false, 11, 9), 4));
        // Below the last passage the lines run on one by one.
        CHECK(near(ComparisonPanes::mapLine(spans, 12, true, 13, 15), 14));
    });
    suite.run("an empty passage does not divide by its length", [] {
        // Source A B C D E F G becomes A B C X Y Z D E F G.
        // The half-open equal passages put source D (line 4) at result line 7.
        const Spans spans{{1, 4, 1, 4}, {4, 4, 4, 7}, {4, 8, 7, 11}};
        CHECK(near(ComparisonPanes::mapLine(spans, 3.5, true, 7, 10), 3.5));
        CHECK(near(ComparisonPanes::mapLine(spans, 4, true, 7, 10), 7));
        CHECK(near(ComparisonPanes::mapLine(spans, 4.5, true, 7, 10), 7.5));
        CHECK(near(ComparisonPanes::mapLine(spans, 5, true, 7, 10), 8));
        CHECK(near(ComparisonPanes::mapLine(spans, 4, false, 10, 7), 4));
        CHECK(near(ComparisonPanes::mapLine(spans, 5, false, 10, 7), 4));
        CHECK(near(ComparisonPanes::mapLine(spans, 7, false, 10, 7), 4));
        CHECK(near(ComparisonPanes::mapLine(spans, 8, false, 10, 7), 5));
        // The same boundary rule applies when traversing a source-only passage backwards.
        const Spans sourceOnly{{1, 4, 1, 4}, {4, 7, 4, 4}, {7, 11, 4, 8}};
        CHECK(near(ComparisonPanes::mapLine(sourceOnly, 3.5, false, 7, 10), 3.5));
        CHECK(near(ComparisonPanes::mapLine(sourceOnly, 4, false, 7, 10), 7));
        CHECK(near(ComparisonPanes::mapLine(sourceOnly, 4.5, false, 7, 10), 7.5));
        CHECK(near(ComparisonPanes::mapLine(sourceOnly, 5, true, 10, 7), 4));
    });
    suite.run("a side without passages maps over the whole text", [] {
        CHECK(near(ComparisonPanes::mapLine({}, 5, true, 10, 20), 9));
        CHECK(near(ComparisonPanes::mapLine({}, 9, false, 20, 10), 5));
        CHECK(near(ComparisonPanes::mapLine({}, 5, true, 0, 20), 1));
    });
    suite.run("passages of unequal length map between the sources and the earlier pane", [] {
        ComparisonPanes panes;
        panes.setPassages(unequalPassages());
        // Left line 16 is halfway through the passage that became result lines 11 to 40.
        CHECK(near(panes.mapBetween("stage2", "result", 16, 40, 60, 60), 26));
        CHECK(near(panes.mapBetween("result", "stage2", 26, 60, 60, 40), 16));
        CHECK(near(panes.mapBetween("stage2", "earlier", 16, 40, 60, 60), 26));
        CHECK(near(panes.mapBetween("earlier", "stage2", 50, 60, 60, 40), 30));
        // The right source has no passages, so its 30 lines spread over the result's 60.
        CHECK(near(panes.mapBetween("stage2", "stage3", 16, 40, 60, 30), 13.5));
        CHECK(near(panes.mapBetween("earlier", "stage3", 26, 60, 60, 30), 13.5));
    });
    suite.run("the registry finds editors by role", [] {
        Scene scene;
        for (const char *role : {"left", "result", "right", "earlier"})
            CHECK(scene.pane(role) != nullptr);
        CHECK(scene.panes->editor("stage2") == scene.pane("left")->property("editor").value<QQuickItem *>());
        CHECK(scene.panes->editor("stage3") == scene.pane("right")->property("editor").value<QQuickItem *>());
        CHECK(scene.panes->editor("earlier") == scene.pane("earlier")->property("editor").value<QQuickItem *>());
        CHECK(scene.panes->editor("result") == scene.pane("result")->property("editor").value<QQuickItem *>());
        CHECK(scene.panes->editor("unknown") == scene.panes->editor("result"));
    });
    suite.run("panes declared in reverse order still scroll together", [] {
        Scene scene;
        auto *left = scene.pane("left");
        auto *result = scene.pane("result");
        auto *right = scene.pane("right");
        auto *earlier = scene.pane("earlier");
        scene.fill();
        CHECK(Scene::blocks(left)->blockCount() == 41 && Scene::blocks(result)->blockCount() == 61);

        // The result counts its trailing empty line, so the right source's 31 blocks spread over 61.
        Scene::putOnRow(left, 16);
        CHECK(near(Scene::lineOnRow(left), 16));
        scene.panes->follow("stage2");
        CHECK(near(Scene::lineOnRow(left), 16));
        CHECK(near(Scene::lineOnRow(result), 26));
        CHECK(near(Scene::lineOnRow(earlier), 26));
        CHECK(near(Scene::lineOnRow(right), 1 + 25.0 * 31 / 61));

        // A pane leads only while the user scrolls it.
        Scene::putOnRow(result, 50);
        scene.panes->paneScrolled("result", false);
        CHECK(near(Scene::lineOnRow(left), 16));
        scene.panes->paneScrolled("result", true);
        CHECK(near(Scene::lineOnRow(left), 30));
        CHECK(near(Scene::lineOnRow(earlier), 50));

        // A hidden earlier version stays where it is, and following only one pane leaves the rest.
        const auto earlierY = Scene::viewport(earlier)->property("contentY").toReal();
        CHECK(earlier->setProperty("visible", false));
        Scene::putOnRow(left, 8.5);
        scene.panes->follow("stage2", "result");
        CHECK(near(Scene::lineOnRow(result), 8.5));
        CHECK(!near(Scene::lineOnRow(right), 1 + 7.5 * 31 / 61));
        scene.panes->follow("stage2");
        CHECK(near(Scene::lineOnRow(right), 1 + 7.5 * 31 / 61));
        CHECK(Scene::viewport(earlier)->property("contentY").toReal() == earlierY);

        // Shown again, it comes to the result's line, with its cursor on the first whole line below the row.
        CHECK(earlier->setProperty("visible", true));
        scene.panes->paneShown("earlier");
        CHECK(near(Scene::lineOnRow(earlier), 8.5));
        auto *earlierText = scene.panes->editor("earlier");
        CHECK(earlierText->property("cursorPosition").toInt() == Scene::blocks(earlier)->positionOf(9));
    });
    suite.run("placing a pane moves the source cursors to their rows", [] {
        Scene scene;
        auto *left = scene.pane("left");
        auto *result = scene.pane("result");
        scene.fill();
        // Result line 27.5 is left line 16.5, whose cursor goes to the start of line 16.
        scene.panes->place("result", 27.5);
        CHECK(near(Scene::lineOnRow(result), 27.5));
        CHECK(near(Scene::lineOnRow(left), 16.5));
        CHECK(scene.panes->editor("stage2")->property("cursorPosition").toInt() == Scene::blocks(left)->positionOf(16));
    });
    suite.run("native placement keeps complete source and folded result rows visible after file replacement", [] {
        QQuickWindow window;
        Scene scene(nativePlacementScene);
        scene.root->setParentItem(window.contentItem());
        window.resize(960, 540);
        window.show();
        const auto before = numberedLines(120);
        const auto after = numberedLines(80);
        const auto leftText = before + QStringLiteral("left value\n") + after;
        const auto rightText = before + QStringLiteral("right value\n") + after;
        const auto resultText = before + QStringLiteral("<<<<<<< left\nleft value\n||||||| base\nbase value\n"
                                                        "=======\nright value\n>>>>>>> right\n") + after;
        auto *resultBlocks = Scene::blocks(scene.pane("result"));
        const QVariantList folds{QVariantMap{{QStringLiteral("line"), 121}, {QStringLiteral("text"), "<<<<<<< left"},
            {QStringLiteral("last"), 127}, {QStringLiteral("lastText"), ">>>>>>> right"}}};
        QVariantList passages;
        for (const auto &side : {QStringLiteral("stage2"), QStringLiteral("stage3")}) {
            passages.append(passage(side, 1, 121, 1, 121));
            passages.append(passage(side, 121, 122, 121, 128));
            passages.append(passage(side, 122, 203, 128, 209));
        }
        scene.panes->setPassages(passages);
        for (const auto height : {390, 386}) {
            scene.root->setHeight(height);
            // Selecting a short file resets each native editor, then the long
            // file restores its text and queues formatting/fold/cursor work.
            for (const auto &text : {QStringLiteral("short\n"), leftText}) {
                CHECK(scene.root->setProperty("leftSource", QVariantMap{{QStringLiteral("text"), text}}));
                CHECK(scene.root->setProperty("rightSource", QVariantMap{{QStringLiteral("text"),
                    text == leftText ? rightText : text}}));
                CHECK(scene.root->setProperty("resultText", text == leftText ? resultText : text));
                resultBlocks->setHidden(text == leftText ? folds : QVariantList{});
                if (text != leftText) renderedFrame(window);
            }
            scene.panes->editor("stage2")->forceActiveFocus();
            auto *focus = window.activeFocusItem();
            // The application defers placement from its editor notification.
            // Allow pending formatting and native cursor work to run with it.
            bool placed = false;
            QTimer::singleShot(0, scene.panes, [&] {
                scene.panes->place("result", 121);
                placed = true;
            });
            renderedFrame(window);
            renderedFrame(window);
            CHECK(placed);
            wholeLineInView(scene.pane("left"), 121, "stage2");
            wholeLineInView(scene.pane("result"), 121, "result folded marker");
            wholeLineInView(scene.pane("right"), 121, "stage3");
            CHECK(window.activeFocusItem() == focus);
            CHECK(scene.panes->editor("stage2")->property("text").toString() == leftText);
            CHECK(scene.panes->editor("stage3")->property("text").toString() == rightText);
            CHECK(scene.panes->editor("result")->property("text").toString() == resultText);
            CHECK(resultBlocks->hidden() == folds);
        }
    });
    suite.run("placement fits a wrapped block independently and yields to subsequent user scrolling", [] {
        QQuickWindow window;
        Scene scene(nativePlacementScene);
        scene.root->setParentItem(window.contentItem());
        window.resize(960, 540);
        window.show();
        CHECK(scene.root->setProperty("wrapLines", true));
        const auto before = numberedLines(120);
        const auto after = numberedLines(80);
        const auto leftText = before + QStringLiteral("wrapped source ").repeated(12) + '\n' + after;
        const auto shortText = before + QStringLiteral("short target\n") + after;
        CHECK(scene.root->setProperty("leftSource", QVariantMap{{QStringLiteral("text"), leftText}}));
        CHECK(scene.root->setProperty("rightSource", QVariantMap{{QStringLiteral("text"), shortText}}));
        CHECK(scene.root->setProperty("resultText", shortText));
        scene.panes->setPassages({passage("stage2", 1, 203, 1, 203), passage("stage3", 1, 203, 1, 203)});
        renderedFrame(window);
        auto *left = scene.pane("left");
        auto *blocks = Scene::blocks(left);
        const auto height = blocks->yAtLine(122) - blocks->yAtLine(121);
        CHECK(height > 2 * blocks->rowHeight());
        // Leave one row of spare height, less than the normal reference context.
        const auto header = scene.root->height() - Scene::viewport(left)->height();
        scene.root->setHeight(header + height + blocks->rowHeight());
        renderedFrame(window);
        scene.panes->place("result", 121);
        renderedFrame(window);
        renderedFrame(window);
        wholeLineInView(left, 121, "stage2 wrapped");
        wholeLineInView(scene.pane("result"), 121, "result");
        wholeLineInView(scene.pane("right"), 121, "stage3");
        CHECK(!near(Scene::viewport(left)->property("contentY").toReal(),
                    Scene::viewport(scene.pane("right"))->property("contentY").toReal()));
        const auto selected = blocks->positionOf(121) + 2;
        CHECK(QMetaObject::invokeMethod(scene.panes->editor("stage2"), "select", Q_ARG(int, selected), Q_ARG(int, selected + 3)));
        scene.panes->place("result", 121);
        CHECK(scene.panes->editor("stage2")->property("selectionStart").toInt() == selected);
        CHECK(scene.panes->editor("stage2")->property("selectionEnd").toInt() == selected + 3);
        auto *view = Scene::viewport(left);
        const auto scrolled = view->property("contentY").toReal() + blocks->rowHeight();
        CHECK(view->setProperty("contentY", scrolled));
        scene.panes->paneScrolled("stage2", true);
        renderedFrame(window);
        CHECK(near(view->property("contentY").toReal(), scrolled));
        CHECK(scene.panes->editor("stage2")->property("text").toString() == leftText);
        CHECK(scene.panes->editor("result")->property("text").toString() == shortText);
        CHECK(scene.panes->editor("stage3")->property("text").toString() == shortText);
    });
    return suite.report();
}
