#pragma once

// Offscreen Qt Quick helpers shared by the comparison view tests and the
// journey acceptance suite. Both load the real BranchTruce Main scene.
#include "TestHarness.h"
#include "fixtures/EventLoop.h"
#include <QAccessible>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QKeyEvent>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QStringList>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QtQml/qqml.h>
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

namespace test_support {
// Runs the event loop until ready() holds, as waitUntil does.
template <typename Predicate> void settleFor(const std::string &what, Predicate ready)
{
    waitUntil(std::move(ready), "timed out waiting for " + what);
}

template <typename Predicate> void settle(Predicate ready) { settleFor("a scene condition", std::move(ready)); }

// Runs the event loop until the window has rendered two more frames. The scene graph polishes
// every item before it renders, so the layout, bindings and queued calls that earlier events
// started have run by then. Wait for this before checking that something did not happen.
inline void waitForFrames(QQuickWindow *window, const std::string &what)
{
    QSignalSpy frames(window, &QQuickWindow::frameSwapped);
    window->update();
    waitUntil(
        [&] {
            if (frames.count() < 2) window->update();
            return frames.count() >= 2;
        },
        "no frame rendered before " + what);
}

// Repeater delegates in a popup are reachable only through the visual item tree.
inline QQuickItem *visualItem(QQuickItem *root, const QString &name, bool visibleOnly = false)
{
    if (!root || (visibleOnly && !root->isVisible())) return nullptr;
    if (root->objectName() == name) return root;
    for (auto *child : root->childItems())
        if (auto *found = visualItem(child, name, visibleOnly)) return found;
    return nullptr;
}

// The texts of the visible labels under root, in item order, joined with " · ": what a row
// of status chips reads as one line.
inline QString shownText(QQuickItem *root)
{
    QStringList texts;
    const std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
        if (!item->isVisible()) return;
        if (item->inherits("QQuickText") && !item->property("text").toString().isEmpty())
            texts.append(item->property("text").toString());
        for (auto *child : item->childItems()) visit(child);
    };
    visit(root);
    return texts.join(QStringLiteral(" · "));
}

// Strict evidence: never derive an accessibility name from painted text.
inline QString accessibleName(QQuickItem *item)
{
    auto *iface = QAccessible::queryAccessibleInterface(item);
    return iface ? iface->text(QAccessible::Name) : QString();
}

// Missing controls and painted text alone cannot satisfy accessibility evidence.
inline QAccessibleInterface *requireAccessible(QQuickItem *item, const std::string &name)
{
    if (!item) throw Failure("missing expected accessible control: " + name);
    auto *face = QAccessible::queryAccessibleInterface(item);
    if (!face || face->role() == QAccessible::NoRole || accessibleName(item).isEmpty())
        throw Failure("missing accessible role or name: " + name);
    return face;
}

// Text is useful for finding standard dialog buttons even without an attached accessibility client.
inline QString locatorName(QQuickItem *item)
{
    const auto name = accessibleName(item);
    return name.isEmpty() ? item->property("text").toString() : name;
}

inline QQuickItem *visualButton(QQuickItem *root, const std::function<bool(QQuickItem *)> &match)
{
    if (!root || !root->isVisible()) return nullptr;
    if (root->inherits("QQuickAbstractButton") && match(root)) return root;
    for (auto *child : root->childItems())
        if (auto *found = visualButton(child, match)) return found;
    return nullptr;
}

// Find a button by objectName. Only a button the scene cannot name, such as a dialog's
// standard button or a combo box entry, falls back to its locator name.
inline QQuickItem *namedButton(QQuickItem *root, const QString &name)
{
    if (auto *found = visualButton(root, [&](QQuickItem *button) { return button->objectName() == name; }))
        return found;
    return visualButton(root, [&](QQuickItem *button) { return locatorName(button) == name; });
}

inline bool reachable(QQuickWindow *window, QQuickItem *item)
{
    if (!item || !item->isVisible() || item->width() <= 0 || item->height() <= 0) return false;
    const auto rect = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    if (!QRectF(0, 0, window->width(), window->height()).contains(rect)) return false;
    for (auto *parent = item->parentItem(); parent; parent = parent->parentItem())
        if (parent->clip() && !parent->mapRectToScene(parent->boundingRect()).contains(rect)) return false;
    return true;
}

inline QQuickItem *reachableButton(QQuickWindow *window, QQuickItem *root, const QString &name)
{
    return visualButton(root, [&](QQuickItem *button) {
        return (button->objectName() == name || locatorName(button) == name) && reachable(window, button);
    });
}

// A popup's root item, under the window overlay, holds its header, content and footer.
inline QQuickItem *popupRoot(QObject *popup)
{
    auto *item = popup->property("contentItem").value<QQuickItem *>();
    if (!item) throw std::runtime_error("popup without content: " + popup->objectName().toStdString());
    while (item->parentItem() && !item->parentItem()->inherits("QQuickOverlay")) item = item->parentItem();
    return item;
}

