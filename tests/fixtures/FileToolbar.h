#pragma once

// Application-specific overflow controls, separate from generic scene lookup and readiness.
#include "fixtures/QuickScene.h"

namespace test_support {
inline bool fileOverflowControl(const char *name)
{
    // These actions may have hidden measurement delegates as well as live buttons.
    // Always resolve the visible native control, opening overflow when necessary.
    return QStringList{
        QStringLiteral("discardUnsaved"),    QStringLiteral("copyResult"),        QStringLiteral("pasteResult"),
        QStringLiteral("sourceChoicesMenu"), QStringLiteral("showSourceChoices"), QStringLiteral("saveDraft"),
        QStringLiteral("markResolved"),      QStringLiteral("undoCommand"),       QStringLiteral("redoCommand"),
        QStringLiteral("showEarlier"),       QStringLiteral("wrapLines"),         QStringLiteral("showWhitespace"),
        QStringLiteral("openPassageSearch")}
        .contains(QString::fromLatin1(name));
}

inline std::string controlGeometry(QQuickWindow *window, const QString &name)
{
    std::string state = "window=" + std::to_string(window->width()) + "x" + std::to_string(window->height());
    const auto visit = [&](auto &&self, QQuickItem *item) -> void {
        if (item->objectName() == name || item->objectName() == "fileToolBar" || locatorName(item) == "More Actions") {
            const auto rect = item->mapRectToScene(item->boundingRect());
            state += "; " + item->objectName().toStdString() + " visible=" + std::to_string(item->isVisible())
                + " enabled=" + std::to_string(item->isEnabled())
                + " reachable=" + std::to_string(reachable(window, item)) + " checked="
                + std::to_string(item->property("checked").toBool()) + " bounds=" + std::to_string(rect.x()) + ","
                + std::to_string(rect.y()) + "," + std::to_string(rect.width()) + "," + std::to_string(rect.height());
        }
        for (auto *child : item->childItems()) self(self, child);
    };
    visit(visit, window->contentItem());
    return state;
}

// Native overflow entries can disappear before the popup's exit transition and
// closed callback finish. Observe the popup and its owning button together so
// an old closed callback cannot uncheck a newly reopened menu.
inline void settleFileToolbarMenu(QQuickItem *more)
{
    settleFor("file toolbar popup transition", [&] {
        auto *menu = more->property("menu").value<QObject *>();
        if (!menu) return !more->property("checked").toBool();
        return menu->property("visible").toBool() ? menu->property("opened").toBool()
                                                  : !more->property("checked").toBool();
    });
}

// Exercise the actual native button or overflow entry, including disabled ones
// inspected by callers. Hidden measurement delegates never establish reachability.
inline QQuickItem *fileToolbarControl(QQuickWindow *window, const char *name, int stableMs = 60)
{
    const auto controlName = QString::fromLatin1(name);
    try {
        auto *toolbar = visualItem(window->contentItem(), QStringLiteral("fileToolBar"), true);
        if (!toolbar) throw std::runtime_error("missing file toolbar");
        if (auto *more = reachableButton(window, toolbar, QStringLiteral("More Actions"))) {
            settleFileToolbarMenu(more);
        }
        if (reachableButton(window, window->contentItem(), controlName)) {
            try {
                return waitForStable(
                    window, controlName.toStdString(),
                    [&] { return reachableButton(window, window->contentItem(), controlName); }, stableMs);
            } catch (const std::runtime_error &) {
                // A closing overflow entry can be reachable at the first
                // lookup and disappear during the stability wait. Reopen the
                // menu to inspect the current item, including disabled ones.
                if (reachableButton(window, window->contentItem(), controlName)) throw;
            }
        }
        auto *more = waitForStable(
            window, "file toolbar overflow",
            [&] { return reachableButton(window, toolbar, QStringLiteral("More Actions")); }, stableMs);
        settleFor("file toolbar native menu created",
                  [&] { return more->property("menu").value<QObject *>() != nullptr; });
        settleFileToolbarMenu(more);
        const auto center = more->mapToScene(QPointF(more->width() / 2, more->height() / 2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center);
        return waitForStable(
            window, controlName.toStdString() + " in native overflow",
            [&] { return reachableButton(window, window->contentItem(), controlName); }, stableMs);
    } catch (const std::exception &error) {
        throw std::runtime_error(std::string(error.what()) + "; " + controlGeometry(window, controlName));
    }
}

inline void closeFileToolbarMenu(QQuickWindow *window)
{
    auto *toolbar = visualItem(window->contentItem(), QStringLiteral("fileToolBar"), true);
    if (!toolbar) throw std::runtime_error("missing file toolbar");
    auto *more = reachableButton(window, toolbar, QStringLiteral("More Actions"));
    if (!more) return;
    settleFileToolbarMenu(more);
    auto *menu = more->property("menu").value<QObject *>();
    if (menu && menu->property("visible").toBool()) QTest::keyClick(window, Qt::Key_Escape);
    settleFor("file toolbar overflow closed", [&] {
        auto *popup = more->property("menu").value<QObject *>();
        return (!popup || !popup->property("visible").toBool()) && !more->property("checked").toBool();
    });
}
} // namespace test_support
