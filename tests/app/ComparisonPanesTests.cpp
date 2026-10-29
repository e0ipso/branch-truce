// Unit tests for ComparisonPanes: its line mapping through the passages and the scrolling of panes by role.
#include "TestHarness.h"
#include "ui/ComparisonPanes.h"
#include "ui/TextBlocks.h"
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
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
    Scene()
    {
        QQmlComponent component(&engine);
        component.setData(reorderedScene, QUrl(QStringLiteral("qrc:/ComparisonPanesTests/ReorderedPanes.qml")));
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
}

int main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    test_support::Suite suite("comparison-panes");
    using Spans = QList<ComparisonPanes::Span>;

    suite.run("TRUCE-0219 an equal passage maps line by line and an unequal one proportionally", [] {
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
    suite.run("TRUCE-0219 an empty passage does not divide by its length", [] {
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
    suite.run("TRUCE-0219 a side without passages maps over the whole text", [] {
        CHECK(near(ComparisonPanes::mapLine({}, 5, true, 10, 20), 9));
        CHECK(near(ComparisonPanes::mapLine({}, 9, false, 20, 10), 5));
        CHECK(near(ComparisonPanes::mapLine({}, 5, true, 0, 20), 1));
    });
    suite.run("TRUCE-0219 passages of unequal length map between the sources and the earlier pane", [] {
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
    suite.run("TRUCE-0219 the registry finds editors by role", [] {
        Scene scene;
        for (const char *role : {"left", "result", "right", "earlier"})
            CHECK(scene.pane(role) != nullptr);
        CHECK(scene.panes->editor("stage2") == scene.pane("left")->property("editor").value<QQuickItem *>());
        CHECK(scene.panes->editor("stage3") == scene.pane("right")->property("editor").value<QQuickItem *>());
        CHECK(scene.panes->editor("earlier") == scene.pane("earlier")->property("editor").value<QQuickItem *>());
        CHECK(scene.panes->editor("result") == scene.pane("result")->property("editor").value<QQuickItem *>());
        CHECK(scene.panes->editor("unknown") == scene.panes->editor("result"));
    });
    suite.run("TRUCE-0219 panes declared in reverse order still scroll together", [] {
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
    suite.run("TRUCE-0219 placing a pane moves the source cursors to their rows", [] {
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
    return suite.report();
}
