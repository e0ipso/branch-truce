// External preload driver: observe the real scene and send native Qt input only.
#include "../../src/app/FileStates.h"
#include <QAccessible>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFont>
#include <QFontInfo>
#include <QGuiApplication>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QMetaProperty>
#include <QPalette>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QScreen>
#include <QSequentialIterable>
#include <QSettings>
#include <QStringList>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QTimer>

#include <cmath>
#include <limits>
#include <optional>

namespace {
QPointer<QQuickWindow> window;
QLocalSocket *channel = nullptr;
QByteArray buffer;
QList<QByteArray> queued;

// One request line: {"action": ..., "target"?, "path"?, "text"?, "line"?, "gesture"?, "timeoutMs"?}.
struct Request {
    QString action, target, path, gesture = QStringLiteral("right-click");
    std::optional<QString> text;
    int line = 0;
    int timeoutMs = 30000;
};

// Progress of the request being served. Each request starts from a fresh value.
struct Progress {
    QElapsedTimer deadline, stable, paintStable;
    QByteArray previous;
    QImage previousFrame;
    bool acted = false;
    bool overflowOpened = false;
    bool sourcePositioned = false;

    Progress()
    {
        deadline.start();
        stable.start();
        paintStable.start();
    }
    // The scene changes on purpose: wait for a fresh settled snapshot before the next step.
    void unsettle()
    {
        previous.clear();
        stable.restart();
    }
};

// One step of an action, run once the scene has settled: keep waiting, reply, or reply with an error.
struct Outcome {
    enum class Kind { Pending, Done, Failed } kind = Kind::Pending;
    QString error;

