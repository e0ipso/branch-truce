// The sandbox, the scene and the typed readers every journey shares.
#include "acceptance/JourneyScene.h"

#include "fixtures/EventLoop.h"
#include "fixtures/QuickScene.h"
#include "mergetool/MergetoolSocket.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QUuid>
#include <QVariantMap>

#include <algorithm>
#include <stdexcept>

using namespace branch_truce;

namespace journey {
namespace {

// The value of `key`, which must be present: a missing key is a contract change, not a false.
QVariant required(const QVariantMap &map, const char *key, const char *what)
{
    const auto found = map.constFind(QLatin1String(key));
    if (found == map.constEnd())
        throw std::runtime_error(std::string(what) + " has no \"" + key
                                 + "\" field: " + QStringList(map.keys()).join(QLatin1Char(',')).toStdString());
    return *found;
}

PaneRange paneRange(const QVariantMap &map, const QString &prefix)
{
    const auto field = [&](const char *suffix) {
        const auto key = (prefix + QLatin1String(suffix)).toLatin1();
        return required(map, key.constData(), "region").toInt();
    };
    return {field("LineBegin"), field("LineEnd"), field("Utf16Begin"), field("Utf16End")};
}
} // namespace

QString fileId(const char *path) { return QString::fromLatin1(QByteArray(path).toHex()); }

void waitFor(const std::string &what, const std::function<bool()> &ready, const std::function<std::string()> &state)
{
    if (heldWithin(ready, test_support::kWaitDeadlineMs)) return;
    throw std::runtime_error("timed out waiting for " + what + (state ? ": " + state() : std::string()));
}

bool heldWithin(const std::function<bool()> &ready, int deadlineMs)
{
    // waitUntil fails with this detail only when the deadline passed; any other failure, such as
    // one an EXPECT recorded during the wait, is the journey's and is raised.
    static const std::string expired = "journey wait deadline";
    try {
        test_support::waitUntil(ready, expired, deadlineMs);
        return true;
    } catch (const test_support::Failure &failure) {
        if (failure.what() != expired) throw;
        return false;
    }
}

void untilFramesDrawn(QQuickWindow *window)
{
    QSignalSpy frames(window, &QQuickWindow::frameSwapped);
    window->update();
    test_support::settleFor("two rendered frames", [&] {
        if (frames.count() < 2) window->update();
        return frames.count() >= 2;
    });
}

Sandbox::Sandbox()
{
    CHECK(recovery_.isValid() && runtime_.isValid());
    CHECK(QFile::setPermissions(runtime_.path(),
                                QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
}

Application::Config Sandbox::config(const Repository &repository) const
{
    Application::Config config;
    config.git = {git_.executable(), git::GitEnvironment::from(repository.environment())};
    config.recoveryDirectory = recovery_.path() + QLatin1Char('/') + QUuid::createUuid().toString(QUuid::WithoutBraces);
    config.mergetoolDirectory = socketDirectory();
    return config;
}

QString Sandbox::socketDirectory() const { return mergetoolSocketDirectory(QFile::encodeName(runtime_.path())); }

QProcessEnvironment Sandbox::mergetoolEnvironment(const Repository &repository) const
{
    auto environment = repository.environment();
    environment.insert("PATH", git_.directory() + QStringLiteral(":/usr/bin:/bin"));
    environment.insert("XDG_RUNTIME_DIR", runtime_.path());
    return environment;
}

Scene::Scene(Sandbox &sandbox, const Repository &repository)
    : sandbox_(sandbox), gitCallsBefore_(sandbox.git().calls().size())
{
    controller_ = std::make_unique<Application>(sandbox.config(repository));
    engine_ = std::make_unique<QQmlApplicationEngine>();
    engine_->setInitialProperties(controller_->windowProperties());
    engine_->loadFromModule("BranchTruce", "Main");
    CHECK(engine_->rootObjects().size() == 1);
    window_ = qobject_cast<QQuickWindow *>(engine_->rootObjects().front());
    CHECK(window_);
    window_->resize(1600, 1200);
    window_->show();
    const auto before = repository.snapshot();
    auto *loader = controller_->loader();
    CHECK(loader->openRepository(repository.root()));
    // The load runs Git and the recovery restore off the GUI thread, so it gets the deadline of a
    // process, not the layout deadline of settleFor.
    waitFor(
        "the repository to open", [&] { return !loader->opening(); },
        [&] { return QVariant::fromValue(loader->startupState()).toString().toStdString(); });
    if (loader->startupState() != SessionLoader::StartupState::Ready)
        throw std::runtime_error("startup " + QVariant::fromValue(loader->startupState()).toString().toStdString()
                                 + ": " + loader->detail().toStdString());
    // Opening a session inspects Git and never mutates the repository.
    CHECK(repository.snapshot() == before);
    // The session listens in the journey's private runtime directory, not a fallback.
    const auto socket = sandbox.socketDirectory();
    if (socket != sandbox.runtimeDirectory() + QStringLiteral("/branch-truce"))
        throw std::runtime_error("socket directory outside the private runtime directory: " + socket.toStdString());
    CHECK(controller_->mergetoolSession()->listening() && QFileInfo(socket).isDir());
}

Scene::~Scene()
{
    engine_.reset();
    controller_.reset();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

QQuickItem *Scene::item(const char *name) const
{
    const auto objectName = QString::fromLatin1(name);
    auto *found = test_support::visibleItem(window_->contentItem(), objectName);
    if (!found) found = window_->findChild<QQuickItem *>(objectName);
    // A toolbar creates its buttons outside the QObject tree, so look in the item tree too.
    if (!found) found = test_support::visualItem(window_->contentItem(), objectName);
    if (!found) throw std::runtime_error(std::string("missing scene item: ") + name);
    return found;
}

QObject *Scene::object(const char *name) const
{
    auto *found = window_->findChild<QObject *>(QString::fromLatin1(name));
    if (!found) throw std::runtime_error(std::string("missing scene object: ") + name);
    return found;
}

QString Scene::text(const char *name) const
{
    const auto value = item(name)->property("text");
    return value.isValid() ? value.toString() : test_support::shownText(item(name));
}

void Scene::click(const char *name)
{
    auto *target = test_support::fileOverflowControl(name)
        ? test_support::fileToolbarControl(window_, name, settleMs)
        : test_support::waitForLayout(window_, QString::fromLatin1(name), settleMs);
    if (reveal(target)) target = test_support::waitForLayout(window_, QString::fromLatin1(name), settleMs);
    test_support::clickItem(window_, target);
}

bool Scene::reveal(QQuickItem *target)
{
    bool moved = false;
    for (auto *view = target->parentItem(); view; view = view->parentItem()) {
        if (!view->inherits("QQuickFlickable")) continue;
        auto *content = view->property("contentItem").value<QQuickItem *>();
        const auto top = target->mapToItem(content, QPointF(0, 0)).y();
        const auto offset = view->property("contentY").toReal();
        if (top >= offset && top + target->height() <= offset + view->height()) continue;
        view->setProperty("contentY", std::max<qreal>(0, top + target->height() - view->height()));
        moved = true;
    }
    return moved;
}

const FileSession &Scene::file(const char *path) const
{
    const auto *found = controller_->store()->file(fileId(path).toStdString());
    if (!found) throw std::runtime_error(std::string("missing session file: ") + path);
    return *found;
}

Progress Scene::progress() const
{
    const auto map = controller_->selectedFile()->progress();
    const auto field = [&](const char *key) { return required(map, key, "progress"); };
    return {field("undecided").toULongLong(),  field("conflicts").toULongLong(),
            field("saved").toBool(),           field("reviewed").toBool(),
            field("resolved").toBool(),        field("earlierResolved").toBool(),
            field("settled").toBool(),         field("currentRevision").toString(),
            field("savedRevision").toString(), field("reviewedRevision").toString(),
            field("stagedRevision").toString()};
}

std::vector<Region> Scene::regions() const
{
    std::vector<Region> regions;
    for (const auto &value : controller_->selectedFile()->regions()) {
        const auto map = value.toMap();
        const auto field = [&](const char *key) { return required(map, key, "region"); };
        regions.push_back({field("id").toString(), field("side").toString(), field("scope").toString(),
                           field("section").toString(), field("state").toString(), field("decisionIds").toStringList(),
                           field("handEdited").toBool(), field("markersRemain").toBool(), field("mapped").toBool(),
                           paneRange(map, QStringLiteral("source")), paneRange(map, QStringLiteral("result"))});
    }
    return regions;
}

std::vector<ChoiceRow> Scene::choiceRows() const
{
    std::vector<ChoiceRow> rows;
    for (const auto &value : controller_->selectedFile()->choices()) {
        const auto map = value.toMap();
        const auto field = [&](const char *key) { return required(map, key, "choice row"); };
        rows.push_back({field("id").toString(), field("label").toString(), field("state").toString(),
                        field("detail").toString(), field("available").toBool()});
    }
    return rows;
}

ChoiceRow Scene::choiceRow(const QString &label) const
{
    for (const auto &row : choiceRows())
        if (row.label == label) return row;
    throw std::runtime_error("missing choice row " + label.toStdString());
}

Region Scene::regionFor(const QString &label) const
{
    const auto id = choiceRow(label).id;
    for (const auto &region : regions())
        if (region.id == id) return region;
    throw std::runtime_error("missing region for " + label.toStdString());
}

std::vector<MarkerBlock> Scene::markerBlocks() const
{
    std::vector<MarkerBlock> blocks;
    for (const auto &value : controller_->selectedFile()->markerBlocks()) {
        const auto map = value.toMap();
        const auto field = [&](const char *key) { return required(map, key, "marker block"); };
        blocks.push_back({field("resultLineBegin").toInt(), field("resultLineEnd").toInt(), field("folded").toBool()});
    }
    return blocks;
}

QMap<QString, HandoffEntry> Scene::handoffEntries() const
{
    QMap<QString, HandoffEntry> entries;
    const auto data = controller_->completion()->handoffData();
    for (const auto &group : required(data, "groups", "handoff").toList()) {
        for (const auto &value : required(group.toMap(), "entries", "handoff group").toList()) {
            const auto map = value.toMap();
            const auto field = [&](const char *key) { return required(map, key, "handoff entry"); };
            entries.insert(field("fileId").toString(),
                           {field("category").toString(), field("actionLabel").toString(),
                            field("outstanding").toBool(), field("blockers").toStringList()});
        }
    }
    return entries;
}

bool Scene::handoffClean() const
{
    return required(controller_->completion()->handoffData(), "clean", "handoff").toBool();
}

QByteArray Scene::gitCalls() const { return sandbox_.git().calls().mid(gitCallsBefore_); }

} // namespace journey
