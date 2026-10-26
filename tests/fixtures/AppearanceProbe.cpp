// Preloaded into the real branch-truce binary with LD_PRELOAD. Once main has
// created its window, it prints one BRANCH_TRUCE_APPEARANCE= line with the
// Qt Quick Controls style and icon theme state the process ended up with.
#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickStyle>
#include <QTimer>
#include <QWindow>
#include <cstdio>

namespace {
void report()
{
    // main selects the style before it loads Main, which creates the window.
    if (QGuiApplication::topLevelWindows().isEmpty()) {
        QTimer::singleShot(10, QCoreApplication::instance(), report);
        return;
    }
    const QJsonObject state{{QStringLiteral("style"), QQuickStyle::name()},
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