    static Outcome pending() { return {}; }
    static Outcome done() { return {Kind::Done, {}}; }
    static Outcome failed(const QString &error) { return {Kind::Failed, error}; }
};

// How an action's target names its control.
enum class Lookup { None, ObjectName, Path, AccessibleName, Toolbar };

struct Handler {
    Lookup lookup;
    QStringList required, optional; // Request fields besides action, path and timeoutMs.
    Outcome (*step)(const Request &, Progress &, QQuickItem *);
};

struct Run {
    Request request;
    const Handler *handler = nullptr;
    Progress progress;
};
std::optional<Run> current;

QJsonValue value(const QVariant &variant)
{
    if (variant.metaType() == QMetaType::fromType<QJSValue>())
        return QJsonValue::fromVariant(variant.value<QJSValue>().toVariant());
    if (variant.metaType().flags().testFlag(QMetaType::IsEnumeration)) return variant.toInt();
    if (variant.metaType().flags().testFlag(QMetaType::IsGadget)) {
        QJsonObject object;
        const auto *meta = variant.metaType().metaObject();
        for (int i = 0; i < meta->propertyCount(); ++i) {
            const auto property = meta->property(i);
            object[property.name()] = value(property.readOnGadget(variant.constData()));
        }
        return object;
    }
    if (variant.canConvert<QSequentialIterable>()) {
        QJsonArray array;
        for (const auto &entry : variant.value<QSequentialIterable>()) array.append(value(entry));
        return array;
    }
    return QJsonValue::fromVariant(variant);
}
// Main's required service objects are the app boundary, also used for discovery.
struct WindowBinding {
    QObject *selectedFile, *loader, *commands, *files;
    static std::optional<WindowBinding> bind(QQuickWindow *candidate)
    {
        WindowBinding binding{
            candidate->property("selectedFile").value<QObject *>(), candidate->property("loader").value<QObject *>(),
            candidate->property("commands").value<QObject *>(), candidate->property("files").value<QObject *>()};
        if (!binding.selectedFile || !binding.loader || !binding.commands || !binding.files) return {};
        return binding;
    }
    bool ready() const
    {
        const auto recovery = selectedFile->property("recoveryState");
        return recovery.isValid() && !selectedFile->property("path").toString().isEmpty()
            && !loader->property("opening").toBool() && !commands->property("resolutionBusy").toBool()
            && !selectedFile->property("pendingInput").toBool()
            && recovery.toInt() == int(branch_truce::FileStates::RecoveryState::Durable);
    }
};
std::optional<WindowBinding> binding() { return window ? WindowBinding::bind(window) : std::nullopt; }
bool reachable(QQuickItem *item)
{
    if (!item || !item->isVisible() || item->width() <= 0 || item->height() <= 0) return false;
    const auto point = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    const auto rect = item->mapRectToScene(item->boundingRect());
    const bool button = item->inherits("QQuickAbstractButton");
    if (!QRectF(0, 0, window->width(), window->height()).contains(point)) return false;
    if (button && !QRectF(0, 0, window->width(), window->height()).contains(rect)) return false;
    for (auto *parent = item->parentItem(); parent; parent = parent->parentItem())
        if (parent->clip()
            && (!parent->contains(parent->mapFromScene(point))
                || (button && !parent->mapRectToScene(parent->boundingRect()).contains(rect))))
            return false;
    return true;
}
QQuickItem *find(QQuickItem *item, const QString &target, bool path = false)
{
    if (!item || !item->isVisible()) return nullptr;
    if ((path ? item->property("path").toString() == target : item->objectName() == target) && reachable(item))
        return item;
    for (auto *child : item->childItems())
        if (auto *found = find(child, target, path)) return found;
    return nullptr;
}
QQuickItem *accessibleButton(QQuickItem *item, const QString &target)
{
    if (!item || !item->isVisible()) return nullptr;
    if (item->inherits("QQuickAbstractButton") && reachable(item)) {
        auto *accessible = QAccessible::queryAccessibleInterface(item);
        const auto name = accessible ? accessible->text(QAccessible::Name) : QString();
        if (name == target || item->property("text").toString() == target) return item;
    }
    for (auto *child : item->childItems())
        if (auto *found = accessibleButton(child, target)) return found;
    return nullptr;
}
QJsonObject fontInfo(const QFont &font)
{
    const QFontInfo resolved(font);
    return {{"requestedFamily", font.family()},
            {"resolvedFamily", resolved.family()},
            {"pointSize", font.pointSizeF()},
            {"pixelSize", resolved.pixelSize()},
            {"exactMatch", resolved.exactMatch()}};
}
QJsonObject profile()
{
    // Probe theme roles in the application's engine, without changing its scene.
    static QPointer<QObject> probe;
    if (!probe) {
        auto *engine = qmlEngine(window);
        if (!engine) qFatal("Native scene has no QML engine");
        QQmlComponent component(engine);
        component.setData(R"QML(
import QtQuick
import org.kde.kirigami as K
Item {
    visible: false
    property font normalFont: K.Theme.defaultFont
    property font smallFont: K.Theme.smallFont
    property font fixedFont: K.Theme.fixedWidthFont
    readonly property var units: ({gridUnit: K.Units.gridUnit, smallSpacing: K.Units.smallSpacing,
                                   mediumSpacing: K.Units.mediumSpacing, largeSpacing: K.Units.largeSpacing})

}
)QML",
                          QUrl("native-calibration.qml"));
        probe = component.create();
        if (!probe) qFatal("Cannot observe Kirigami profile: %s", qPrintable(component.errorString()));
        probe->setParent(engine);
        qobject_cast<QQuickItem *>(probe)->setParentItem(window->contentItem());
    }
    QJsonObject fonts;
    for (const auto *key : {"normal", "small", "fixed"})
        fonts[key] = fontInfo(probe->property((QByteArray(key) + "Font").constData()).value<QFont>());
    QSettings settings(qEnvironmentVariable("XDG_CONFIG_HOME") + "/kdeglobals", QSettings::IniFormat);
    const auto actualPalette = QGuiApplication::palette();
    const QJsonObject palette{{"window", actualPalette.color(QPalette::Window).name()},
                              {"viewBase", actualPalette.color(QPalette::Base).name()},
                              {"viewAlternateBase", actualPalette.color(QPalette::AlternateBase).name()},
                              {"text", actualPalette.color(QPalette::Text).name()},
                              {"disabledText", actualPalette.color(QPalette::Disabled, QPalette::Text).name()},
                              {"highlight", actualPalette.color(QPalette::Highlight).name()}};
    auto *screen = window->screen();
    return {{"qtVersion", qVersion()},
            {"configuredColourScheme", settings.value("ColorScheme").toString()},
            {"platform", QGuiApplication::platformName()},
            {"quickControlsStyle", QQuickStyle::name()},
            {"iconTheme", QIcon::themeName()},
            {"fallbackIconTheme", QIcon::fallbackThemeName()},
            {"fonts", fonts},
            {"units", value(probe->property("units"))},
            {"palette", palette},
            {"devicePixelRatio", window->devicePixelRatio()},
            {"logicalDpi", screen->logicalDotsPerInch()},
            {"physicalDpi", screen->physicalDotsPerInch()},
            {"screenName", screen->name()},
            {"physicalDesktop", false}};
}
QJsonObject snapshot()
{
    QJsonObject state;
    QJsonArray items, details;
    auto walk = [&](auto &&self, QQuickItem *item) -> void {
        if (!item->isVisible()) return;
        if (!item->objectName().isEmpty() || item->property("line").isValid() || item->property("text").isValid()
            || item->property("color").isValid()) {
            const auto pos = item->mapToScene(QPointF());
            bool painted = true;
            for (auto *ancestor = item; ancestor; ancestor = ancestor->parentItem())
                painted = painted && ancestor->isVisible() && ancestor->opacity() > 0;
            QJsonObject row{{"name", item->objectName()},
                            {"x", pos.x()},
                            {"y", pos.y()},
                            {"width", item->width()},
                            {"height", item->height()},
                            {"enabled", item->isEnabled()},
                            {"activeFocus", item->hasActiveFocus()},
                            {"opacity", item->opacity()},
                            {"onScreen", reachable(item) && painted},
                            {"clip", item->clip()},
                            {"className", item->metaObject()->className()}};
            QStringList ancestors;
            for (auto *parent = item->parentItem(); parent; parent = parent->parentItem())
                if (!parent->objectName().isEmpty()) ancestors.append(parent->objectName());
            row["ancestors"] = QJsonArray::fromStringList(ancestors);
            if (auto *accessible = QAccessible::queryAccessibleInterface(item)) {
                row["accessibleName"] = accessible->text(QAccessible::Name);
                row["accessibleDescription"] = accessible->text(QAccessible::Description);
            }
            for (const auto *key : {"text",           "path",
                                    "fileId",         "contentY",
                                    "cursorPosition", "checked",
                                    "rows",           "folds",
                                    "rowHeight",      "lineSpacing",
                                    "color",          "schemeBackground",
                                    "line",           "kind",
                                    "truncated",      "explanation",
                                    "implicitWidth",  "baselineOffset",
                                    "textFormat",     "readOnly",
                                    "selectByMouse",  "selectByKeyboard",
                                    "sourceTop",      "sourceBottom",
                                    "resultTop",      "resultBottom",
                                    "sourceX",        "resultX",
                                    "middleX",        "radius",
                                    "orientation"}) {
                const auto property = item->property(key);
                if (property.isValid()) row[key] = value(property);
            }
            const auto textFormat = item->property("textFormat");
            if (textFormat.isValid()) {
                bool converted = false;
                const auto format = textFormat.toInt(&converted);
                row["plainText"] = converted && format == int(Qt::PlainText);
            }
            if (item->property("font").isValid()) row["font"] = item->property("font").value<QFont>().toString();
            if (item->objectName().endsWith("Label") && item->parentItem() && item->parentItem()->parentItem()) {
                auto *header = item->parentItem()->parentItem()->parentItem();
                if (header) {
                    const auto p = header->mapToScene(QPointF());
                    row["ancestorGeometry"] = QJsonArray{p.x(), p.y(), header->width(), header->height()};
                    row["ancestorColor"] = value(header->property("color"));
                }
            }
            if (item->objectName() == "stage2Text" || item->objectName() == "stage3Text"
                || item->objectName() == "resultText") {
                auto *quick = item->property("textDocument").value<QQuickTextDocument *>();
                QJsonArray formats;
                if (quick)
                    for (auto block = quick->textDocument()->begin(); block.isValid(); block = block.next()) {
                        for (const auto &range : block.layout()->formats())
                            formats.append(QJsonObject{{"line", block.blockNumber() + 1},
                                                       {"start", range.start},
                                                       {"length", range.length},
                                                       {"foreground", range.format.foreground().color().name()},
                                                       {"weight", range.format.fontWeight()}});
                    }
                row["syntaxFormats"] = formats;
            }
            if (!item->objectName().isEmpty())
                items.append(row);
            else
                details.append(row);
        }
        for (auto *child : item->childItems()) self(self, child);
    };
    walk(walk, window->contentItem());
    state["items"] = items;
    state["renderedDetails"] = details;
    state["width"] = window->width();
    state["height"] = window->height();
    state["profile"] = profile();
    if (const auto app = binding()) {
        for (const auto *key : {"path", "status", "unsaved", "undecided", "progress", "decisions", "resultText",
                                "pendingInput", "recoveryState", "supportDetail"})
            state[key] = value(app->selectedFile->property(key));
        if (auto *comparison = app->selectedFile->property("comparison").value<QObject *>()) {
            for (const auto *key : {"passages", "markerBlocks"}) state[key] = value(comparison->property(key));
            // Replay's scene observations retain their names; both read the joined value.
            const auto contributions = value(comparison->property("contributions")).toArray();
            state["regions"] = contributions;
            QJsonArray choices;
            for (const auto &entry : contributions) {
                auto choice = entry.toObject();
                choice["state"] = choice.value("decisionState");
                choices.append(choice);
            }
            state["choices"] = choices;
        }
    }
    return state;
}

