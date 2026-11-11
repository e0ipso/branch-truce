// The appearance chooses the Qt Quick Controls style and the icon themes once, at startup.
#include "Appearance.h"
#include <QFileInfo>
#include <QIcon>
#include <QQmlEngine>
#include <QQuickStyle>
#include <algorithm>

namespace branch_truce {
void selectControlsStyle(const QQmlEngine &engine)
{
    if (!qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) return;
    const auto paths = engine.importPathList();
    const bool kdeStyle = std::any_of(paths.cbegin(), paths.cend(), [](const QString &path) {
        return QFileInfo::exists(path + QStringLiteral("/org/kde/desktop/qmldir"));
    });
    QQuickStyle::setStyle(kdeStyle ? QStringLiteral("org.kde.desktop") : QStringLiteral("Fusion"));
    if (kdeStyle) QQuickStyle::setFallbackStyle(QStringLiteral("Fusion"));
}

void selectIconThemes()
{
    QIcon::setFallbackThemeName(QStringLiteral("breeze"));
    if (QIcon::themeName().isEmpty()) QIcon::setThemeName(QStringLiteral("hicolor"));
}
}
