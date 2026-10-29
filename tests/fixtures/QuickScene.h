#pragma once

// Offscreen Qt Quick helpers shared by the comparison view tests and the
// journey acceptance suite. Both load the real BranchTruce Main scene.
#include "TestHarness.h"
#include <QAccessible>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QStringList>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QtQml/qqml.h>
#include <functional>
#include <stdexcept>
#include <string>

namespace test_support {
template<typename Predicate>
void settle(Predicate ready)
{
    QElapsedTimer deadline;
    deadline.start();
    while (!ready() && deadline.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    CHECK(ready());
}

template<typename Predicate>
void settleFor(const std::string &what, Predicate ready)
{
    QElapsedTimer deadline;
    deadline.start();
    while (!ready() && deadline.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    if (!ready()) throw std::runtime_error("timed out waiting for " + what);
}

// Repeater delegates in a popup are reachable only through the visual item tree.
inline QQuickItem *visualItem(QQuickItem *root, const QString &name)
{
    if (root->objectName() == name) return root;
    for (auto *child : root->childItems())
        if (auto *found = visualItem(child, name)) return found;
    return nullptr;
}

// The page header may retain an inactive title delegate after a resize. Prefer the
// live visual item while still allowing callers to inspect intentionally hidden items.
inline QQuickItem *visibleItem(QQuickItem *root, const QString &name)
{
    if (!root || !root->isVisible()) return nullptr;
    if (root->objectName() == name) return root;
    for (auto *child : root->childItems())
        if (auto *found = visibleItem(child, name)) return found;
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

// The name assistive technology reads: Accessible.name, or else the text of a button or
// label. Qt derives that second name only while an assistive technology is attached, which
// never happens offscreen, so derive it here the same way.
inline QString accessibleName(QQuickItem *item)
{
    auto *iface = QAccessible::queryAccessibleInterface(item);
    const auto name = iface ? iface->text(QAccessible::Name) : QString();
    if (!name.isEmpty() || !(item->inherits("QQuickAbstractButton") || item->inherits("QQuickText")))
        return name;
    return item->property("text").toString();
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
// standard button or a combo box entry, falls back to its accessible name. Never match text.
inline QQuickItem *namedButton(QQuickItem *root, const QString &name)
{
    if (auto *found = visualButton(root, [&](QQuickItem *button) { return button->objectName() == name; }))
        return found;
    return visualButton(root, [&](QQuickItem *button) { return accessibleName(button) == name; });
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
    std::string state = "not found";
    const auto laidOut = [&] {
        auto *target = find();
        found = target;
        if (!target) return false;
        const auto center = target->mapToScene(QPointF(target->width() / 2.0, target->height() / 2.0));
        state = "visible=" + std::to_string(target->isVisible()) + " size=" + std::to_string(target->width())
            + "x" + std::to_string(target->height()) + " center=" + std::to_string(center.x()) + ","
            + std::to_string(center.y());
        if (!target->isVisible() || target->width() <= 0 || target->height() <= 0) return false;
        if (center != previous) held.restart();
        const bool stable = center == previous && held.elapsed() >= stableMs && window->contentItem()->contains(center);
        previous = center;
        return stable;
    };
    QElapsedTimer deadline;
    deadline.start();
    while (!laidOut() && deadline.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    if (!laidOut()) throw std::runtime_error("layout of " + what + ": " + state);
    return found;
}

inline QQuickItem *waitForLayout(QQuickWindow *window, const QString &name, int stableMs = 0)
{
    return waitForStable(window, name.toStdString(),
                         [&] {
                             if (auto *visible = visibleItem(window->contentItem(), name)) return visible;
                             return visualItem(window->contentItem(), name);
                         }, stableMs);
}

inline bool fileOverflowControl(const char *name)
{
    return QStringList{QStringLiteral("discardUnsaved"), QStringLiteral("copyResult"),
        QStringLiteral("pasteResult"), QStringLiteral("sourceChoicesMenu"),
        QStringLiteral("showSourceChoices")}.contains(QString::fromLatin1(name));
}

// These commands deliberately live in overflow even in a wide window. Exercise their
// actual menu controls rather than triggering the action object behind the UI.
inline QQuickItem *fileToolbarControl(QQuickWindow *window, const char *name, int stableMs = 60)
{
    const auto controlName = QString::fromLatin1(name);
    if (auto *button = namedButton(window->contentItem(), controlName)) return button;
    auto *toolbar = visualItem(window->contentItem(), QStringLiteral("fileToolBar"));
    if (!toolbar) throw std::runtime_error("missing file toolbar");
    auto *more = waitForStable(window, "file toolbar overflow", [&] {
        return namedButton(toolbar, QStringLiteral("More Actions"));
    }, stableMs);
    const auto center = more->mapToScene(QPointF(more->width() / 2, more->height() / 2)).toPoint();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center);
    return waitForLayout(window, controlName, stableMs);
}

inline void closeFileToolbarMenu(QQuickWindow *window)
{
    QTest::keyClick(window, Qt::Key_Escape);
    settleFor("file toolbar overflow closed", [&] {
        return !namedButton(window->contentItem(), QStringLiteral("copyResult"));
    });
}

inline int visualLines(QQuickItem *editor, int blockNumber)
{
    auto *quick = editor->property("textDocument").value<QQuickTextDocument *>();
    if (!quick || !quick->textDocument()) throw std::runtime_error("missing text document");
    const auto *layout = quick->textDocument()->findBlockByNumber(blockNumber).layout();
    return layout ? layout->lineCount() : 0;
}

inline int paintedMarkers(QQuickItem *markers)
{
    int count = 0;
    auto *spots = markers->findChild<QQuickItem *>(markers->objectName() + QStringLiteral("Spots"));
    if (!spots) throw std::runtime_error("missing whitespace spots");
    for (auto *child : spots->childItems()) {
        if (child->isVisible() && child->width() >= 2.0 && child->height() >= 2.0)
            ++count;
    }
    return count;
}

inline int markerKind(QQuickItem *markers, bool tab)
{
    const auto expression = tab
        ? QStringLiteral("spots.filter(function(spot) { return spot.tab; }).length")
        : QStringLiteral("spots.filter(function(spot) { return !spot.tab; }).length");
    QQmlExpression query(qmlContext(markers), markers, expression);
    const auto value = query.evaluate();
    if (query.hasError()) throw std::runtime_error(query.error().toString().toStdString());
    return value.toInt();
}
}