QPoint centre(QQuickItem *item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); }

Outcome finish(const Request &, Progress &, QQuickItem *) { return Outcome::done(); }

Outcome click(const Request &, Progress &progress, QQuickItem *item)
{
    if (progress.acted) return Outcome::done();
    const auto point = centre(item);
    QTest::mouseMove(window, point);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point);
    progress.acted = true;
    progress.unsettle();
    return Outcome::pending();
}

Outcome escape(const Request &, Progress &progress, QQuickItem *)
{
    if (progress.acted) return Outcome::done();
    QTest::keyClick(window, Qt::Key_Escape);
    progress.acted = true;
    progress.unsettle();
    return Outcome::pending();
}

Outcome neutral(const Request &, Progress &progress, QQuickItem *)
{
    if (progress.acted) return Outcome::done();
    QTest::mouseMove(window, QPoint(window->width() - 5, window->height() - 40));
    progress.acted = true;
    progress.stable.restart();
    return Outcome::pending();
}

Outcome sourceMenu(const Request &request, Progress &progress, QQuickItem *item)
{
    if (progress.acted) return Outcome::done();
    auto *quick = item->property("textDocument").value<QQuickTextDocument *>();
    if (!quick || request.line > quick->textDocument()->blockCount())
        return Outcome::failed("Source menu line unavailable");
    if (!progress.sourcePositioned) {
        // Native cursor keys reveal the requested line; wait for layout and
        // cursor-driven scrolling before opening its menu on a later step.
        item->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Home, Qt::ControlModifier);
        for (int at = 1; at < request.line; ++at) QTest::keyClick(window, Qt::Key_Down);
        progress.sourcePositioned = true;
        progress.unsettle();
        return Outcome::pending();
    }
    const auto cursor = item->property("cursorPosition").toInt();
    if (quick->textDocument()->findBlock(cursor).blockNumber() + 1 != request.line)
        return Outcome::failed("Native source cursor did not reach requested logical line");
    if (request.gesture == "Menu")
        QTest::keyClick(window, Qt::Key_Menu);
    else if (request.gesture == "Shift+F10")
        QTest::keyClick(window, Qt::Key_F10, Qt::ShiftModifier);
    else {
        QRectF rect;
        if (!QMetaObject::invokeMethod(item, "positionToRectangle", Q_RETURN_ARG(QRectF, rect), Q_ARG(int, cursor)))
            return Outcome::failed("Source cursor rectangle unavailable");
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, item->mapToScene(rect.center()).toPoint());
    }
    progress.acted = true;
    progress.unsettle();
    return Outcome::pending();
}

