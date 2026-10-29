#include "ApplicationController.h"
#include "MergetoolLauncher.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <algorithm>

namespace {
// The KDE style takes its colours from the user's KDE colour scheme. Fusion
// stands in where that style is not installed. QT_QUICK_CONTROLS_STYLE wins.
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
}

int main(int argc, char *argv[])
{
    // The one PATH lookup for Git. Every Git call in this process runs this
    // executable; it is empty when Git is not on PATH.
    const auto git = QStandardPaths::findExecutable(QStringLiteral("git"));
    for (int index = 1; index < argc; ++index) {
        if (qstrcmp(argv[index], "--mergetool") == 0)
            return branch_truce::runMergetoolLauncher(argc, argv, git);
    }
    // QApplication rather than QGuiApplication: the org.kde.desktop style draws its
    // controls through QApplication::style(). See docs/platform-validation.md.
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Branch Truce"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Branch Truce"));
    // No organization name: KDE apps set the domain only. See docs/platform-validation.md.
    QCoreApplication::setOrganizationDomain(QStringLiteral("canpicasoft.com"));
    // Matches the installed desktop entry so Plasma shows its name and icon.
    QGuiApplication::setDesktopFileName(QStringLiteral(BRANCH_TRUCE_APP_ID));
    // Off Plasma the icon theme is hicolor or none, so icon.name falls back to Breeze.
    QIcon::setFallbackThemeName(QStringLiteral("breeze"));
    // Platforms such as offscreen or vnc name no icon theme. With no desktop
    // named either, Kirigami then swaps in a bundled theme and search path that
    // distro builds lack. Naming hicolor, as Qt does on xcb and Wayland, keeps
    // the system path, so the Breeze fallback still applies.
    if (QIcon::themeName().isEmpty()) QIcon::setThemeName(QStringLiteral("hicolor"));
    // An installed icon wins. Isolated launches and the build tree have none, so the
    // bundled symbol keeps the window and its taskbar entry from going blank.
    auto applicationIcon = QIcon::fromTheme(QStringLiteral(BRANCH_TRUCE_APP_ID));
    if (applicationIcon.isNull()) applicationIcon = QIcon(QStringLiteral(":/branch-truce/icons/branch-truce-symbol.svg"));
    QApplication::setWindowIcon(applicationIcon);
    branch_truce::ApplicationController controller(git);
    QCoreApplication::setApplicationVersion(controller.version());
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Open local merge or rebase conflicts offline."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("repository"),
                                QStringLiteral("Repository directory, defaults to the current directory."),
                                QStringLiteral("[repository]"));
    parser.addOption({QStringLiteral("smoke-test"), QStringLiteral("Load the window and session, report startup state, and exit.")});
    // A mergetool launcher that starts the GUI names the path Git launched,
    // hex-encoded like its attach frame, so the window opens on it.
    QCommandLineOption launchedPath(QStringLiteral("mergetool-path"),
                                    QStringLiteral("Select this hex-encoded repository path first."),
                                    QStringLiteral("hex-path"));
    launchedPath.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(launchedPath);
    QCommandLineOption recoverySession(QStringLiteral("recovery-session"),
        QStringLiteral("Restore this recovery session when several match the operation; the others are set aside, not deleted."),
        QStringLiteral("session"));
    parser.addOption(recoverySession);
    parser.process(application);
    const auto paths = parser.positionalArguments();
    if (paths.size() > 1) parser.showHelp(2);

    QQmlApplicationEngine engine;
    selectControlsStyle(engine);
    engine.setInitialProperties({{QStringLiteral("controller"), QVariant::fromValue(&controller)}});
    engine.loadFromModule(QStringLiteral("BranchTruce"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    // Finish or deliberate leave ends the process; the outcome stays visible
    // in the terminal the user returns to.
    QObject::connect(&controller, &branch_truce::ApplicationController::sessionEnded, &application,
                     [&application](const QString &outcome, bool) {
        QTextStream(stderr) << "Branch Truce: " << outcome << Qt::endl;
        QTimer::singleShot(0, &application, &QCoreApplication::quit);
    });
    if (parser.isSet(QStringLiteral("smoke-test"))) {
        auto *loader = controller.loader();
        QObject::connect(loader, &branch_truce::SessionLoader::launchChanged, &application, [&controller, &application, loader] {
            if (loader->opening()) return;
            QJsonArray files;
            for (int row = 0; row < controller.files()->totalFiles(); ++row) {
                const auto index = controller.files()->index(row, 0);
                using Model = branch_truce::SessionModel;
                files.push_back(QJsonObject{
                    {QStringLiteral("path"), controller.files()->data(index, Model::PathRole).toString()},
                    {QStringLiteral("support"), controller.files()->data(index, Model::SupportRole).toString()},
                    {QStringLiteral("detail"), controller.files()->data(index, Model::SupportDetailRole).toString()},
                    {QStringLiteral("text"), QString::fromUtf8(QByteArray::fromStdString(controller.files()->fileAt(row)->current().draft.text))}});
            }
            const QJsonObject state{{QStringLiteral("state"), loader->startupState()},
                {QStringLiteral("repository"), loader->repositoryRoot()},
                {QStringLiteral("operation"), loader->operation()},
                {QStringLiteral("detail"), loader->detail()},
                {QStringLiteral("stage2"), loader->stage2()}, {QStringLiteral("stage3"), loader->stage3()},
                {QStringLiteral("stage2CommitId"), loader->stage2CommitId()}, {QStringLiteral("stage3CommitId"), loader->stage3CommitId()},
                {QStringLiteral("workflow"), loader->workflow()},
                {QStringLiteral("resolved"), controller.files()->resolvedFiles()}, {QStringLiteral("files"), files}};
            QTextStream(stdout) << "BRANCH_TRUCE_SESSION=" << QJsonDocument(state).toJson(QJsonDocument::Compact) << Qt::endl;
            QTimer::singleShot(0, &application, &QCoreApplication::quit);
        });
    }
    const auto directory = paths.isEmpty() ? QDir::currentPath() : QDir(paths.front()).absolutePath();
    const auto preferredFileId = parser.value(launchedPath);
    const auto chosenRecovery = parser.value(recoverySession);
    QTimer::singleShot(0, &controller, [&controller, directory, preferredFileId, chosenRecovery] {
        controller.loader()->openRepository(directory, preferredFileId, chosenRecovery);
    });
    return application.exec();
}
