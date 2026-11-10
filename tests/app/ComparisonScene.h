// The real Main scene over an explicitly supplied application. Feature probes live with their consumers.
#pragma once

#include "ComparisonNetwork.h"
#include "app/Application.h"
#include "fixtures/ChoiceFile.h"
#include "fixtures/QmlWarnings.h"
#include "fixtures/QuickScene.h"
#include <QQmlApplicationEngine>

namespace comparison_scene {
using namespace branch_truce;
using test_support::settle;
using test_support::settleFor;

class Fixture {
public:
    explicit Fixture(Application &controller, FileSession file = test_support::batchFile())
        : store_(*controller.store()), selection_(*controller.selection())
    {
        CHECK(controller.store()->appendFile(std::move(file)));
        CHECK(controller.selection()->select("choices"));
        engine.setNetworkAccessManagerFactory(&network);
        test_support::failOnQmlWarnings(engine);
        engine.setInitialProperties(controller.windowProperties());
        engine.loadFromModule("BranchTruce", "Main");
        CHECK(engine.rootObjects().size() == 1);
        window = qobject_cast<QQuickWindow *>(engine.rootObjects().front());
        CHECK(window);
        window->resize(1500, 1100);
        window->show();
        settle([&] { return item("resultText")->width() > 0 && item("resultText")->height() > 0; });
    }
    QQuickItem *item(const char *name) const
    {
        auto *found = test_support::visualItem(window->contentItem(), QString::fromLatin1(name), true);
        if (!found) found = test_support::visualItem(window->contentItem(), QString::fromLatin1(name));
        if (!found) throw std::runtime_error(std::string("missing comparison item: ") + name);
        return found;
    }
    QObject *object(const char *name) const
    {
        auto *found = window->findChild<QObject *>(QString::fromLatin1(name));
        CHECK(found);
        return found;
    }
    const FileSession &file() { return *store_.file("choices"); }
    QString text() const { return item("resultText")->property("text").toString(); }
    void settleAligned()
    {
        settleFor("the typed edit aligned", [&] {
            const auto *shown = store_.file(selection_.fileId().toStdString());
            return !shown || !shown->pendingAlignment();
        });
    }
    void settleCommand()
    {
        settleFor("the clicked command committed", [&] { return !store_.commandPending(selection_.id()); });
        settleAligned();
    }
    [[nodiscard]] QList<QUrl> networkRequests() const { return network.requests(); }
    QQuickWindow *window = nullptr;

private:
    SessionStore &store_;
    Selection &selection_;
    // The factory outlives the engine and its image loader.
    RecordingNetwork network;
    QQmlApplicationEngine engine;
};

} // namespace comparison_scene