Outcome captureReady(const Request &, Progress &progress, QQuickItem *)
{
    // Anonymous icon/colour transitions can keep painting after named geometry
    // settles. Observe real decoded frames before asking the compositor to shoot.
    const auto frame = window->grabWindow();
    if (frame.isNull()) return Outcome::failed("Native window frame unavailable");
    if (frame != progress.previousFrame) {
        progress.previousFrame = frame;
        progress.paintStable.restart();
        return Outcome::pending();
    }
    return progress.paintStable.elapsed() < 700 ? Outcome::pending() : Outcome::done();
}

Outcome quit(const Request &, Progress &, QQuickItem *)
{
    // Queued, so the reply is written before the event loop stops.
    QTimer::singleShot(0, QCoreApplication::instance(), &QCoreApplication::quit);
    return Outcome::done();
}

const QHash<QString, Handler> &handlers()
{
    static const QHash<QString, Handler> table{
        {"wait", {Lookup::ObjectName, {}, {"target", "text"}, finish}},
        {"click", {Lookup::ObjectName, {"target"}, {"text"}, click}},
        {"click-path", {Lookup::Path, {"target"}, {"text"}, click}},
        {"toolbar-click", {Lookup::Toolbar, {"target"}, {"text"}, click}},
        {"click-accessible", {Lookup::AccessibleName, {"target"}, {"text"}, click}},
        {"source-menu", {Lookup::ObjectName, {"target", "line"}, {"gesture"}, sourceMenu}},
        {"escape", {Lookup::None, {}, {}, escape}},
        {"neutral", {Lookup::None, {}, {}, neutral}},
        {"capture-ready", {Lookup::None, {}, {}, captureReady}},
        {"quit", {Lookup::None, {}, {}, quit}},
    };
    return table;
}

