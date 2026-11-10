// Checks of the window's geometry.
#include "acceptance/WindowChecks.h"
#include "fixtures/FileToolbar.h"

#include "acceptance/JourneySteps.h"
#include "fixtures/QuickScene.h"
#include "ui/TextBlocks.h"

#include <QCoreApplication>
#include <QFont>
#include <QFontMetricsF>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlExpression>
#include <QSignalSpy>
#include <QTextLayout>
#include <QTextOption>
#include <QWheelEvent>

#include <algorithm>
#include <cstring>
#include <functional>
#include <stdexcept>

using namespace branch_truce;
using test_support::settleFor;

namespace journey {
namespace {

// How long scrolling a target into view may take just after a resize.
constexpr int kRevealDeadlineMs = 2000;

std::string size(int width, int height) { return std::to_string(width) + "x" + std::to_string(height); }

std::string pageExtent(QObject *page)
{
    return "content " + std::to_string(int(page->property("contentHeight").toReal())) + " px in a "
        + std::to_string(int(page->property("height").toReal())) + " px page";
}

// Lays a wrapped text out as Text does, with its font, wrap mode and width inside its padding, and
// returns the first line break between two letters or digits of a word that would fit on a line
// of its own. The layout must give the item's own line count, or the check is not valid.
std::optional<QString> midWordBreak(QQuickItem *label)
{
    const auto width =
        label->width() - label->property("leftPadding").toReal() - label->property("rightPadding").toReal();
    const auto font = label->property("font").value<QFont>();
    const QFontMetricsF metrics(font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapMode(label->property("wrapMode").toInt()));
    std::optional<QString> broken;
    int lines = 0;
    for (const auto &paragraph : label->property("text").toString().split(QLatin1Char('\n'))) {
        QTextLayout layout(paragraph, font);
        layout.setTextOption(option);
        layout.beginLayout();
        for (auto line = layout.createLine(); line.isValid(); line = layout.createLine()) line.setLineWidth(width);
        layout.endLayout();
        lines += std::max(1, layout.lineCount());
        for (int i = 1; i < layout.lineCount() && !broken; ++i) {
            auto start = layout.lineAt(i).textStart();
            auto end = start;
            if (start <= 0 || start >= paragraph.size() || !paragraph[start - 1].isLetterOrNumber()
                || !paragraph[start].isLetterOrNumber())
                continue;
            while (start > 0 && paragraph[start - 1].isLetterOrNumber()) --start;
            while (end < paragraph.size() && paragraph[end].isLetterOrNumber()) ++end;
            const auto word = paragraph.mid(start, end - start);
            if (metrics.horizontalAdvance(word) <= width) broken = "\"" + word + "\" in \"" + paragraph + "\"";
        }
    }
    if (lines != label->property("lineCount").toInt())
        throw std::runtime_error("layout of \"" + label->property("text").toString().toStdString() + "\" gives "
                                 + std::to_string(lines) + " lines, the item has "
                                 + label->property("lineCount").toString().toStdString());
    return broken;
}

// Opens the file toolbar's overflow and returns its entry named `name`.
QQuickItem *overflowEntry(Scene &scene, const char *name)
{
    auto *toolbar = scene.item("fileToolBar");
    auto *more = test_support::namedButton(toolbar, QStringLiteral("More Actions"));
    if (!more) return nullptr;
    test_support::clickItem(scene.window(), more);
    return stable(scene, name,
                  [&] { return test_support::namedButton(scene.window()->contentItem(), QString::fromLatin1(name)); });
}

// The first undecided region's line on one pane, and where that pane shows it.
struct Pane {
    const char *side;
    const char *viewport;
    const char *text;
    int line = 0;
    int position = -1;
    int sourceLine = 0;
    int resultLine = 0;
};
} // namespace

std::string geometry(QQuickItem *item)
{
    const auto origin = item->mapToScene(QPointF(0, 0));
    return item->objectName().toStdString() + "@" + std::to_string(int(origin.x())) + ","
        + std::to_string(int(origin.y())) + " " + std::to_string(int(item->width())) + "x"
        + std::to_string(int(item->height())) + (item->isVisible() ? "" : " hidden");
}

std::vector<const char *> everyControl()
{
    std::vector<const char *> names(std::begin(mainControls), std::end(mainControls));
    names.insert(names.end(), std::begin(textPanes), std::end(textPanes));
    return names;
}

qreal checkFits(Scene &scene, int width, int height, const std::vector<const char *> &names)
{
    auto *window = scene.window();
    if (window->minimumWidth() > width || window->minimumHeight() > height)
        throw std::runtime_error("minimum size " + size(window->minimumWidth(), window->minimumHeight())
                                 + " exceeds the screen " + size(width, height));
    window->resize(width, height);
    settleFor("window size", [&] { return window->width() == width && window->height() == height; });
    const QRectF screen(0, 0, width, height);
    const auto inside = [&](QQuickItem *item) {
        const auto rect = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        return item->isVisible() && item->width() > 0 && item->height() > 0
            && screen.adjusted(-0.5, -0.5, 0.5, 0.5).contains(rect);
    };
    for (const auto *name : names) {
        bool openedOverflow = test_support::fileOverflowControl(name);
        auto *item =
            openedOverflow ? test_support::fileToolbarControl(window, name, Scene::settleMs) : scene.item(name);
        if (!item->isVisible()) {
            if (auto *entry = overflowEntry(scene, name)) {
                item = entry;
                openedOverflow = true;
            }
        }
        // Scroll again while waiting: just after a resize the page can still lay out at the
        // previous size, so the first scroll position can be stale.
        bool fits = heldWithin(
            [&] {
                Scene::reveal(item);
                return inside(item);
            },
            kRevealDeadlineMs);
        // The toolbar can move an action into overflow during the resize after the first
        // lookup, so try the menu again once the layout has settled.
        if (!fits && !openedOverflow) {
            if (auto *entry = overflowEntry(scene, name)) {
                item = entry;
                openedOverflow = true;
                fits = heldWithin(
                    [&] {
                        Scene::reveal(item);
                        return inside(item);
                    },
                    kRevealDeadlineMs);
            }
        }
        if (!fits) throw std::runtime_error("unreachable at " + size(width, height) + ": " + geometry(item));
        if (openedOverflow) {
            CHECK(!test_support::accessibleName(item).isEmpty());
            test_support::closeFileToolbarMenu(window);
        }
    }
    // Each pane header is one line high.
    for (const auto *name : {"stage2Label", "resultLabel", "stage3Label"}) {
        auto *label = scene.item(name);
        const auto line = QFontMetricsF(label->property("font").value<QFont>()).lineSpacing();
        if (label->height() > line * 1.5)
            throw std::runtime_error(std::string(name) + " is taller than one line at " + size(width, height) + ": "
                                     + geometry(label));
    }
    qreal smallest = height;
    for (const auto *name : textPanes) smallest = std::min(smallest, scene.item(name)->height());
    return smallest;
}

bool checkWheelScroll(Scene &scene, const char *over)
{
    auto *page = scene.object("pageScroll");
    page->setProperty("contentY", 0);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    const auto overflowing = page->property("overflowing").toBool();
    auto *label = scene.item(over);
    const auto point = label->mapToScene(QPointF(label->width() / 2, label->height() / 2));
    QSignalSpy moved(page, SIGNAL(contentYChanged()));
    QWheelEvent wheel(point, scene.window()->mapToGlobal(point), QPoint(), QPoint(0, -120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(scene.window(), &wheel);
    if (overflowing) {
        settleFor("page scrolled by the wheel", [&] { return page->property("contentY").toReal() > 0; });
    } else {
        // A scroll the wheel started, smooth or not, has moved the page by the second frame drawn
        // after it.
        test_support::waitForFrames(scene.window(), "the journey scene");
        if (moved.count() > 0 || page->property("contentY").toReal() != 0)
            throw std::runtime_error("the wheel scrolled a page that fits the window: " + pageExtent(page));
    }
    return overflowing;
}

int expectWholeWords(QQuickItem *root, const std::string &where)
{
    int wrapped = 0;
    const std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
        if (!item->isVisible()) return;
        if (item->inherits("QQuickText") && item->property("wrapMode").toInt() != QTextOption::NoWrap
            && !item->property("text").toString().isEmpty()) {
            if (const auto word = midWordBreak(item))
                throw std::runtime_error(where + " breaks the word " + word->toStdString());
            if (item->property("lineCount").toInt() > item->property("text").toString().count(QLatin1Char('\n')) + 1)
                ++wrapped;
        }
        for (auto *child : item->childItems()) visit(child);
    };
    visit(root);
    return wrapped;
}

std::string firstUndecidedInView(Scene &scene, bool snapshot)
{
    auto &app = scene.app();
    Pane panes[] = {{"stage2", "stage2Viewport", "stage2Text"},
                    {"result", "resultViewport", "resultText"},
                    {"stage3", "stage3Viewport", "stage3Text"}};
    for (const auto &region : scene.regions()) {
        if (region.state != QStringLiteral("undecided")) continue;
        for (auto &pane : panes) {
            const bool result = std::strcmp(pane.side, "result") == 0;
            if (result && !region.mapped) continue;
            if (!result && region.side != QLatin1String(pane.side)) continue;
            const auto &range = result ? region.result : region.source;
            if (pane.position >= 0 && range.lineBegin >= pane.line) continue;
            pane.line = range.lineBegin;
            pane.position = range.utf16Begin;
            pane.sourceLine = region.source.lineBegin;
            pane.resultLine = region.result.lineBegin;
        }
    }
    std::string failures;
    for (const auto &pane : panes) {
        auto *view = scene.item(pane.viewport);
        auto *editor = scene.item(pane.text);
        QRectF rect;
        if (pane.position >= 0)
            CHECK(QMetaObject::invokeMethod(editor, "positionToRectangle", Q_RETURN_ARG(QRectF, rect),
                                            Q_ARG(int, pane.position)));
        auto top = editor->y() + rect.top() - view->property("contentY").toReal();
        auto height = rect.height();
        bool shown = pane.position >= 0;
        const auto *blocks =
            scene.item((std::string(pane.side) + "Gutter").c_str())->property("blocks").value<TextBlocks *>();
        CHECK(blocks);
        QVariantMap laidOut;
        for (const auto &value : blocks->between(0, 1e9))
            if (value.toMap().value("number").toInt() == pane.line) laidOut = value.toMap();
        // Keep the cursor-rectangle assertion as well as the complete block.
        const bool cursorShown = shown && top >= 0 && top + height <= view->height();
        qreal blockTop = top, blockHeight = height;
        if (!laidOut.isEmpty()) {
            blockTop = editor->y() + editor->property("topPadding").toReal() + laidOut.value("top").toReal()
                - view->property("contentY").toReal();
            blockHeight = laidOut.value("height").toReal();
        }
        int foldedFirst = 0, foldedEnd = 0;
        // A result line inside a folded marker block is represented by its visible conflict row,
        // not by the hidden QTextBlock's positionToRectangle.
        if (std::strcmp(pane.side, "result") == 0 && shown) {
            for (const auto &block : scene.markerBlocks()) {
                if (!block.folded || pane.line < block.resultLineBegin || pane.line >= block.resultLineEnd) continue;
                foldedFirst = block.resultLineBegin;
                foldedEnd = block.resultLineEnd;
                auto *row =
                    test_support::visualItem(scene.item("resultFoldedRows"),
                                             QStringLiteral("resultConflictRow-") + QString::number(foldedFirst));
                shown = row && row->isVisible();
                if (shown) {
                    top = sceneOrigin(row).y() - sceneOrigin(view).y();
                    height = row->height();
                }
                break;
            }
        }
        const bool folded = foldedFirst > 0;
        const bool fullBlockShown = !laidOut.isEmpty() && blockTop >= 0 && blockTop + blockHeight <= view->height();
        const bool inView =
            shown && top >= 0 && top + height <= view->height() && (folded || (cursorShown && fullBlockShown));
        if (inView && !snapshot) continue;
        const auto cursor = editor->property("cursorRectangle").toRectF();
        const auto origin = sceneOrigin(view);
        QJsonObject diagnostic{
            {"schema", 1},
            {"pane", pane.side},
            {"inView", inView},
            {"screen", QJsonObject{{"width", scene.window()->width()}, {"height", scene.window()->height()}}},
            {"selectedFileId", app.selection()->fileId()},
            {"editorPlacement", QJsonObject::fromVariantMap(app.editor()->placement())},
            {"navigation", QJsonObject::fromVariantMap(app.navigator()->target())},
            {"line", pane.line},
            {"utf16Position", pane.position},
            {"mappedSourceLine", pane.sourceLine},
            {"mappedResultLine", pane.resultLine},
            {"block", QJsonObject::fromVariantMap(laidOut)},
            {"fullBlockViewportTop", blockTop},
            {"fullBlockViewportBottom", blockTop + blockHeight},
            {"foldedFirst", foldedFirst},
            {"foldedEndExclusive", foldedEnd},
            {"visibleTop", top},
            {"visibleHeight", height},
            {"positionRectangle", QJsonObject{{"y", rect.y()}, {"height", rect.height()}}},
            {"cursorRectangle", QJsonObject{{"y", cursor.y()}, {"height", cursor.height()}}},
            {"viewport",
             QJsonObject{{"x", origin.x()}, {"y", origin.y()}, {"width", view->width()}, {"height", view->height()}}},
            {"editorY", editor->y()},
            {"editorWidth", editor->width()},
            {"editorHeight", editor->height()},
            {"activeFocus", editor->hasActiveFocus()},
            {"rowHeight", blocks->rowHeight()}};
        for (const auto *key : {"contentY", "contentHeight", "moving", "dragging", "flicking"})
            diagnostic[key] = QJsonValue::fromVariant(view->property(key));
        for (const auto *key :
             {"cursorPosition", "selectionStart", "selectionEnd", "topPadding", "bottomPadding", "wrapMode"})
            diagnostic[key] = QJsonValue::fromVariant(editor->property(key));
        QQmlExpression scrollbar(qmlContext(view), view, "ScrollBar.vertical ? ScrollBar.vertical.pressed : false");
        diagnostic["verticalScrollbarPressed"] = QJsonValue::fromVariant(scrollbar.evaluate());
        failures +=
            (failures.empty() ? "" : "\n") + QJsonDocument(diagnostic).toJson(QJsonDocument::Compact).toStdString();
    }
    return failures;
}

} // namespace journey
