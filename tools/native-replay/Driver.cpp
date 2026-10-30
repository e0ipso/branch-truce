// External preload driver: observe the real scene and send native Qt input only.
#include <QAccessible>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJSValue>
#include <QLocalSocket>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTest>
#include <QTimer>

namespace {
QPointer<QQuickWindow> window;
QLocalSocket *channel = nullptr;
QByteArray buffer;
QJsonObject request;
QElapsedTimer deadline, stable, paintStable;
QImage previousFrame;
QByteArray previous;
bool pending = false;
bool clicked = false;

QJsonValue value(const QVariant &variant) {
    if (variant.metaType() == QMetaType::fromType<QJSValue>())
        return QJsonValue::fromVariant(variant.value<QJSValue>().toVariant());
    return QJsonValue::fromVariant(variant);
}
QObject *controller() { return window ? window->property("controller").value<QObject *>() : nullptr; }
QObject *selected() {
    auto *owner = controller();
    return owner ? owner->property("selectedFile").value<QObject *>() : nullptr;
}
QQuickItem *find(QQuickItem *item, const QString &target, bool path = false) {
    if (!item->isVisible()) return nullptr;
    if (path ? item->property("path").toString() == target
             : item->objectName() == target) return item;
    for (auto *child : item->childItems())
        if (auto *found = find(child, target, path)) return found;
    return nullptr;
}
QJsonObject snapshot() {
    QJsonObject state;
    QJsonArray items;
    auto walk = [&](auto &&self, QQuickItem *item) -> void {
        if (!item->isVisible()) return;
        if (!item->objectName().isEmpty()) {
            const auto pos = item->mapToScene(QPointF());
            QJsonObject row{{"name", item->objectName()}, {"x", pos.x()}, {"y", pos.y()},
                {"width", item->width()}, {"height", item->height()}, {"enabled", item->isEnabled()},
                {"activeFocus", item->hasActiveFocus()}, {"opacity", item->opacity()}};
            if (auto *accessible = QAccessible::queryAccessibleInterface(item))
                row["accessibleName"] = accessible->text(QAccessible::Name);
            for (const auto *key : {"text", "path", "fileId", "contentY", "cursorPosition", "checked", "rows", "folds"}) {
                const auto property = item->property(key);
                if (property.isValid()) row[key] = value(property);
            }
            if (item->property("font").isValid()) row["font"] = item->property("font").value<QFont>().toString();
            items.append(row);
        }
        for (auto *child : item->childItems()) self(self, child);
    };
    walk(walk, window->contentItem());
    state["items"] = items;
    state["width"] = window->width();
    state["height"] = window->height();
    if (auto *file = selected())
        for (const auto *key : {"path", "status", "unsaved", "undecided", "progress", "regions", "choices", "decisions",
                               "resultText", "passages", "markerBlocks", "pendingInput", "recoveryState"})
            state[key] = value(file->property(key));
    return state;
}
void reply(const QString &error = {}) {
    QJsonObject result{{"error", error}};
    if (window) result["state"] = snapshot();
    channel->write(QJsonDocument(result).toJson(QJsonDocument::Compact) + '\n');
    channel->flush();
    pending = false;
}
bool loaded() {
    auto *owner = controller();
    auto *file = selected();
    if (!owner || !file || file->property("path").toString().isEmpty()) return false;
    auto *loader = owner->property("loader").value<QObject *>();
    return loader && !loader->property("opening").toBool()
        && !owner->property("resolutionBusy").toBool()
        && !file->property("pendingInput").toBool()
        && file->property("recoveryState").toString() == "durable";
}
void tick() {
    if (!pending) return;
    if (deadline.elapsed() > request.value("timeoutMs").toInt(30000)) {
        reply("Readiness/layout deadline exceeded: " + request.value("action").toString()
              + " " + request.value("target").toString());
        return;
    }
    if (!window) {
        for (auto *candidate : QGuiApplication::topLevelWindows()) {
            auto *quick = qobject_cast<QQuickWindow *>(candidate);
            if (quick && quick->property("controller").isValid()) { window = quick; break; }
        }
    }
    if (!window || !window->isExposed() || !loaded()) { stable.restart(); return; }
    const auto path = request.value("path").toString();
    if (!path.isEmpty() && selected()->property("path").toString() != path) { stable.restart(); return; }
    const auto action = request.value("action").toString();
    const auto target = request.value("target").toString();
    QQuickItem *item = nullptr;
    if (!target.isEmpty() && !clicked) {
        item = find(window->contentItem(), target, action == "click-path");
        if (!item || !item->isEnabled() || item->width() <= 0 || item->height() <= 0) { stable.restart(); return; }
        const auto point = item->mapToScene(QPointF(item->width()/2, item->height()/2));
        if (!QRectF(0, 0, window->width(), window->height()).contains(point)) { stable.restart(); return; }
        // Reject clipped targets. Never click their former layout position.
        for (auto *parent = item->parentItem(); parent; parent = parent->parentItem()) {
            if (parent->clip() && !parent->contains(parent->mapFromScene(point))) { stable.restart(); return; }
        }
        if (request.contains("text") && item->property("text").toString() != request.value("text").toString()) {
            stable.restart(); return;
        }
    }
    const auto current = QJsonDocument(snapshot()).toJson(QJsonDocument::Compact);
    if (current != previous) { previous = current; stable.restart(); return; }
    if (stable.elapsed() < 350) return;
    if ((action == "click" || action == "click-path") && !clicked) {
        const auto point = item->mapToScene(QPointF(item->width()/2, item->height()/2)).toPoint();
        QTest::mouseMove(window, point);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point);
        clicked = true;
        previous.clear();
        stable.restart();
        return;
    }
    if (action == "resize" && !clicked) {
        window->resize(request["width"].toInt(), request["height"].toInt());
        clicked = true; stable.restart(); return;
    }
    if (action == "neutral" && !clicked) {
        QTest::mouseMove(window, QPoint(window->width()-5, window->height()-40));
        clicked = true; stable.restart(); return;
    }
    if (action == "capture-ready") {
        // Anonymous icon/colour transitions can keep painting after named geometry
        // settles. Observe real decoded frames before asking the compositor to shoot.
        const auto frame = window->grabWindow();
        if (frame.isNull()) { reply("Native window frame unavailable"); return; }
        if (frame != previousFrame) {
            previousFrame = frame; paintStable.restart(); return;
        }
        if (paintStable.elapsed() < 700) return;
        reply(); return;
    }
    if (action == "quit") {
        reply();
        QCoreApplication::quit();
        return;
    }
    if (action != "wait" && action != "click" && action != "click-path" && action != "resize" && action != "neutral") {
        reply("Unknown action: " + action); return;
    }
    reply();
}
void begin() {
    bool ok = false;
    const auto fd = qEnvironmentVariableIntValue("BRANCH_TRUCE_REPLAY_FD", &ok);
    if (!ok) return;
    channel = new QLocalSocket(QCoreApplication::instance());
    if (!channel->setSocketDescriptor(fd)) qFatal("Cannot open native replay channel");
    QObject::connect(channel, &QLocalSocket::readyRead, [] {
        buffer += channel->readAll();
        const auto newline = buffer.indexOf('\n');
        if (newline < 0) return;
        if (pending) qFatal("Native replay commands must be serialized");
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(buffer.left(newline), &error);
        buffer.remove(0, newline+1);
        if (error.error != QJsonParseError::NoError || !document.isObject()) qFatal("Invalid replay command");
        request = document.object();
        pending = true; clicked = false; previous.clear(); previousFrame = QImage();
        deadline.start(); stable.start(); paintStable.start();
    });
    auto *timer = new QTimer(channel);
    QObject::connect(timer, &QTimer::timeout, tick);
    timer->start(20);
}
void schedule() { QTimer::singleShot(0, begin); }
}
Q_COREAPP_STARTUP_FUNCTION(schedule)
