#include "ApplicationController.h"
#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Branch Truce"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("branch-truce.local"));
    branch_truce::ApplicationController controller;
    QCoreApplication::setApplicationVersion(controller.version());
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("applicationController"), &controller);
    engine.loadFromModule(QStringLiteral("BranchTruce"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    if (application.arguments().contains(QStringLiteral("--smoke-test"))) {
        QTimer::singleShot(0, &application, &QCoreApplication::quit);
    }
    return application.exec();
}