inline void clickItem(QQuickWindow *window, QQuickItem *target)
{
    CHECK(target->isVisible() && target->isEnabled() && target->width() > 0 && target->height() > 0);
    const auto center = target->mapToScene(QPointF(target->width() / 2.0, target->height() / 2.0)).toPoint();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

// Dialog rows and list delegates get their final position only after a layout
// pass, so wait until the found item is visible and its scene position stops moving.
// stableMs requires the position to hold for that long, so a click cannot land
// between a state change and the layout pass that moves the target.
inline QQuickItem *waitForStable(QQuickWindow *window, const std::string &what,
                                 const std::function<QQuickItem *()> &find, int stableMs = 0)
{
    QQuickItem *found = nullptr;
    QPointF previous(-1, -1);
    QElapsedTimer held;
    held.start();
    QSignalSpy frames(window, &QQuickWindow::frameSwapped);
    std::string state = "not found";
    const auto laidOut = [&] {
        window->update();
        auto *target = find();
        found = target;
        if (!target) {
            state = "not found";
            return false;
        }
        const auto center = target->mapToScene(QPointF(target->width() / 2.0, target->height() / 2.0));
        state = "visible=" + std::to_string(target->isVisible()) + " size=" + std::to_string(target->width()) + "x"
            + std::to_string(target->height()) + " center=" + std::to_string(center.x()) + ","
            + std::to_string(center.y());
        if (!target->isVisible() || target->width() <= 0 || target->height() <= 0) return false;
        if (center != previous) {
            held.restart();
            frames.clear();
        }
        const bool stable = center == previous && frames.count() >= 2 && held.elapsed() >= stableMs
            && window->contentItem()->contains(center);
        state += " heldMs=" + std::to_string(held.elapsed())
            + " contentContains=" + std::to_string(window->contentItem()->contains(center))
            + " sameWindow=" + std::to_string(target->window() == window);
        previous = center;
        return stable;
    };
    waitUntil(laidOut, [&] { return "layout of " + what + ": " + state; }, 5000);
    return found;
}

inline QQuickItem *waitForLayout(QQuickWindow *window, const QString &name, int stableMs = 0)
{
    return waitForStable(
        window, name.toStdString(),
        [&] {
            if (auto *visible = visualItem(window->contentItem(), name, true)) return visible;
            return visualItem(window->contentItem(), name);
        },
        stableMs);
}

inline bool painted(QQuickItem *item)
{
    for (auto *ancestor = item; ancestor; ancestor = ancestor->parentItem())
        if (!ancestor->isVisible() || ancestor->opacity() <= 0) return false;
    return item != nullptr;
}

// Source choices reveal through a real pointer move, before any click is allowed.
inline QQuickItem *hoverChoiceControl(QQuickWindow *window, QQuickItem *root, const QString &name)
{
    auto *target = reachableButton(window, root, name);
    if (!target) return nullptr;
    QTest::mouseMove(window, target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint());
    return waitForStable(
        window, name.toStdString() + " after hover",
        [&] {
            auto *candidate = reachableButton(window, root, name);
            return painted(candidate) ? candidate : nullptr;
        },
        60);
}

// Open a range menu at the requested real source line, including decided ranges
// and later-line scopes that have no permanent gutter button.
inline void sourceLineMenu(QQuickWindow *window, QQuickItem *editor, QQuickItem *viewport, int line, int key = 0)
{
    auto *quick = editor->property("textDocument").value<QQuickTextDocument *>();
    if (!quick) throw std::runtime_error("source editor has no document");
    const auto block = quick->textDocument()->findBlockByNumber(line - 1);
    if (!block.isValid()) throw std::runtime_error("source line unavailable: " + std::to_string(line));
    editor->forceActiveFocus();
    editor->setProperty("cursorPosition", block.position());
    QRectF rect;
    CHECK(QMetaObject::invokeMethod(editor, "positionToRectangle", Q_RETURN_ARG(QRectF, rect),
                                    Q_ARG(int, block.position())));
    const auto top = editor->mapToItem(viewport->property("contentItem").value<QQuickItem *>(), rect.topLeft()).y();
    const auto offset = viewport->property("contentY").toReal();
    if (top < offset || top + rect.height() > offset + viewport->height())
        viewport->setProperty("contentY", std::max<qreal>(0, top - viewport->height() / 2));
    QPointF previousPoint(-1, -1);
    QElapsedTimer held;
    held.start();
    settleFor("source line " + std::to_string(line), [&] {
        QRectF current;
        QMetaObject::invokeMethod(editor, "positionToRectangle", Q_RETURN_ARG(QRectF, current),
                                  Q_ARG(int, block.position()));
        const auto point = editor->mapToScene(current.center());
        if (point != previousPoint) held.restart();
        previousPoint = point;
        return viewport->contains(viewport->mapFromScene(point)) && window->contentItem()->contains(point)
            && held.elapsed() >= 60;
    });
    if (key) {
        QTest::keyClick(window, static_cast<Qt::Key>(key), key == Qt::Key_F10 ? Qt::ShiftModifier : Qt::NoModifier);
    } else {
        CHECK(QMetaObject::invokeMethod(editor, "positionToRectangle", Q_RETURN_ARG(QRectF, rect),
                                        Q_ARG(int, block.position())));
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, editor->mapToScene(rect.center()).toPoint());
    }
}

}
