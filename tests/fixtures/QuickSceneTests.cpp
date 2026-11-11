#include "TestHarness.h"
#include "fixtures/QuickScene.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <memory>

int main(int argc, char **argv)
{
    test_support::configure(argc, argv);
    QGuiApplication application(argc, argv);
    test_support::Suite suite("QuickSceneTests");
    suite.run("a click follows hover layout and activates its target exactly once", [] {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import QtQuick.Controls
            Window {
                id: root
                width: 500; height: 300; visible: true
                property bool shifted: false
                property int activations: 0
                Button {
                    objectName: "movingButton"
                    x: root.shifted ? 250 : 20
                    y: 20; width: 100; height: 40
                    text: "Resolve"
                    hoverEnabled: true
                    onHoveredChanged: if (hovered) root.shifted = true
                    onClicked: root.activations++
                }
            }
        )",
                          QUrl("inmemory:click-test.qml"));
        test_support::waitUntil([&] { return !component.isLoading(); }, "click component did not load");
        if (component.isError()) throw test_support::Failure(component.errorString().toStdString());
        const std::unique_ptr<QObject> root(component.create());
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        CHECK(window);
        QTest::mouseMove(window, QPoint(450, 250));
        auto *target = test_support::waitForLayout(window, "movingButton", 60);
        test_support::hoverAndClickItem(window, target);
        CHECK(window->property("shifted").toBool());
        CHECK_EQ(window->property("activations").toInt(), 1);
    });
    return suite.report();
}