// Field readers leave out unchanged when the field is absent and return the reply error when it is malformed.
QString readString(const QJsonObject &object, const QString &key, QString &out)
{
    const auto field = object.value(key);
    if (field.isUndefined()) return {};
    if (!field.isString() || (key != "text" && field.toString().isEmpty()))
        return key + (key == "text" ? " must be a string" : " must be a non-empty string");
    out = field.toString();
    return {};
}

QString readPositive(const QJsonObject &object, const QString &key, int &out)
{
    const auto field = object.value(key);
    if (field.isUndefined()) return {};
    const double number = field.toDouble(0);
    if (!field.isDouble() || number < 1 || number > std::numeric_limits<int>::max() || number != std::floor(number))
        return key + " must be a positive integer";
    out = static_cast<int>(number);
    return {};
}

// Validates one request line against its action's fields. Returns the reply error, or empty when valid.
QString readRequest(const QByteArray &line, Request &request, const Handler *&handler)
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError) return "Invalid replay request: " + parseError.errorString();
    if (!document.isObject()) return QStringLiteral("Invalid replay request: expected a JSON object");
    const auto object = document.object();
    request.action = object.value("action").toString();
    const auto found = handlers().constFind(request.action);
    if (found == handlers().cend()) return "Unknown action: " + request.action;
    handler = &found.value();
    for (const auto &key : object.keys())
        if (key != "action" && key != "path" && key != "timeoutMs" && !handler->required.contains(key)
            && !handler->optional.contains(key))
            return QStringLiteral("%1 does not take %2").arg(request.action, key);
    for (const auto &key : handler->required)
        if (!object.contains(key)) return QStringLiteral("%1 needs %2").arg(request.action, key);
    QString text;
    for (const auto &error :
         {readString(object, "target", request.target), readString(object, "path", request.path),
          readString(object, "gesture", request.gesture), readString(object, "text", text),
          readPositive(object, "line", request.line), readPositive(object, "timeoutMs", request.timeoutMs)})
        if (!error.isEmpty()) return error;
    if (object.contains("text")) {
        if (request.target.isEmpty()) return QStringLiteral("text needs a target");
        request.text = text;
    }
    if (!QStringList{"right-click", "Menu", "Shift+F10"}.contains(request.gesture))
        return "Unknown source menu gesture: " + request.gesture;
    return {};
}

void reply(const QString &error = {})
{
    QJsonObject result{{"error", error}};
    if (window) result["state"] = snapshot();
    channel->write(QJsonDocument(result).toJson(QJsonDocument::Compact) + '\n');
    channel->flush();
    current.reset();
}

// Serves queued requests in order. An invalid one gets its error reply at once.
void startNext()
{
    while (!current && !queued.isEmpty()) {
        Request request;
        const Handler *handler = nullptr;
        const auto error = readRequest(queued.takeFirst(), request, handler);
        if (!error.isEmpty())
            reply(error);
        else
            current.emplace(Run{request, handler, {}});
    }
}

