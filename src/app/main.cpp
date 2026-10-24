#include "ApplicationController.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QTextStream>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Branch Truce"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("branch-truce.local"));
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
    parser.process(application);
    const auto paths = parser.positionalArguments();
    if (paths.size() > 1) parser.showHelp(2);

    QQmlApplicationEngine engine;
    engine.setInitialProperties({{QStringLiteral("controller"), QVariant::fromValue(&controller)}});
    engine.loadFromModule(QStringLiteral("BranchTruce"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
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
    QTimer::singleShot(0, &controller, [&controller, directory] { controller.openRepository(directory); });
    return application.exec();
}
