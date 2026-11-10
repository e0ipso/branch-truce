// The desktop entry point selects the native appearance and opens repository or Git mergetool sessions.
#include "AppPaths.h"
#include "Appearance.h"
#include "Application.h"
#include "StartupReport.h"
#include "Translations.h"
#include "core/BuildInfo.h"
#include "mergetool/MergetoolLauncher.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QJsonDocument>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <algorithm>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>

namespace {
// SIGTERM, SIGINT and SIGHUP end the event loop instead of the process, so the
// session closes its launcher socket and stops its Git processes in order. The
// handler only writes to a pipe; the event loop reads it.
int signalPipe[2]{-1, -1};
void quitOnSignals(QCoreApplication &application)
{
    if (::pipe2(signalPipe, O_CLOEXEC | O_NONBLOCK) != 0) return;
    struct sigaction action{};
    action.sa_handler = [](int) {
        const char byte = 1;
        [[maybe_unused]] const auto written = ::write(signalPipe[1], &byte, 1);
    };
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    for (const int signal : {SIGTERM, SIGINT, SIGHUP}) ::sigaction(signal, &action, nullptr);
    auto *notifier = new QSocketNotifier(signalPipe[0], QSocketNotifier::Read, &application);
    QObject::connect(notifier, &QSocketNotifier::activated, &application, [] {
        char byte = 0;
        while (::read(signalPipe[0], &byte, 1) > 0) {}
        QCoreApplication::quit();
    });
}
}

int main(int argc, char *argv[])
{
    // The one PATH lookup for Git. Every Git call in this process runs this
    // executable; it is empty when Git is not on PATH. It reads the
    // configuration this process's environment selects, as the user's Git does.
    const branch_truce::git::GitProgram git{QStandardPaths::findExecutable(QStringLiteral("git")),
                                            branch_truce::git::GitEnvironment::fromProcess()};
    // Git's mergetool command line names the launcher mode first. Only that
    // position selects it, so a repository or option value spelled
    // --mergetool still opens the window.
    if (argc > 1 && qstrcmp(argv[1], "--mergetool") == 0) {
        QCoreApplication launcher(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("Branch Truce"));
        QCoreApplication::setOrganizationDomain(QStringLiteral("canpicasoft.com"));
        branch_truce::installTranslations(launcher);
        return branch_truce::runMergetoolLauncher(QCoreApplication::arguments(), git);
    }
    // QApplication rather than QGuiApplication: the org.kde.desktop style draws its
    // controls through QApplication::style(). See docs/platform-validation.md.
    QApplication application(argc, argv);
    quitOnSignals(application);
    branch_truce::installTranslations(application);
    QCoreApplication::setApplicationName(QStringLiteral("Branch Truce"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Branch Truce"));
    // No organization name: KDE apps set the domain only. See docs/platform-validation.md.
    QCoreApplication::setOrganizationDomain(QStringLiteral("canpicasoft.com"));
    // Matches the installed desktop entry so Plasma shows its name and icon.
    QGuiApplication::setDesktopFileName(QStringLiteral(BRANCH_TRUCE_APP_ID));
    branch_truce::selectIconThemes();
    // An installed icon wins. Isolated launches and the build tree have none, so the
    // bundled symbol keeps the window and its taskbar entry from going blank.
    auto applicationIcon = QIcon::fromTheme(QStringLiteral(BRANCH_TRUCE_APP_ID));
    if (applicationIcon.isNull())
        applicationIcon = QIcon(QStringLiteral(":/branch-truce/icons/branch-truce-symbol.svg"));
    QApplication::setWindowIcon(applicationIcon);
    const auto version = branch_truce::version();
    QCoreApplication::setApplicationVersion(QString::fromUtf8(version.data(), static_cast<qsizetype>(version.size())));
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QCoreApplication::translate("main", "Open local merge or rebase conflicts offline."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(
        QStringLiteral("repository"),
        QCoreApplication::translate("main", "Repository directory, defaults to the current directory."),
        QStringLiteral("[repository]"));
    parser.addOption(
        {QStringLiteral("smoke-test"),
         QCoreApplication::translate("main", "Load the window and session, report startup state, and exit.")});
    // Listed for --help only: the launcher mode is selected before parsing,
    // and only as the first argument.
    parser.addOption({QStringLiteral("mergetool"),
                      QCoreApplication::translate(
                          "main", "As the first argument: run as the blocking launcher for git mergetool.")});
    // A mergetool launcher that starts the GUI names the path Git launched,
    // hex-encoded like its attach frame, so the window opens on it.
    QCommandLineOption launchedPath(
        QStringLiteral("mergetool-path"),
        QCoreApplication::translate("main", "Select this hex-encoded repository path first."),
        QStringLiteral("hex-path"));
    launchedPath.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(launchedPath);
    QCommandLineOption recoverySession(
        QStringLiteral("recovery-session"),
        QCoreApplication::translate(
            "main",
            "Restore this recovery session when several match the operation; the others are set aside, not deleted."),
        QStringLiteral("session"));
    parser.addOption(recoverySession);
    parser.process(application);
    if (parser.isSet(QStringLiteral("mergetool"))) {
        QTextStream(stderr) << QCoreApplication::translate("main", "--mergetool must be the first argument.")
                            << Qt::endl;
        return 2;
    }
    const auto paths = parser.positionalArguments();
    if (paths.size() > 1) parser.showHelp(2);

    // The composition root: the data roots and services this session uses.
    branch_truce::Application session(
        {.git = git,
         .recoveryDirectory = branch_truce::newRecoveryDirectory(),
         .mergetoolDirectory = branch_truce::mergetoolSocketDirectory(qgetenv("XDG_RUNTIME_DIR"))});
    QQmlApplicationEngine engine;
    branch_truce::selectControlsStyle(engine);
    engine.setInitialProperties(session.windowProperties());
    engine.loadFromModule(QStringLiteral("BranchTruce"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    // Finish or deliberate leave ends the process; the outcome stays visible
    // in the terminal the user returns to.
    QObject::connect(session.completion(), &branch_truce::SessionCompletion::sessionEnded, &application,
                     [&application](const QString &outcome, bool) {
                         QTextStream(stderr) << "Branch Truce: " << outcome << Qt::endl;
                         QTimer::singleShot(0, &application, &QCoreApplication::quit);
                     });
    if (parser.isSet(QStringLiteral("smoke-test"))) {
        auto *loader = session.loader();
        QObject::connect(
            loader, &branch_truce::SessionLoader::launchChanged, &application, [&session, &application, loader] {
                if (loader->opening()) return;
                const auto report = branch_truce::startupReport(*loader, *session.files());
                QTextStream(stdout) << "BRANCH_TRUCE_SESSION=" << QJsonDocument(report).toJson(QJsonDocument::Compact)
                                    << Qt::endl;
                QTimer::singleShot(0, &application, &QCoreApplication::quit);
            });
    }
    const auto directory = paths.isEmpty() ? QDir::currentPath() : QDir(paths.front()).absolutePath();
    const auto preferredFileId = parser.value(launchedPath);
    const auto chosenRecovery = parser.value(recoverySession);
    QTimer::singleShot(0, &session, [&session, directory, preferredFileId, chosenRecovery] {
        session.loader()->openRepository(directory, preferredFileId, chosenRecovery);
    });
    return application.exec();
}