// Finds the request's target once it is reachable, enabled, painted and showing any requested text.
QQuickItem *resolve(const Request &request, Lookup lookup, Progress &progress)
{
    auto *root = window->contentItem();
    auto *item = lookup == Lookup::AccessibleName ? accessibleButton(root, request.target)
                                                  : find(root, request.target, lookup == Lookup::Path);
    if (!item && lookup == Lookup::Toolbar && !progress.overflowOpened) {
        auto *more = accessibleButton(find(root, "fileToolBar"), "More Actions");
        if (more && more->isEnabled()) {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centre(more));
            progress.overflowOpened = true;
            progress.unsettle();
        }
        return nullptr;
    }
    if (!item || !item->isEnabled()) {
        progress.stable.restart();
        return nullptr;
    }
    if (request.target.startsWith("choiceAccept-") || request.target.startsWith("choiceIgnore-")
        || request.target.startsWith("choiceMenu-")) {
        bool painted = true;
        for (auto *ancestor = item; ancestor; ancestor = ancestor->parentItem())
            painted = painted && ancestor->isVisible() && ancestor->opacity() > 0;
        if (!painted) {
            QTest::mouseMove(window, centre(item));
            progress.unsettle();
            return nullptr;
        }
    }
    if (request.text && item->property("text").toString() != *request.text) {
        progress.stable.restart();
        return nullptr;
    }
    return item;
}

// True once the named scene has been unchanged for 350 ms.
bool settled(Progress &progress)
{
    const auto scene = QJsonDocument(snapshot()).toJson(QJsonDocument::Compact);
    if (scene != progress.previous) {
        progress.previous = scene;
        progress.stable.restart();
        return false;
    }
    return progress.stable.elapsed() >= 350;
}

void tick()
{
    // A request that arrived while another was served starts once that one has replied.
    if (!current) startNext();
    if (!current) return;
    auto &[request, handler, progress] = *current;
    if (progress.deadline.elapsed() > request.timeoutMs) {
        reply("Readiness/layout deadline exceeded: " + request.action + " " + request.target);
        return;
    }
    if (!window) {
        for (auto *candidate : QGuiApplication::topLevelWindows()) {
            auto *quick = qobject_cast<QQuickWindow *>(candidate);
            if (quick && WindowBinding::bind(quick)) {
                window = quick;
                break;
            }
        }
    }
    const auto app = binding();
    if (!app || !window->isExposed() || !app->ready()) {
        progress.stable.restart();
        return;
    }
    if (!request.path.isEmpty() && app->selectedFile->property("path").toString() != request.path) {
        progress.stable.restart();
        return;
    }
    QQuickItem *item = nullptr;
    if (handler->lookup != Lookup::None && !request.target.isEmpty() && !progress.acted) {
        item = resolve(request, handler->lookup, progress);
        if (!item) return;
    }
    if (!settled(progress)) return;
    const auto outcome = handler->step(request, progress, item);
    if (outcome.kind == Outcome::Kind::Done)
        reply();
    else if (outcome.kind == Outcome::Kind::Failed)
        reply(outcome.error);
}

void begin()
{
    bool ok = false;
    const auto fd = qEnvironmentVariableIntValue("BRANCH_TRUCE_REPLAY_FD", &ok);
    if (!ok) return;
    channel = new QLocalSocket(QCoreApplication::instance());
    if (!channel->setSocketDescriptor(fd)) qFatal("Cannot open native replay channel");
    QObject::connect(channel, &QLocalSocket::readyRead, [] {
        buffer += channel->readAll();
        for (auto newline = buffer.indexOf('\n'); newline >= 0; newline = buffer.indexOf('\n')) {
            queued.append(buffer.left(newline));
            buffer.remove(0, newline + 1);
        }
        startNext();
    });
    auto *timer = new QTimer(channel);
    QObject::connect(timer, &QTimer::timeout, tick);
    timer->start(20);
}
void schedule() { QTimer::singleShot(0, begin); }
}
Q_COREAPP_STARTUP_FUNCTION(schedule)
