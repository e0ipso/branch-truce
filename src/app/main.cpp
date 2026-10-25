#include "ApplicationController.h"
#include "MergetoolLauncher.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QQmlApplicationEngine>
#include <QTextStream>
#include <QTimer>

namespace {
class RecoveryCloseGuard final : public QObject {
public:
    explicit RecoveryCloseGuard(branch_truce::SessionModel &model) : model_(model) {}
protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() != QEvent::Close || !model_.hasPendingRecovery())
            return QObject::eventFilter(object, event);
        const auto choice = QMessageBox::warning(nullptr, tr("Pending input needs recovery"),
            tr("Recovery could not keep the pending input. Retry before closing, or cancel to keep this window open."),
            QMessageBox::Retry | QMessageBox::Cancel, QMessageBox::Cancel);
        if (choice == QMessageBox::Retry) {
            for (int row = 0; row < model_.totalFiles(); ++row) {
                const auto id = model_.data(model_.index(row), branch_truce::SessionModel::FileIdRole).toString();
                (void)model_.retryPendingRecovery(id);
            }
        }
        if (!model_.hasPendingRecovery()) return QObject::eventFilter(object, event);
        event->ignore();
        return true;
    }
private:
    branch_truce::SessionModel &model_;
};
}

int main(int argc, char *argv[])
{
    for (int index = 1; index < argc; ++index) {
        if (qstrcmp(argv[index], "--mergetool") == 0)
            return branch_truce::runMergetoolLauncher(argc, argv);
    }
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Branch Truce"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("branch-truce.local"));
    // Matches the installed desktop entry so Plasma shows its name and icon.
    QGuiApplication::setDesktopFileName(QStringLiteral(BRANCH_TRUCE_APP_ID));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral(BRANCH_TRUCE_APP_ID)));
    branch_truce::ApplicationController controller;
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
    engine.setInitialProperties({{QStringLiteral("controller"), QVariant::fromValue(&controller)}});
    engine.loadFromModule(QStringLiteral("BranchTruce"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    RecoveryCloseGuard recoveryCloseGuard(*controller.files());
    for (auto *root : engine.rootObjects()) root->installEventFilter(&recoveryCloseGuard);
    // Finish or deliberate leave ends the process; the outcome stays visible
    // in the terminal the user returns to.
    QObject::connect(&controller, &branch_truce::ApplicationController::sessionEnded, &application,
                     [&application](const QString &outcome, bool) {
        QTextStream(stderr) << "Branch Truce: " << outcome << Qt::endl;
        QTimer::singleShot(0, &application, &QCoreApplication::quit);
    });
    if (parser.isSet(QStringLiteral("smoke-test"))) {
        QObject::connect(&controller, &branch_truce::ApplicationController::launchChanged, &application, [&] {
            if (controller.opening()) return;
            QJsonArray files;
            for (int row = 0; row < controller.files()->totalFiles(); ++row) {
                const auto index = controller.files()->index(row, 0);
                using Model = branch_truce::SessionModel;
                files.push_back(QJsonObject{
                    {QStringLiteral("path"), controller.files()->data(index, Model::PathRole).toString()},
                    {QStringLiteral("support"), controller.files()->data(index, Model::SupportRole).toString()},
                    {QStringLiteral("detail"), controller.files()->data(index, Model::SupportDetailRole).toString()},
                    {QStringLiteral("text"), controller.files()->data(index, Model::ResultTextRole).toString()}});
            }
            const QJsonObject state{{QStringLiteral("state"), controller.startupState()},
                {QStringLiteral("repository"), controller.repositoryRoot()},
                {QStringLiteral("operation"), controller.operation()},
                {QStringLiteral("detail"), controller.detail()},
                {QStringLiteral("stage2"), controller.stage2()}, {QStringLiteral("stage3"), controller.stage3()},
                {QStringLiteral("workflow"), controller.workflow()},
                {QStringLiteral("resolved"), controller.files()->resolvedFiles()}, {QStringLiteral("files"), files}};
            QTextStream(stdout) << "BRANCH_TRUCE_SESSION=" << QJsonDocument(state).toJson(QJsonDocument::Compact) << Qt::endl;
            QTimer::singleShot(0, &application, &QCoreApplication::quit);
        });
    }
    const auto directory = paths.isEmpty() ? QDir::currentPath() : QDir(paths.front()).absolutePath();
    const auto preferredFileId = parser.value(launchedPath);
    const auto chosenRecovery = parser.value(recoverySession);
    QTimer::singleShot(0, &controller, [&controller, directory, preferredFileId, chosenRecovery] {
        controller.openRepository(directory, preferredFileId, chosenRecovery);
    });
    return application.exec();
}
