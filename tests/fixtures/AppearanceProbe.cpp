// Preloaded into the real branch-truce binary with LD_PRELOAD. Once main has
// created its window, it prints one BRANCH_TRUCE_APPEARANCE= line with the
// Qt Quick Controls style, icon theme state and window icon the process ended up with.
#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPixmap>
#include <QQuickStyle>
#include <QTimer>
#include <QWindow>
#include <cstdio>

namespace {
// True when the icon renders at this size and paints at least one visible pixel,
// so a transparent placeholder does not pass for an icon.
bool rendersAt(const QIcon &icon, int edge)
{
    const auto image = icon.pixmap(QSize(edge, edge)).toImage().convertToFormat(QImage::Format_ARGB32);
    if (image.isNull()) return false;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) != 0) return true;
        }
    }
    return false;
}

void report()
{
    // main selects the style before it loads Main, which creates the window.
    if (QGuiApplication::topLevelWindows().isEmpty()) {
        QTimer::singleShot(10, QCoreApplication::instance(), report);
        return;
    }
    // The window's own icon, which falls back to the application icon main set.
    const auto windowIcon = QGuiApplication::topLevelWindows().constFirst()->icon();
    const auto desktopName = QGuiApplication::desktopFileName();
    const QJsonObject icon{{QStringLiteral("null"), windowIcon.isNull()},
        // A theme icon carries its lookup name; the bundled resource has none.
        {QStringLiteral("name"), windowIcon.name()},
        {QStringLiteral("themeHasApplicationIcon"), QIcon::hasThemeIcon(desktopName)},
        {QStringLiteral("small"), rendersAt(windowIcon, 16)},
        {QStringLiteral("large"), rendersAt(windowIcon, 128)}};
    const QJsonObject state{{QStringLiteral("windowIcon"), icon}, {QStringLiteral("style"), QQuickStyle::name()},
        {QStringLiteral("theme"), QIcon::themeName()},
        {QStringLiteral("fallbackTheme"), QIcon::fallbackThemeName()},
        {QStringLiteral("documentSave"), !QIcon::fromTheme(QStringLiteral("document-save")).isNull()},
        {QStringLiteral("displayName"), QGuiApplication::applicationDisplayName()}};
    std::printf("BRANCH_TRUCE_APPEARANCE=%s\n", QJsonDocument(state).toJson(QJsonDocument::Compact).constData());
    std::fflush(stdout);
}

void schedule()
{
    QTimer::singleShot(0, QCoreApplication::instance(), report);
}
}

Q_COREAPP_STARTUP_FUNCTION(schedule)
