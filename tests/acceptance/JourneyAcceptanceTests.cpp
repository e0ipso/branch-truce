// Qt acceptance checks for the PRD's observable acceptance examples.
// Each journey opens a real Git operation in a disposable repository through
// ApplicationController::openRepository, loads the real BranchTruce Main scene
// offscreen and drives it with mouse clicks and key events. Coverage notes and
// the entry point are in tests/acceptance/README.md.
#include "app/ApplicationController.h"
#include "fixtures/GitOnPath.h"
#include "mergetool/MergetoolLauncher.h"
#include "mergetool/MergetoolProtocol.h"
#include "TestHarness.h"
#include "fixtures/QuickScene.h"
#include <QAccessible>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QLockFile>
#include <QKeySequence>
#include <QMap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

using namespace branch_truce;
using test_support::settleFor;

namespace {
// Shared by every journey: private HOME, XDG and runtime directories, plus an
// offline guard in front of /usr/bin/git. Created by main() before QApplication.
struct Sandbox {
    QTemporaryDir root;
    // The private XDG_RUNTIME_DIR. Under a long TMPDIR a QTemporaryDir would push
    // the socket path past its length limit, and the session would fall back to
    // the shared /tmp/branch-truce-<uid>. A short base under /tmp keeps it private.
    QTemporaryDir runtime{QStringLiteral("/tmp/bt-run-XXXXXX")};
    QString bin;
    QString argvLog;
    QString networkMarker;
};
Sandbox *sandbox = nullptr;

void writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size());
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly));
    const auto bytes = file.readAll();
    CHECK(file.error() == QFileDevice::NoError);
    return bytes;
}

QByteArray quoteShell(QByteArray value)
{
    value.replace("'", "'\\''");
    return '\'' + value + '\'';
}

QString fileId(const char *path)
{
    return QString::fromLatin1(QByteArray(path).toHex());
}

// One path in the disposable operation. A missing side deletes the path on that branch.
struct PathSpec {
    QString path;
    std::optional<QByteArray> base;
    std::optional<QByteArray> left;
    std::optional<QByteArray> right;
    // Written over Git's conflict output after the operation stops, as a user would before launch.
    std::optional<QByteArray> manual;
};

class Repository {
public:
    Repository(const QString &operation, const std::vector<PathSpec> &paths)
    {
        CHECK(directory.isValid());
        root = QFileInfo(directory.path()).canonicalFilePath();
        // The fixture uses /usr/bin/git directly so the guard log records only app commands.
        environment = QProcessEnvironment::systemEnvironment();
        git({"init", "--quiet", "--initial-branch=main"});
        git({"config", "user.name", "Branch Truce Acceptance"});
        git({"config", "user.email", "acceptance@example.invalid"});
        git({"config", "core.autocrlf", "false"});
        // A remote that cannot be reached: startup and use must not contact it.
        git({"config", "remote.origin.url", "https://offline.invalid/never-contact"});
        git({"config", "branch.main.remote", "origin"});
        writeFile(path("README"), "acceptance fixture\n");
        QStringList tracked{"README"};
        for (const auto &spec : paths) {
            if (!spec.base) continue;
            QDir().mkpath(QFileInfo(path(spec.path)).path());
            writeFile(path(spec.path), *spec.base);
            tracked << spec.path;
        }
        git(QStringList{"add", "--"} + tracked);
        git({"commit", "--quiet", "-m", "base"});
        commitSide("side", paths, false);
        git({"checkout", "--quiet", "main"});
        commitSide("main", paths, true);
        if (operation == "rebase") {
            git({"checkout", "--quiet", "side"});
            git({"rebase", "--merge", "main"}, 1);
        } else {
            git({"merge", "--no-edit", "side"}, 1);
        }
        for (const auto &spec : paths)
            if (spec.manual) writeFile(path(spec.path), *spec.manual);
    }
    QString path(const QString &relative) const { return root + '/' + relative; }
    QByteArray git(const QStringList &arguments, int expected = 0) const
    {
        QProcess process;
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(root);
        process.start("/usr/bin/git", QStringList{"--no-pager", "--literal-pathspecs"} + arguments);
        CHECK(process.waitForStarted(5000) && process.waitForFinished(15000));
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != expected)
            throw std::runtime_error("fixture git " + arguments.join(' ').toStdString() + ": "
                                     + process.readAllStandardError().toStdString());
        return process.readAllStandardOutput();
    }
    QByteArray stageObject(int stage, const QString &relative) const
    {
        return git({"rev-parse", QStringLiteral(":%1:%2").arg(stage).arg(relative)}).trimmed();
    }
    QByteArray stageText(int stage, const QString &relative) const
    {
        return git({"cat-file", "blob", QString::fromLatin1(stageObject(stage, relative))});
    }
    // Unmerged index entries for one path; empty once Git records a resolution.
    QByteArray unmerged(const QString &relative) const { return git({"ls-files", "-u", "--", relative}); }
    QMap<QString, QByteArray> snapshot() const
    {
        QMap<QString, QByteArray> result;
        // Includes the index, operation metadata, refs, objects and untracked files.
        QDirIterator files(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const auto selected = files.next();
            result.insert(QDir(root).relativeFilePath(selected), readFile(selected));
        }
        return result;
    }
    QTemporaryDir directory;
    QString root;
    QProcessEnvironment environment;
    QByteArray leftCommit;
    QByteArray rightCommit;

private:
    void commitSide(const char *branch, const std::vector<PathSpec> &paths, bool left)
    {
        if (!left) git({"checkout", "--quiet", "-b", branch});
        QStringList changed;
        QStringList removed;
        for (const auto &spec : paths) {
            const auto &content = left ? spec.left : spec.right;
            if (content) {
                QDir().mkpath(QFileInfo(path(spec.path)).path());
                writeFile(path(spec.path), *content);
                changed << spec.path;
            } else if (spec.base) {
                removed << spec.path;
            }
        }
        if (!changed.isEmpty()) git(QStringList{"add", "--"} + changed);
        if (!removed.isEmpty()) git(QStringList{"rm", "--quiet", "--"} + removed);
        git({"commit", "--quiet", "-m", branch});
        (left ? leftCommit : rightCommit) = git({"rev-parse", "HEAD"}).trimmed();
    }
};

// The real Main scene over a controller that opened the repository itself.
class Scene {
public:
    explicit Scene(const Repository &repository) : repo(repository)
    {
        controller = std::make_unique<ApplicationController>(gitOnPath());
        engine = std::make_unique<QQmlApplicationEngine>();
        engine->setInitialProperties({{"controller", QVariant::fromValue(controller.get())}});
        engine->loadFromModule("BranchTruce", "Main");
        CHECK(engine->rootObjects().size() == 1);
        window = qobject_cast<QQuickWindow *>(engine->rootObjects().front());
        CHECK(window);
        window->resize(1600, 1200);
        window->show();
        const auto before = repo.snapshot();
        CHECK(controller->loader()->openRepository(repo.root));
        QElapsedTimer deadline;
        deadline.start();
        while (controller->loader()->opening() && deadline.elapsed() < 20000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (controller->loader()->startupState() != QStringLiteral("ready"))
            throw std::runtime_error("startup " + controller->loader()->startupState().toStdString() + ": "
                                     + controller->loader()->detail().toStdString());
        // Opening a session inspects Git and never mutates the repository.
        CHECK(repo.snapshot() == before);
        // The session listens in the sandbox's private runtime directory, not a fallback.
        const auto socket = mergetoolSocketDirectory();
        if (socket != sandbox->runtime.path() + QStringLiteral("/branch-truce"))
            throw std::runtime_error("socket directory outside the private runtime directory: " + socket.toStdString());
        CHECK(controller->mergetool()->listening() && QFileInfo(socket).isDir());
    }
    ~Scene()
    {
        engine.reset();
        controller.reset();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    QQuickItem *item(const char *name) const
    {
        auto *found = test_support::visibleItem(window->contentItem(), QString::fromLatin1(name));
        if (!found) found = window->findChild<QQuickItem *>(QString::fromLatin1(name));
        // A toolbar creates its buttons outside the QObject tree, so look in the item tree too.
        if (!found) found = test_support::visualItem(window->contentItem(), QString::fromLatin1(name));
        if (!found) throw std::runtime_error(std::string("missing scene item: ") + name);
        return found;
    }
    QObject *object(const char *name) const
    {
        auto *found = window->findChild<QObject *>(QString::fromLatin1(name));
        if (!found) throw std::runtime_error(std::string("missing scene object: ") + name);
        return found;
    }
    // A label's text, or for a row of labels such as the status chips, their visible texts.
    QString text(const char *name) const
    {
        const auto value = item(name)->property("text");
        return value.isValid() ? value.toString() : test_support::shownText(item(name));
    }
    void click(const char *name)
    {
        auto *target = test_support::fileOverflowControl(name)
            ? test_support::fileToolbarControl(window, name, settleMs)
            : test_support::waitForLayout(window, QString::fromLatin1(name), settleMs);
        if (reveal(target)) target = test_support::waitForLayout(window, QString::fromLatin1(name), settleMs);
        test_support::clickItem(window, target);
    }
    // Scroll enclosing views, as a user would, so a clipped target is under the pointer.
    static bool reveal(QQuickItem *target)
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
    const FileSession &file(const char *path) const
    {
        const auto *found = controller->files()->file(fileId(path).toStdString());
        if (!found) throw std::runtime_error(std::string("missing session file: ") + path);
        return *found;
    }
    // Positions must hold this long before a click; see test_support::waitForStable.
    static constexpr int settleMs = 60;
    QVariantMap progress() const { return controller->selectedFile()->progress(); }

    const Repository &repo;
    std::unique_ptr<ApplicationController> controller;
    std::unique_ptr<QQmlApplicationEngine> engine;
    QQuickWindow *window = nullptr;
};

const QByteArray storyBase = "header\nbase first\nbridge one\nbridge two\nbridge three\nbase later\ntail\n";
const QByteArray storyLeft = "header\nleft first\nbridge one\nbridge two\nbridge three\nleft later\ntail\n";
const QByteArray storyRight = "header\nright first\nbridge one\nbridge two\nbridge three\nright later\ntail\n";

QQuickItem *stable(Scene &scene, const std::string &what, const std::function<QQuickItem *()> &find)
{
    return test_support::waitForStable(scene.window, what, find, Scene::settleMs);
}

QQuickItem *descendant(QQuickItem *root, const std::function<bool(QQuickItem *)> &match)
{
    if (!root) return nullptr;
    if (match(root)) return root;
    for (auto *child : root->childItems())
        if (auto *found = descendant(child, match)) return found;
    return nullptr;
}

QVariantList choiceRows(Scene &scene)
{
    return scene.controller->selectedFile()->choices();
}

QVariantMap choiceRow(Scene &scene, const QString &label)
{
    for (const auto &value : choiceRows(scene))
        if (value.toMap().value(QStringLiteral("label")).toString() == label) return value.toMap();
    throw std::runtime_error("missing choice row " + label.toStdString());
}

// The region that shares its ID with one scoped-choice row.
QVariantMap regionFor(Scene &scene, const QString &label)
{
    const auto id = choiceRow(scene, label).value(QStringLiteral("id"));
    for (const auto &value : scene.controller->selectedFile()->regions())
        if (value.toMap().value(QStringLiteral("id")) == id) return value.toMap();
    throw std::runtime_error("missing region for " + label.toStdString());
}

// The displayed result text inside a region's UTF-16 result range.
QString regionResult(Scene &scene, const QVariantMap &region)
{
    const auto begin = region.value(QStringLiteral("resultUtf16Begin")).toInt();
    const auto end = region.value(QStringLiteral("resultUtf16End")).toInt();
    if (begin < 0 || end < begin) throw std::runtime_error("region result range is unmapped");
    return scene.text("resultText").mid(begin, end - begin);
}

// The scope whose gutter row offers a choice: the section that starts on the choice's line
// on its side, or else the choice itself.
QString rowOwner(Scene &scene, const QVariantMap &region)
{
    const auto id = region.value(QStringLiteral("id")).toString();
    if (region.value(QStringLiteral("scope")).toString() == QStringLiteral("section")) return id;
    for (const auto &value : scene.controller->selectedFile()->regions()) {
        const auto other = value.toMap();
        if (other.value(QStringLiteral("scope")).toString() == QStringLiteral("section")
            && other.value(QStringLiteral("side")) == region.value(QStringLiteral("side"))
            && other.value(QStringLiteral("sourceLineBegin")) == region.value(QStringLiteral("sourceLineBegin")))
            return other.value(QStringLiteral("id")).toString();
    }
    return id;
}

// Click one scoped choice in the source gutters. kind is the part of the control's objectName
// before the choice ID: Accept, Ignore, Resolve, PreviewLeftFirst or PreviewRightFirst. Accept
// and Ignore of the scope that owns a gutter row are buttons on that row; every other choice
// is an entry of the row's menu, choiceMore-<owner ID>.
void clickChoice(Scene &scene, const QString &label, const char *kind)
{
    auto *lanes = scene.item("scopedChoices");
    const auto region = regionFor(scene, label);
    const auto id = region.value(QStringLiteral("id")).toString();
    const auto owner = rowOwner(scene, region);
    const auto name = QStringLiteral("choice") + QLatin1String(kind) + QLatin1Char('-') + id;
    const bool onRow = owner == id && (qstrcmp(kind, "Accept") == 0 || qstrcmp(kind, "Ignore") == 0);
    if (onRow) {
        auto *target = stable(scene, (label + " " + name).toStdString(),
                              [&] { return test_support::namedButton(lanes, name); });
        test_support::clickItem(scene.window, target);
        return;
    }
    const auto more = QStringLiteral("choiceMore-") + owner;
    auto *menuButton = stable(scene, (label + " " + more).toStdString(),
                              [&] { return test_support::namedButton(lanes, more); });
    test_support::clickItem(scene.window, menuButton);
    auto *entry = stable(scene, (label + " " + name).toStdString(),
                         [&] { return test_support::namedButton(scene.window->contentItem(), name); });
    test_support::clickItem(scene.window, entry);
}

// A dialog button by objectName, or a standard button such as OK, Cancel or Apply by its
// accessible name, once the dialog is open. The whole dialog is searched, so a Kirigami.Dialog
// footer works too.
QQuickItem *dialogButton(Scene &scene, const char *dialog, const QString &button)
{
    auto *popup = scene.object(dialog);
    settleFor(std::string(dialog) + " opened", [&] { return popup->property("opened").toBool(); });
    auto *root = test_support::popupRoot(popup);
    return stable(scene, std::string(dialog) + " " + button.toStdString(),
                  [&] { return test_support::namedButton(root, button); });
}

// Click a dialog button and wait for the dialog to close.
void clickDialogButton(Scene &scene, const char *dialog, const QString &button)
{
    test_support::clickItem(scene.window, dialogButton(scene, dialog, button));
    auto *popup = scene.object(dialog);
    settleFor(std::string(dialog) + " closed", [&] { return !popup->property("visible").toBool(); });
}

// Select a file the way a user does: click its row in the file list.
void clickFile(Scene &scene, const char *path)
{
    const auto id = fileId(path);
    auto *list = scene.item("fileList");
    int row = -1;
    for (int i = 0; i < scene.controller->files()->rowCount(); ++i)
        if (scene.controller->files()->data(scene.controller->files()->index(i), SessionModel::FileIdRole).toString() == id) row = i;
    CHECK(row >= 0);
    constexpr int contain = 4; // ListView.Contain
    CHECK(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, row), Q_ARG(int, contain)));
    auto *target = stable(scene, std::string("file row ") + path, [&] {
        return descendant(list, [&](QQuickItem *candidate) {
            return candidate->inherits("QQuickAbstractButton") && candidate->property("fileId").toString() == id;
        });
    });
    test_support::clickItem(scene.window, target);
    settleFor(std::string("selection of ") + path, [&] { return scene.controller->selectedFileId() == id; });
}

void keyClick(Scene &scene, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QTest::keyClick(scene.window, key, modifiers);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

// QTest::keyClicks has no QWindow overload; send each Latin-1 character as a key click.
void typeText(Scene &scene, const char *text)
{
    for (const char *c = text; *c; ++c) QTest::keyClick(scene.window, *c);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

// Text and decisions together, as shared undo/redo must restore them.
struct Work {
    QString shown;
    Draft draft;
    bool operator==(const Work &) const = default;
};

Work work(Scene &scene, const char *path)
{
    return {scene.text("resultText"), scene.file(path).current().draft};
}

// Waits until the result folds each conflict-marker block into one row, its opening marker
// line, with every later line of the block hidden, or shows every line. The folded rows layer
// draws a row only while the blocks are folded. The line numbers are the buffer's, so the
// line after a folded block keeps its own number.
void expectMarkersFolded(Scene &scene, bool folded)
{
    const auto *document = scene.item("resultText")->property("textDocument").value<QQuickTextDocument *>()->textDocument();
    const auto look = [&] {
        int blocks = 0;
        bool inside = false;
        QList<int> expected;
        for (auto block = document->begin(); block.isValid(); block = block.next()) {
            const auto line = block.text();
            const bool hidden = folded && inside;
            if (line.startsWith(QStringLiteral("<<<<<<< "))) {
                inside = true;
                ++blocks;
            } else if (line.startsWith(QStringLiteral(">>>>>>> "))) {
                inside = false;
            }
            if (block.isVisible() == hidden) return false;
            if (!hidden) expected.push_back(block.blockNumber() + 1);
        }
        QList<int> numbered;
        int rows = 0;
        for (auto *child : scene.item("resultGutter")->childItems()) {
            bool number = false;
            const auto value = child->property("text").toString().toInt(&number);
            if (number && child->isVisible()) numbered.push_back(value);
        }
        for (auto *child : scene.item("resultFoldedRowsRows")->childItems())
            rows += child->isVisible() && child->property("line").isValid();
        std::sort(numbered.begin(), numbered.end());
        // The gutter numbers the lines near the view, which here is the whole file.
        return blocks > 0 && numbered == expected && rows == (folded ? blocks : 0);
    };
    settleFor(folded ? "folded conflict markers" : "shown conflict markers", look);
}

// Opens the file toolbar's overflow menu and clicks the entry of the action with this objectName.
void clickOverflowAction(Scene &scene, const char *name)
{
    auto *toolbar = scene.item("fileToolBar");
    test_support::clickItem(scene.window, test_support::waitForStable(scene.window, "file toolbar overflow", [&] {
        return test_support::visualButton(toolbar, [](QQuickItem *button) {
            return test_support::accessibleName(button) == QStringLiteral("More Actions");
        });
    }, Scene::settleMs));
    auto *action = scene.object(name);
    test_support::clickItem(scene.window, test_support::waitForStable(scene.window, std::string(name) + " entry", [&] {
        return test_support::visualButton(scene.window->contentItem(), [&](QQuickItem *entry) {
            return entry->property("action").value<QObject *>() == action;
        });
    }, Scene::settleMs));
}

// A named toolbar control may move to overflow as the window narrows.
void clickFileToolbarControl(Scene &scene, const char *name)
{
    const auto find = [&] { return test_support::namedButton(scene.window->contentItem(), QString::fromLatin1(name)); };
    auto *control = find();
    if (!control) {
        auto *toolbar = scene.item("fileToolBar");
        auto *more = test_support::waitForStable(scene.window, "file toolbar overflow", [&] {
            return test_support::namedButton(toolbar, QStringLiteral("More Actions"));
        }, Scene::settleMs);
        test_support::clickItem(scene.window, more);
        control = test_support::waitForStable(scene.window, name, find, Scene::settleMs);
    }
    test_support::clickItem(scene.window, control);
}

// Opens the file toolbar's overflow menu and clicks its Show conflict markers entry.
void toggleConflictMarkers(Scene &scene)
{
    clickOverflowAction(scene, "showConflictMarkers");
    // A click on the overflow button while the menu is still closing closes it again.
    settleFor("overflow menu closed", [&] {
        return !descendant(scene.window->contentItem(), [](QQuickItem *item) {
            return item->objectName() == QStringLiteral("ActionsMenu") && item->isVisible();
        });
    });
}

QPointF sceneOrigin(QQuickItem *item)
{
    return item->mapToScene(QPointF(0, 0));
}

void expectText(const QString &actual, const QString &expected, const char *what)
{
    if (actual != expected)
        throw std::runtime_error(std::string(what) + ": expected [" + expected.toStdString() + "] got ["
                                 + actual.toStdString() + "]");
    CHECK(actual == expected);
}

void expectContains(const QString &actual, const QString &expected, const char *what)
{
    if (!actual.contains(expected))
        throw std::runtime_error(std::string(what) + ": [" + actual.toStdString() + "] lacks ["
                                 + expected.toStdString() + "]");
    CHECK(actual.contains(expected));
}

QString utf8(const QByteArray &bytes)
{
    return QString::fromUtf8(bytes);
}

// Every visible text in the scene, including open dialogs.
void collectTexts(QQuickItem *root, QStringList &texts)
{
    if (!root->isVisible()) return;
    const auto value = root->property("text");
    if (value.isValid() && !value.toString().isEmpty()) texts << value.toString();
    for (auto *child : root->childItems()) collectTexts(child, texts);
}

// Offline and account-free: no Git network command ran and nothing asks for an account.
void checkOffline(Scene &scene)
{
    CHECK(!QFile::exists(sandbox->networkMarker));
    const auto trace = readFile(sandbox->argvLog);
    CHECK(trace.contains("rev-parse\n") && trace.contains("cat-file\n"));
    QStringList texts;
    collectTexts(scene.window->contentItem(), texts);
    CHECK(!texts.isEmpty());
    for (const auto &text : texts) {
        for (const auto *word : {"sign in", "sign-in", "log in", "login", "account", "subscription", "payment",
                                 "license key", "register"}) {
            if (text.contains(QLatin1String(word), Qt::CaseInsensitive))
                throw std::runtime_error("account prompt in scene text: " + text.toStdString());
        }
    }
}

QString rowText(Scene &scene, const char *path)
{
    const auto id = fileId(path);
    auto *row = stable(scene, std::string("file row ") + path, [&] {
        return descendant(scene.item("fileList"), [&](QQuickItem *candidate) {
            return candidate->inherits("QQuickAbstractButton") && candidate->property("fileId").toString() == id;
        });
    });
    QStringList texts;
    collectTexts(row, texts);
    // The summary is the row's accessible description and tooltip, not a label.
    if (const auto *face = QAccessible::queryAccessibleInterface(row))
        texts << face->text(QAccessible::Description);
    return texts.join('\n');
}

// Click near the top-left of a text area: unwrapped text can extend past its viewport.
void clickText(Scene &scene, QQuickItem *editor)
{
    CHECK(editor->isVisible() && editor->width() > 20 && editor->height() > 8);
    const auto point = editor->mapToScene(QPointF(20, 8)).toPoint();
    QTest::mouseClick(scene.window, Qt::LeftButton, Qt::NoModifier, point);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

void appendToResult(Scene &scene, const char *text)
{
    auto *editor = scene.item("resultText");
    clickText(scene, editor);
    CHECK(editor->hasActiveFocus());
    keyClick(scene, Qt::Key_End, Qt::ControlModifier);
    typeText(scene, text);
}

// Mark resolved through its confirmation, then wait for Git to resolve the file.
void markResolvedByMouse(Scene &scene)
{
    scene.click("markResolved");
    settleFor("resolution confirmation", [&] { return scene.controller->resolutionConfirmation(); });
    scene.click("confirmResolution");
    const auto finished = [&] { return !scene.controller->resolutionBusy() && !scene.controller->resolutionConfirmation(); };
    QElapsedTimer deadline;
    deadline.start();
    while (!finished() && deadline.elapsed() < 10000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    if (!finished())
        throw std::runtime_error("resolution of " + scene.text("selectedPath").toStdString() + " busy="
                                 + std::to_string(scene.controller->resolutionBusy()) + " confirmation="
                                 + std::to_string(scene.controller->resolutionConfirmation()) + " status="
                                 + scene.text("selectedStatus").toStdString() + " message="
                                 + scene.controller->commandMessage().toStdString());
}

// Waits longer than settleFor: a launcher process discovers the repository before it attaches.
void waitFor(const std::string &what, const std::function<bool()> &ready, const std::function<std::string()> &state)
{
    QElapsedTimer deadline;
    deadline.start();
    while (!ready() && deadline.elapsed() < 25000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
    if (!ready()) throw std::runtime_error("timed out waiting for " + what + ": " + state());
}

// A loader's startup state, its recovery choices and its detail, for the failure text of a
// wait on a repository load.
std::string loaderState(const SessionLoader &loader)
{
    return "startup " + loader.startupState().toStdString() + ", "
        + std::to_string(loader.recoveryChoices().size()) + " recovery choices: " + loader.detail().toStdString();
}

// Real `git mergetool` over the given paths. This test binary is the launcher: main()
// runs the real launcher code when Git starts it with --mergetool.
std::unique_ptr<QProcess> startMergetool(const Repository &repo, const QStringList &paths)
{
    const auto binary = QFile::encodeName(QCoreApplication::applicationFilePath());
    repo.git({"config", "mergetool.truce-launcher.cmd",
              QString::fromLocal8Bit("MERGED=\"$MERGED\" BASE=\"$BASE\" LOCAL=\"$LOCAL\" REMOTE=\"$REMOTE\" "
                                     + quoteShell(binary) + " --mergetool")});
    repo.git({"config", "mergetool.truce-launcher.trustExitCode", "true"});
    repo.git({"config", "mergetool.keepBackup", "false"});
    auto environment = repo.environment;
    // The session in this process must already listen; a launcher never starts another GUI.
    environment.insert("BRANCH_TRUCE_MERGETOOL_NO_SPAWN", "1");
    auto process = std::make_unique<QProcess>();
    process->setProcessEnvironment(environment);
    process->setWorkingDirectory(repo.root);
    process->start("/usr/bin/git", QStringList{"--no-pager", "--literal-pathspecs", "mergetool", "--no-prompt",
                                               "--tool=truce-launcher", "--"} + paths);
    CHECK(process->waitForStarted(5000));
    return process;
}

// Every named control of the main view, in reading order.
const char *const mainControls[] = {
    "openHandoff", "previousUndecided", "previousUnfinished", "nextUnfinished",
    "undecidedCounter", "nextUndecided", "navigateMenu", "toggleFileList", "fileList", "selectedPath", "saveDraft",
    "markResolved", "discardUnsaved", "undoCommand", "redoCommand", "copyResult", "pasteResult",
    "sourceChoicesMenu", "showSourceChoices",
    "openPassageSearch", "wrapLines", "showWhitespace", "stage2Label", "resultLabel",
    "stage3Label", "showEarlier", "workflowStatus",
};
const char *const textPanes[] = {"stage2Viewport", "resultViewport", "stage3Viewport"};

std::string geometry(QQuickItem *item)
{
    const auto origin = item->mapToScene(QPointF(0, 0));
    return item->objectName().toStdString() + "@" + std::to_string(int(origin.x())) + ","
        + std::to_string(int(origin.y())) + " " + std::to_string(int(item->width())) + "x"
        + std::to_string(int(item->height())) + (item->isVisible() ? "" : " hidden");
}

std::vector<const char *> everyControl()
{
    std::vector<const char *> names(std::begin(mainControls), std::end(mainControls));
    names.insert(names.end(), std::begin(textPanes), std::end(textPanes));
    return names;
}

// Resize to a logical screen size and require the named items inside the window, scrolling
// an enclosing view first as a user would. Returns the smallest text pane height.
qreal checkFits(Scene &scene, int width, int height, const std::vector<const char *> &names = everyControl())
{
    auto *window = scene.window;
    if (window->minimumWidth() > width || window->minimumHeight() > height)
        throw std::runtime_error("minimum size " + std::to_string(window->minimumWidth()) + "x"
                                 + std::to_string(window->minimumHeight()) + " exceeds the screen "
                                 + std::to_string(width) + "x" + std::to_string(height));
    window->resize(width, height);
    settleFor("window size", [&] { return window->width() == width && window->height() == height; });
    const QRectF screen(0, 0, width, height);
    const auto inside = [&](QQuickItem *item) {
        const auto rect = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        return item->isVisible() && item->width() > 0 && item->height() > 0
            && screen.adjusted(-0.5, -0.5, 0.5, 0.5).contains(rect);
    };
    for (const auto *name : names) {
        bool openedOverflow = test_support::fileOverflowControl(name);
        auto *item = openedOverflow ? test_support::fileToolbarControl(scene.window, name, Scene::settleMs) : scene.item(name);
        if (!item->isVisible()) {
            auto *toolbar = scene.item("fileToolBar");
            if (auto *more = test_support::namedButton(toolbar, QStringLiteral("More Actions"))) {
                test_support::clickItem(scene.window, more);
                item = test_support::waitForStable(scene.window, name, [&] {
                    return test_support::namedButton(scene.window->contentItem(), QString::fromLatin1(name));
                }, Scene::settleMs);
                openedOverflow = true;
            }
        }
        // Scroll again while waiting: just after a resize the page can still lay out at the
        // previous size, so the first scroll position can be stale.
        QElapsedTimer deadline;
        deadline.start();
        do {
            Scene::reveal(item);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        } while (!inside(item) && deadline.elapsed() < 2000);
        // The toolbar can move an action into overflow during the resize after the
        // first lookup, so try the menu again once the layout has settled.
        if (!inside(item) && !openedOverflow) {
            auto *toolbar = scene.item("fileToolBar");
            if (auto *more = test_support::namedButton(toolbar, QStringLiteral("More Actions"))) {
                test_support::clickItem(scene.window, more);
                item = test_support::waitForStable(scene.window, name, [&] {
                    return test_support::namedButton(scene.window->contentItem(), QString::fromLatin1(name));
                }, Scene::settleMs);
                openedOverflow = true;
            }
        }
        if (!inside(item))
            throw std::runtime_error("unreachable at " + std::to_string(width) + "x" + std::to_string(height)
                                     + ": " + geometry(item));
        if (openedOverflow) {
            CHECK(!test_support::accessibleName(item).isEmpty());
            keyClick(scene, Qt::Key_Escape);
            settleFor("file toolbar overflow closed", [&] {
                return !descendant(scene.window->contentItem(), [](QQuickItem *candidate) {
                    return candidate->objectName() == QStringLiteral("ActionsMenu") && candidate->isVisible();
                });
            });
        }
    }
    // Each pane header is one line high.
    for (const auto *name : {"stage2Label", "resultLabel", "stage3Label"}) {
        auto *label = scene.item(name);
        const auto line = QFontMetricsF(label->property("font").value<QFont>()).lineSpacing();
        if (label->height() > line * 1.5)
            throw std::runtime_error(std::string(name) + " is taller than one line at " + std::to_string(width) + "x"
                                     + std::to_string(height) + ": " + geometry(label));
    }
    qreal smallest = height;
    for (const auto *name : textPanes) smallest = std::min(smallest, scene.item(name)->height());
    return smallest;
}

std::string pageExtent(QObject *page)
{
    return "content " + std::to_string(int(page->property("contentHeight").toReal())) + " px in a "
        + std::to_string(int(page->property("height").toReal())) + " px page";
}

// Turns the mouse wheel over the named label at the top of the page. The page moves only when
// its content is taller than the window. Returns whether it was.
bool checkWheelScroll(Scene &scene, const char *over)
{
    auto *page = scene.object("pageScroll");
    page->setProperty("contentY", 0);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    const auto overflowing = page->property("overflowing").toBool();
    auto *label = scene.item(over);
    const auto point = label->mapToScene(QPointF(label->width() / 2, label->height() / 2));
    QWheelEvent wheel(point, scene.window->mapToGlobal(point), QPoint(), QPoint(0, -120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(scene.window, &wheel);
    if (overflowing) {
        settleFor("page scrolled by the wheel", [&] { return page->property("contentY").toReal() > 0; });
    } else {
        QElapsedTimer settled;
        settled.start();
        while (settled.elapsed() < 300) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (page->property("contentY").toReal() != 0)
            throw std::runtime_error("the wheel scrolled a page that fits the window: " + pageExtent(page));
    }
    return overflowing;
}

// Lays a wrapped text out as Text does, with its font, wrap mode and width inside its padding,
// and returns the first line break between two letters or digits of a word that would fit on
// a line of its own. The layout must give the item's own line count, or the check is not valid.
std::optional<QString> midWordBreak(QQuickItem *label)
{
    const auto width = label->width() - label->property("leftPadding").toReal()
        - label->property("rightPadding").toReal();
    const auto font = label->property("font").value<QFont>();
    const QFontMetricsF metrics(font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapMode(label->property("wrapMode").toInt()));
    std::optional<QString> broken;
    int lines = 0;
    for (const auto &paragraph : label->property("text").toString().split(QLatin1Char('\n'))) {
        QTextLayout layout(paragraph, font);
        layout.setTextOption(option);
        layout.beginLayout();
        for (auto line = layout.createLine(); line.isValid(); line = layout.createLine()) line.setLineWidth(width);
        layout.endLayout();
        lines += std::max(1, layout.lineCount());
        for (int i = 1; i < layout.lineCount() && !broken; ++i) {
            auto start = layout.lineAt(i).textStart();
            auto end = start;
            if (start <= 0 || start >= paragraph.size() || !paragraph[start - 1].isLetterOrNumber()
                || !paragraph[start].isLetterOrNumber())
                continue;
            while (start > 0 && paragraph[start - 1].isLetterOrNumber()) --start;
            while (end < paragraph.size() && paragraph[end].isLetterOrNumber()) ++end;
            const auto word = paragraph.mid(start, end - start);
            if (metrics.horizontalAdvance(word) <= width) broken = "\"" + word + "\" in \"" + paragraph + "\"";
        }
    }
    if (lines != label->property("lineCount").toInt())
        throw std::runtime_error("layout of \"" + label->property("text").toString().toStdString() + "\" gives "
                                 + std::to_string(lines) + " lines, the item has "
                                 + label->property("lineCount").toString().toStdString());
    return broken;
}

// Every visible wrapped text under root keeps its words whole. Returns how many texts wrap.
int expectWholeWords(QQuickItem *root, const std::string &where)
{
    int wrapped = 0;
    const std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
        if (!item->isVisible()) return;
        if (item->inherits("QQuickText") && item->property("wrapMode").toInt() != QTextOption::NoWrap
            && !item->property("text").toString().isEmpty()) {
            if (const auto word = midWordBreak(item))
                throw std::runtime_error(where + " breaks the word " + word->toStdString());
            if (item->property("lineCount").toInt() > item->property("text").toString().count(QLatin1Char('\n')) + 1)
                ++wrapped;
        }
        for (auto *child : item->childItems()) visit(child);
    };
    visit(root);
    return wrapped;
}

const QByteArray longLine = QByteArray(420, 'L') + "\n";

std::vector<PathSpec> journeyPaths()
{
    return {
        // The user replaced Git's conflict output with the common version before launch.
        {"story.txt", storyBase, storyLeft, storyRight, storyBase},
        {"notes.txt", QByteArray("notes base\n"), QByteArray("notes left\n"), QByteArray("notes right\n"),
         QByteArray("notes edited before launch \xc3\xa9\n")},
        // Git's own conflict output stays the launch result.
        {"reading.txt", "REPEAT one\n" + longLine + "REPEAT two\nspace\n",
         "REPEAT one\n" + longLine + "REPEAT left\nspace \n",
         "REPEAT one\n" + longLine + "REPEAT right\nspace\t\n", std::nullopt},
        // Modified on the left and deleted on the right: outside the app.
        {"removed.txt", QByteArray("removed base\n"), QByteArray("removed edited on the left\n"), std::nullopt, std::nullopt},
    };
}

std::vector<PathSpec> handoffPaths()
{
    std::vector<PathSpec> paths;
    for (const auto *name : {"done.txt", "newer.txt", "saved.txt", "typed.txt", "written.txt", "locked/failing.txt"}) {
        const QByteArray stem(name);
        paths.push_back({name, stem + " base\n", stem + " left\n", stem + " right\n", stem + " chosen by hand\n"});
    }
    paths.push_back({"removed.txt", QByteArray("removed base\n"), QByteArray("removed edited on the left\n"),
                     std::nullopt, std::nullopt});
    return paths;
}

// Owner-only read and execute on one directory, restored on scope exit.
class ReadOnlyDirectory {
public:
    explicit ReadOnlyDirectory(QString path) : path_(std::move(path))
    {
        CHECK(QFile::setPermissions(path_, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    }
    ~ReadOnlyDirectory()
    {
        QFile::setPermissions(path_, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }
private:
    QString path_;
};

QMap<QString, QVariantMap> handoffEntries(Scene &scene)
{
    QMap<QString, QVariantMap> entries;
    for (const auto &group : scene.controller->handoff().value(QStringLiteral("groups")).toList())
        for (const auto &entry : group.toMap().value(QStringLiteral("entries")).toList())
            entries.insert(entry.toMap().value(QStringLiteral("fileId")).toString(), entry.toMap());
    return entries;
}

void expectSourcesAround(Scene &scene, const Repository &repo, const char *path, const QString &leftLabel,
                         const QString &rightLabel)
{
    clickFile(scene, path);
    expectText(scene.text("selectedPath"), QString::fromLatin1(path), "selected path");
    // These fixture refs point exactly at the verified source commits in both merge and rebase.
    CHECK(scene.controller->loader()->stage2Name() == QStringLiteral("main"));
    CHECK(scene.controller->loader()->stage3Name() == QStringLiteral("side"));
    expectText(scene.text("stage2Label"), "main", "left version name");
    expectText(scene.text("stage3Label"), "side", "right version name");
    expectText(scene.text("stage2Identity"), leftLabel.section(": ", 0, 0) + " · " + utf8(repo.leftCommit).left(10), "left role and commit");
    expectText(scene.text("stage3Identity"), rightLabel.section(": ", 0, 0) + " · " + utf8(repo.rightCommit).left(10), "right role and commit");
    // The full commit and blob ids stay reachable as accessible text.
    const auto described = [&](const char *name) {
        const auto *face = QAccessible::queryAccessibleInterface(scene.item(name));
        return face ? face->text(QAccessible::Description) : QString();
    };
    for (const auto *name : {"stage2Label", "stage2Identity"}) {
        expectContains(described(name), utf8(repo.leftCommit), "left commit description");
        expectContains(described(name), utf8(repo.stageObject(2, path)), "left blob description");
    }
    for (const auto *name : {"stage3Label", "stage3Identity"}) {
        expectContains(described(name), utf8(repo.rightCommit), "right commit description");
        expectContains(described(name), utf8(repo.stageObject(3, path)), "right blob description");
    }
    expectText(scene.text("stage2Text"), utf8(repo.stageText(2, path)), "left source text");
    expectText(scene.text("stage3Text"), utf8(repo.stageText(3, path)), "right source text");
    // The editable center starts from the launch file exactly as it was on disk.
    expectText(scene.text("resultText"), utf8(readFile(repo.path(path))), "initial result");
    CHECK(scene.item("stage2Text")->property("readOnly").toBool());
    CHECK(scene.item("stage3Text")->property("readOnly").toBool());
    CHECK(!scene.item("resultText")->property("readOnly").toBool());
    // Compare the clipping viewports: unwrapped text can be wider than its pane.
    auto *left = scene.item("stage2Viewport");
    auto *result = scene.item("resultViewport");
    auto *right = scene.item("stage3Viewport");
    CHECK(left->isVisible() && result->isVisible() && right->isVisible());
    CHECK(sceneOrigin(left).x() + left->width() <= sceneOrigin(result).x() + 1);
    CHECK(sceneOrigin(result).x() + result->width() <= sceneOrigin(right).x() + 1);
    CHECK(left->width() > 50 && result->width() > 50 && right->width() > 50);
}

// Empty when the first line of the earliest undecided region on each side lies
// inside its pane's viewport, else where each pane stands.
std::string firstUndecidedInView(Scene &scene)
{
    struct Pane {
        const char *side;
        const char *viewport;
        const char *text;
        int line = 0;
        int position = -1;
    };
    Pane panes[] = {{"stage2", "stage2Viewport", "stage2Text"}, {"result", "resultViewport", "resultText"},
                    {"stage3", "stage3Viewport", "stage3Text"}};
    for (const auto &value : scene.controller->selectedFile()->regions()) {
        const auto region = value.toMap();
        if (region.value(QStringLiteral("state")).toString() != QStringLiteral("undecided")) continue;
        for (auto &pane : panes) {
            const bool result = std::strcmp(pane.side, "result") == 0;
            if (result && !region.value(QStringLiteral("mapped")).toBool()) continue;
            if (!result && region.value(QStringLiteral("side")).toString() != QLatin1String(pane.side)) continue;
            const auto prefix = QString::fromLatin1(result ? "result" : "source");
            const auto line = region.value(prefix + QStringLiteral("LineBegin")).toInt();
            if (pane.position >= 0 && line >= pane.line) continue;
            pane.line = line;
            pane.position = region.value(prefix + QStringLiteral("Utf16Begin")).toInt();
        }
    }
    std::string failures;
    for (const auto &pane : panes) {
        auto *view = scene.item(pane.viewport);
        auto *editor = scene.item(pane.text);
        QRectF rect;
        if (pane.position >= 0)
            CHECK(QMetaObject::invokeMethod(editor, "positionToRectangle", Q_RETURN_ARG(QRectF, rect),
                                            Q_ARG(int, pane.position)));
        auto top = editor->y() + rect.top() - view->property("contentY").toReal();
        auto height = rect.height();
        bool shown = pane.position >= 0;
        // A result line inside a folded marker block is represented by its visible
        // conflict row, not by the hidden QTextBlock's positionToRectangle.
        if (std::strcmp(pane.side, "result") == 0 && shown) {
            for (const auto &value : scene.controller->selectedFile()->markerBlocks()) {
                const auto block = value.toMap();
                const int first = block.value(QStringLiteral("resultLineBegin")).toInt();
                const int end = block.value(QStringLiteral("resultLineEnd")).toInt();
                if (!block.value(QStringLiteral("folded")).toBool() || pane.line < first || pane.line >= end) continue;
                auto *row = test_support::visualItem(scene.item("resultFoldedRows"), QStringLiteral("resultConflictRow-") + QString::number(first));
                shown = row && row->isVisible();
                if (shown) {
                    top = sceneOrigin(row).y() - sceneOrigin(view).y();
                    height = row->height();
                }
                break;
            }
        }
        if (!shown || top < 0 || top + height > view->height())
            failures += std::string(failures.empty() ? "" : "; ") + pane.side + " line " + std::to_string(pane.line)
                + " at " + std::to_string(top) + " of " + std::to_string(view->height());
    }
    return failures;
}

// Which journeys one run executes. Real merges and rebases make the journeys slow, so ctest
// registers each group as its own entry with a timeout sized for it.
enum class Selection { All, Review, Handoff, Window };

// Forwards a journey to the suite only when its group is selected.
class Group {
public:
    Group(test_support::Suite &suite, bool selected) : suite_(suite), selected_(selected) {}
    template<typename Test>
    void run(const std::string &name, Test test)
    {
        if (selected_) suite_.run(name, std::move(test));
    }
private:
    test_support::Suite &suite_;
    bool selected_;
};
}

class JourneyAcceptanceTests {
public:
    int run(Selection selection)
    {
        test_support::Suite results("JourneyAcceptanceTests");
        Group review(results, selection == Selection::All || selection == Selection::Review);
        Group handoff(results, selection == Selection::All || selection == Selection::Handoff);
        Group window(results, selection == Selection::All || selection == Selection::Window);
        review.run("PRD.md:127 PRD.md:139 PRD.md:157 the full inventory keeps truthful sources around the preserved editable result", [] {
            Repository repo("merge", journeyPaths());
            const auto launched = repo.snapshot();
            Scene scene(repo);
            CHECK(scene.controller->loader()->operation() == QStringLiteral("Merge"));
            expectText(scene.text("repositorySubtitle"), "Merging side into main", "merge operation with branches");
            CHECK(scene.controller->files()->totalFiles() == 4 && scene.controller->outsideAppFiles() == 1);
            CHECK(!scene.item("inventoryWarning")->isVisible());
            expectContains(scene.text("outsideAppCount"), "1 file to handle outside the app", "outside-app count");
            for (const auto *path : {"story.txt", "notes.txt", "reading.txt", "removed.txt"})
                expectContains(rowText(scene, path), QString::fromLatin1(path), "file row");
            expectContains(rowText(scene, "removed.txt"), "Handle outside the app", "outside-app row");
            const auto left = "Current checkout: " + utf8(repo.leftCommit).left(10);
            const auto right = "Merged commit: " + utf8(repo.rightCommit).left(10);
            for (const auto *path : {"story.txt", "notes.txt", "reading.txt"})
                expectSourcesAround(scene, repo, path, left, right);
            // A file edited before launch keeps that content exactly.
            clickFile(scene, "notes.txt");
            expectText(scene.text("resultText"), QString::fromUtf8("notes edited before launch \xc3\xa9\n"), "pre-launch edit");
            CHECK(!scene.text("resultText").contains(QStringLiteral("<<<<<<<")));
            clickFile(scene, "reading.txt");
            CHECK(scene.text("resultText").startsWith(QStringLiteral("REPEAT one\n"))
                  && scene.text("resultText").contains(QStringLiteral("<<<<<<< HEAD\n")));
            // the view folds each of Git's marker blocks into one row while
            // the buffer keeps it, and the overflow menu's toggle shows the block without changing
            // the text, decisions or history.
            expectMarkersFolded(scene, true);
            const auto reading = work(scene, "reading.txt");
            const auto readingProgress = scene.progress();
            toggleConflictMarkers(scene);
            expectMarkersFolded(scene, false);
            CHECK(scene.object("showConflictMarkers")->property("checked").toBool());
            CHECK(work(scene, "reading.txt") == reading && scene.progress() == readingProgress);
            toggleConflictMarkers(scene);
            expectMarkersFolded(scene, true);
            CHECK(work(scene, "reading.txt") == reading && scene.progress() == readingProgress);

            // Earlier context is shown on demand and hidden without touching the result.
            clickFile(scene, "notes.txt");
            const auto before = work(scene, "notes.txt");
            auto *earlier = scene.item("earlierText");
            CHECK(scene.item("showEarlier")->isEnabled() && !earlier->isVisible());
            scene.click("showEarlier");
            settleFor("earlier context shown", [&] { return earlier->isVisible(); });
            expectText(earlier->property("text").toString(), utf8(repo.stageText(1, "notes.txt")), "earlier text");
            expectContains(scene.text("earlierLabel"), "Common earlier version", "earlier label");
            CHECK(earlier->property("readOnly").toBool());
            scene.click("showEarlier");
            settleFor("earlier context hidden", [&] { return !earlier->isVisible(); });
            CHECK(work(scene, "notes.txt") == before);

            // The unsupported deletion stays listed with its reason and no editor.
            clickFile(scene, "removed.txt");
            CHECK(scene.item("selectedProblem")->isVisible() && !scene.item("resultText")->isVisible());
            expectContains(scene.text("selectedProblem"), "outside Branch Truce", "outside-app reason");
            // an outside-app file has no app draft, so neither its row nor its status calls it one.
            for (const auto &label : {rowText(scene, "removed.txt"), scene.text("selectedProblem")}) {
                if (label.contains(QStringLiteral("draft"), Qt::CaseInsensitive)
                    || label.contains(QStringLiteral("Manual Save checkpoint")))
                    throw std::runtime_error("outside-app file labelled as a draft: " + label.toStdString());
            }
            expectContains(scene.item("selectedProblem")->property("explanation").toString(), "git add", "outside-app next step");
            expectContains(rowText(scene, "story.txt"), "Not saved.", "supported row");

            // Saving the three supported results leaves the session and file warnings.
            for (const auto *path : {"story.txt", "notes.txt", "reading.txt"}) {
                clickFile(scene, path);
                scene.click("saveDraft");
                settleFor(std::string("save of ") + path, [&] { return scene.progress().value("saved").toBool(); });
                // Save writes the buffer, folded marker lines included.
                CHECK(scene.file(path).checkpoint()
                      && QString::fromStdString(scene.file(path).checkpoint()->draft.text) == scene.text("resultText"));
            }
            CHECK(!scene.item("inventoryWarning")->isVisible());
            CHECK(scene.item("outsideAppCount")->isVisible());
            clickFile(scene, "removed.txt");
            expectContains(scene.item("selectedProblem")->property("explanation").toString(), "outstanding", "outside-app notice");
            CHECK(scene.controller->files()->resolvedFiles() == 0);
            CHECK(repo.snapshot() == launched);
            checkOffline(scene);
        });
        review.run("PRD.md:139 rebasing keeps the arrangement with replay identities and explains missing earlier context", [] {
            Repository repo("rebase", {
                {"story.txt", storyBase, storyLeft, storyRight, std::nullopt},
                {"added.txt", std::nullopt, QByteArray("added on the left\n"), QByteArray("added on the right\n"), std::nullopt},
            });
            Scene scene(repo);
            CHECK(scene.controller->loader()->operation() == QStringLiteral("Rebase"));
            expectText(scene.text("repositorySubtitle"), "Rebasing side onto main", "rebase operation with branches");
            CHECK(scene.controller->files()->totalFiles() == 2 && scene.controller->outsideAppFiles() == 0);
            const auto left = "Current rebased state: " + utf8(repo.leftCommit).left(10);
            const auto right = "Commit being replayed: " + utf8(repo.rightCommit).left(10);
            expectSourcesAround(scene, repo, "story.txt", left, right);
            CHECK(scene.item("showEarlier")->isEnabled());
            expectSourcesAround(scene, repo, "added.txt", left, right);
            const auto before = work(scene, "added.txt");
            auto *toggle = scene.item("showEarlier");
            CHECK(!toggle->isEnabled() && !scene.item("earlierText")->isVisible());
            expectText(toggle->property("text").toString(), "Common earlier version unavailable", "unavailable earlier");
            CHECK(work(scene, "added.txt") == before);
            checkOffline(scene);
        });
        review.run("PRD.md:157 scoped choices and ordered combinations over pre-launch edits share one undo history with typing", [] {
            Repository repo("merge", journeyPaths());
            Scene scene(repo);
            clickFile(scene, "notes.txt");
            const auto notes = scene.file("notes.txt").current();
            clickFile(scene, "story.txt");
            std::vector<Work> history{work(scene, "story.txt")};
            const auto state = [&](const char *label) {
                return choiceRow(scene, QString::fromUtf8(label)).value(QStringLiteral("state")).toString();
            };
            // Replacing pre-launch content asks first; cancelling changes nothing.
            clickChoice(scene, "Left line 2", "Accept");
            CHECK(scene.item("manualReplacementNotice")->isVisible());
            expectText(scene.text("replacementBeforeText"), "base first\n", "replaced scope");
            expectText(scene.text("replacementAfterText"), "left first\n", "replacement");
            clickDialogButton(scene, "replacementDialog", "Cancel");
            CHECK(work(scene, "story.txt") == history.back());
            CHECK(state("Left line 2") == QStringLiteral("undecided"));

            // A line choice decides only its side; the competitor stays available.
            clickChoice(scene, "Left line 2", "Accept");
            clickDialogButton(scene, "replacementDialog", "Apply choice");
            expectText(scene.text("resultText"), "header\nleft first\nbridge one\nbridge two\nbridge three\nbase later\ntail\n", "accept left");
            CHECK(state("Left line 2") == QStringLiteral("accepted") && state("Right line 2") == QStringLiteral("undecided"));
            CHECK(choiceRow(scene, "Right line 2").value(QStringLiteral("available")).toBool());
            CHECK(state("Left line 6") == QStringLiteral("undecided") && state("Right line 6") == QStringLiteral("undecided"));
            history.push_back(work(scene, "story.txt"));

            // Resolving the other region with one side explicitly rejects its competitor.
            clickChoice(scene, QString::fromUtf8("Right section, lines 6–6"), "Resolve");
            clickDialogButton(scene, "replacementDialog", "Apply choice");
            expectText(scene.text("resultText"), "header\nleft first\nbridge one\nbridge two\nbridge three\nright later\ntail\n", "resolve right");
            CHECK(state("Right line 6") == QStringLiteral("accepted") && state("Left line 6") == QStringLiteral("ignored"));
            CHECK(state("Left line 2") == QStringLiteral("accepted") && state("Right line 2") == QStringLiteral("undecided"));
            history.push_back(work(scene, "story.txt"));

            // Ignore decides against a contribution without changing the result.
            clickChoice(scene, "Right line 2", "Ignore");
            CHECK(!scene.object("replacementDialog")->property("visible").toBool());
            CHECK(work(scene, "story.txt").shown == history.back().shown);
            CHECK(state("Right line 2") == QStringLiteral("ignored") && state("Left line 2") == QStringLiteral("accepted"));
            history.push_back(work(scene, "story.txt"));
            CHECK(!(history.back() == history[history.size() - 2]));

            // An inspectable order: preview Right then Left before applying it.
            clickChoice(scene, QString::fromUtf8("Right section, lines 2–2"), "PreviewRightFirst");
            expectText(scene.object("replacementDialog")->property("title").toString(), "Preview Right then Left", "order title");
            expectText(scene.text("replacementAfterText"), "right first\nleft first\n", "ordered preview");
            clickDialogButton(scene, "replacementDialog", "Apply choice");
            expectText(scene.text("resultText"), "header\nright first\nleft first\nbridge one\nbridge two\nbridge three\nright later\ntail\n", "ordered combination");
            CHECK(state("Right line 2") == QStringLiteral("accepted") && state("Left line 2") == QStringLiteral("accepted"));
            history.push_back(work(scene, "story.txt"));

            // Direct typing joins the same history.
            appendToResult(scene, "ok");
            expectText(scene.text("resultText"), "header\nright first\nleft first\nbridge one\nbridge two\nbridge three\nright later\ntail\nok", "typed");
            CHECK(scene.file("story.txt").current().draft.decisions == history.back().draft.decisions);
            const auto typed = work(scene, "story.txt");

            // Undo by shortcut and button walks back through text and decisions together.
            keyClick(scene, Qt::Key_Z, Qt::ControlModifier);
            const auto onceUndone = work(scene, "story.txt");
            auto *toolbarUndo = scene.item("undoCommand");
            auto *sharedRedo = scene.object("resultEditActions")->property("redo").value<QObject *>();
            CHECK(sharedRedo);
            auto *menuRedo = scene.object("resultMenuRedo");
            CHECK(menuRedo->property("action").value<QObject *>() == sharedRedo);
            const auto redoBindings = QKeySequence::keyBindings(QKeySequence::Redo);
            CHECK(redoBindings.contains(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)));
            for (const auto &binding : redoBindings) {
                CHECK(binding.count() == 1);
                toolbarUndo->forceActiveFocus(Qt::TabFocusReason);
                CHECK(toolbarUndo->hasActiveFocus());
                keyClick(scene, Qt::Key_Space);
                CHECK(toolbarUndo->hasActiveFocus());
                CHECK(work(scene, "story.txt") == history.back());
                CHECK(scene.item("redoCommand")->isEnabled() && scene.file("story.txt").canRedo());
                CHECK(sharedRedo->property("enabled").toBool() && menuRedo->property("enabled").toBool());
                const auto key = binding[0];
                keyClick(scene, key.key(), key.keyboardModifiers());
                CHECK(work(scene, "story.txt") == onceUndone);
            }
            scene.click("undoCommand");
            CHECK(work(scene, "story.txt") == history.back());
            for (std::size_t step = history.size() - 1; step-- > 0;) {
                if (step % 2) keyClick(scene, Qt::Key_Z, Qt::ControlModifier);
                else scene.click("undoCommand");
                CHECK(work(scene, "story.txt") == history[step]);
            }
            CHECK(!scene.file("story.txt").canUndo() && !scene.item("undoCommand")->isEnabled());
            for (std::size_t step = 1; step < history.size(); ++step) {
                if (step % 2) {
                    // TextArea handles all platform redo bindings when it has focus.
                    clickText(scene, scene.item("resultText"));
                    CHECK(scene.item("resultText")->hasActiveFocus());
                    keyClick(scene, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
                } else scene.click("redoCommand");
                CHECK(work(scene, "story.txt") == history[step]);
            }
            scene.click("redoCommand");
            clickText(scene, scene.item("resultText"));
            CHECK(scene.item("resultText")->hasActiveFocus());
            keyClick(scene, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
            CHECK(work(scene, "story.txt") == typed && !scene.file("story.txt").canRedo());
            CHECK(!scene.item("redoCommand")->isEnabled());
            CHECK(!sharedRedo->property("enabled").toBool() && !menuRedo->property("enabled").toBool());
            // Other files are untouched.
            CHECK(scene.file("notes.txt").current().id == notes.id && scene.file("notes.txt").current().draft == notes.draft);
            checkOffline(scene);
        });
        review.run("PRD.md:41 PRD.md:49 typing in an accepted line marks its region manual until a choice replaces it", [] {
            Repository repo("merge", journeyPaths());
            Scene scene(repo);
            clickFile(scene, "story.txt");
            CHECK(scene.controller->selectedFile()->fileName() == QStringLiteral("story.txt"));
            const auto region = [&](const char *label) { return regionFor(scene, QString::fromUtf8(label)); };
            const auto edited = [&](const char *label) {
                return region(label).value(QStringLiteral("handEdited")).toBool();
            };

            // The launch result differs from both sides, but nobody has typed in it yet.
            const auto launched = scene.controller->selectedFile()->regions();
            CHECK(!launched.isEmpty());
            for (const auto &value : launched) {
                CHECK(!value.toMap().value(QStringLiteral("handEdited")).toBool());
                CHECK(value.toMap().value(QStringLiteral("state")).toString() != QStringLiteral("manual"));
            }

            clickChoice(scene, "Left line 2", "Accept");
            clickDialogButton(scene, "replacementDialog", "Apply choice");
            CHECK(region("Left line 2").value(QStringLiteral("state")).toString() == QStringLiteral("accepted"));
            CHECK(!edited("Left line 2"));
            expectText(regionResult(scene, region("Left line 2")), "left first\n", "accepted range");

            // Typing inside the accepted line in the editor marks only that region.
            auto *editor = scene.item("resultText");
            clickText(scene, editor);
            CHECK(editor->hasActiveFocus());
            keyClick(scene, Qt::Key_Home, Qt::ControlModifier);
            keyClick(scene, Qt::Key_Down);
            for (int step = 0; step < 4; ++step) keyClick(scene, Qt::Key_Right);
            typeText(scene, " hand");
            expectText(scene.text("resultText"), "header\nleft hand first\nbridge one\nbridge two\nbridge three\nbase later\ntail\n", "typed");
            const auto typed = work(scene, "story.txt");
            CHECK(region("Left line 2").value(QStringLiteral("state")).toString() == QStringLiteral("manual"));
            CHECK(edited("Left line 2"));
            expectText(regionResult(scene, region("Left line 2")), "left hand first\n", "edited range");
            CHECK(!edited("Left line 6") && !edited("Right line 6"));
            CHECK(region("Left line 6").value(QStringLiteral("state")).toString() == QStringLiteral("undecided"));

            // An ordered combination over that line replaces the typed text and clears the mark.
            clickChoice(scene, QString::fromUtf8("Right section, lines 2–2"), "PreviewLeftFirst");
            CHECK(scene.item("manualReplacementNotice")->isVisible());
            expectText(scene.text("replacementBeforeText"), "left hand first\n", "replaced edit");
            clickDialogButton(scene, "replacementDialog", "Apply choice");
            expectText(scene.text("resultText"), "header\nleft first\nright first\nbridge one\nbridge two\nbridge three\nbase later\ntail\n", "combined");
            CHECK(!edited("Left line 2") && !edited("Right line 2"));
            CHECK(region("Left line 2").value(QStringLiteral("state")).toString() == QStringLiteral("accepted"));
            CHECK(region("Right line 2").value(QStringLiteral("state")).toString() == QStringLiteral("accepted"));

            // Undo brings back the typed text together with its mark.
            scene.click("undoCommand");
            CHECK(work(scene, "story.txt") == typed);
            CHECK(edited("Left line 2"));
            CHECK(region("Left line 2").value(QStringLiteral("state")).toString() == QStringLiteral("manual"));
            checkOffline(scene);
        });
        review.run("PRD.md:135 PRD.md:153 Save keeps an app-only draft and Mark resolved without a prior Save confirms, writes and resolves", [] {
            Repository repo("merge", journeyPaths());
            const auto launched = repo.snapshot();
            Scene scene(repo);
            const auto status = [&] { return scene.controller->commandMessage(); };

            // Save checkpoints an unfinished draft in the app only.
            clickFile(scene, "notes.txt");
            appendToResult(scene, "draft");
            CHECK(!scene.progress().value("saved").toBool() && !scene.progress().value("reviewed").toBool() && !scene.progress().value("resolved").toBool());
            scene.click("saveDraft");
            settleFor("notes saved", [&] { return scene.progress().value("saved").toBool(); });
            CHECK(scene.progress().value("saved").toBool() && !scene.progress().value("reviewed").toBool() && !scene.progress().value("resolved").toBool());
            CHECK(scene.progress().value("undecided").toULongLong() > 0);
            CHECK(repo.snapshot() == launched);
            CHECK(!repo.unmerged("notes.txt").isEmpty());

            // Mark resolved with undecided choices asks for confirmation; cancelling keeps everything.
            clickFile(scene, "story.txt");
            appendToResult(scene, "manual");
            const auto edited = work(scene, "story.txt");
            CHECK(!scene.progress().value("saved").toBool());
            scene.click("markResolved");
            settleFor("resolution confirmation", [&] { return scene.controller->resolutionConfirmation(); });
            expectContains(scene.controller->commandMessage(), "intentional", "confirmation message");
            scene.click("cancelResolution");
            CHECK(!scene.controller->resolutionConfirmation());
            CHECK(work(scene, "story.txt") == edited);
            CHECK(!scene.progress().value("reviewed").toBool() && !scene.progress().value("saved").toBool());
            CHECK(repo.snapshot() == launched);

            // Confirming saves, writes and resolves the displayed result without a prior Save.
            markResolvedByMouse(scene);
            const auto progress = scene.progress();
            if (!progress.value("resolved").toBool())
                throw std::runtime_error("not resolved: " + status().toStdString());
            CHECK(progress.value("saved").toBool() && progress.value("reviewed").toBool());
            CHECK(progress.value("undecided").toULongLong() > 0);
            expectContains(rowText(scene, "story.txt"), "Resolved", "resolved status");
            CHECK(work(scene, "story.txt") == edited);
            CHECK(readFile(repo.path("story.txt")) == edited.shown.toUtf8());
            CHECK(repo.unmerged("story.txt").isEmpty());
            const auto staged = repo.git({"ls-files", "-s", "--", "story.txt"});
            CHECK(staged.startsWith("100644 " + repo.git({"hash-object", "--", "story.txt"}).trimmed() + " 0\t"));
            CHECK(scene.controller->files()->resolvedFiles() == 1);
            // Only that file changed; the saved draft stays off the repository.
            CHECK(readFile(repo.path("notes.txt")) == launched.value("notes.txt"));
            CHECK(!repo.unmerged("notes.txt").isEmpty() && !repo.unmerged("reading.txt").isEmpty());

            // Editing again makes a newer draft that invalidates review and is distinguished from Git's copy.
            appendToResult(scene, "+");
            const auto newer = scene.progress();
            CHECK(!newer.value("reviewed").toBool() && newer.value("earlierResolved").toBool());
            expectContains(scene.text("staleStagedNotice"), "Earlier result staged; current draft not applied", "newer draft status");
            CHECK(readFile(repo.path("story.txt")) == edited.shown.toUtf8());
            checkOffline(scene);
        });
        review.run("PRD.md:137 PRD.md:141 scoped search, wrapping, whitespace, mouse inspection and clipboard shortcuts keep the work", [] {
            Repository repo("merge", journeyPaths());
            const auto launched = repo.snapshot();
            Scene scene(repo);
            clickFile(scene, "story.txt");
            const auto story = scene.file("story.txt").current();
            clickFile(scene, "reading.txt");
            const auto before = work(scene, "reading.txt");
            const auto unchanged = [&] {
                CHECK(work(scene, "reading.txt") == before && !scene.file("reading.txt").canUndo());
                expectText(scene.text("stage2Text"), utf8(repo.stageText(2, "reading.txt")), "left source");
                expectText(scene.text("stage3Text"), utf8(repo.stageText(3, "reading.txt")), "right source");
            };
            auto *left = scene.item("stage2Text");
            auto *result = scene.item("resultText");

            // Repeated passages: Find walks the scoped pane and wraps to the first match.
            clickText(scene, result);
            keyClick(scene, Qt::Key_F, Qt::ControlModifier);
            CHECK(scene.item("readingControls")->isVisible());
            CHECK(scene.item("searchQuery")->hasActiveFocus());
            expectText(scene.item("searchScope")->property("currentText").toString(), "Left source", "default scope");
            typeText(scene, "REPEAT");
            expectText(scene.text("searchQuery"), "REPEAT", "typed query");
            const auto leftText = left->property("text").toString();
            const auto second = leftText.indexOf(QStringLiteral("REPEAT"), 1);
            expectContains(scene.text("searchStatus"), "1 of 2", "initial match count");
            keyClick(scene, Qt::Key_Return);
            CHECK(left->property("selectionStart").toInt() == 0);
            expectText(left->property("selectedText").toString(), "REPEAT", "left match");
            expectContains(scene.text("searchStatus"), "Passage in Left source.", "left status");
            CHECK(result->property("selectedText").toString().isEmpty());
            expectContains(scene.text("searchStatus"), "1 of 2", "first match count");
            scene.click("closePassageSearch");
            CHECK(!scene.item("readingControls")->isVisible());
            clickText(scene, result);
            keyClick(scene, Qt::Key_Find);
            CHECK(scene.item("readingControls")->isVisible());
            CHECK(scene.item("searchQuery")->hasActiveFocus());
            expectText(scene.text("searchQuery"), "REPEAT", "retained query");
            expectContains(scene.text("searchStatus"), "1 of 2", "retained match count");
            keyClick(scene, Qt::Key_Return);
            for (const int expected : {static_cast<int>(second), 0}) {
                CHECK(left->property("selectionStart").toInt() == expected);
                expectText(left->property("selectedText").toString(), "REPEAT", "left match");
                expectContains(scene.text("searchStatus"), "Passage in Left source.", "left status");
                expectContains(scene.text("searchStatus"), expected == second ? "2 of 2" : "1 of 2", "match count");
                CHECK(result->property("selectedText").toString().isEmpty());
                if (expected == second) scene.click("findPassage");
            }
            unchanged();
            // Choose the result scope with the mouse.
            scene.click("searchScope");
            auto *scopePopup = scene.item("searchScope")->property("popup").value<QObject *>();
            CHECK(scopePopup);
            settleFor("search scope list", [&] { return scopePopup->property("opened").toBool(); });
            auto *option = stable(scene, "Editable result option", [&] {
                return test_support::namedButton(test_support::popupRoot(scopePopup), QStringLiteral("Editable result"));
            });
            test_support::clickItem(scene.window, option);
            settleFor("result scope", [&] {
                return scene.item("searchScope")->property("currentText").toString() == QStringLiteral("Editable result");
            });
            scene.click("findPassage");
            expectContains(scene.text("searchStatus"), "Passage in Editable result.", "result status");
            CHECK(result->property("selectionStart").toInt() == 0);
            expectText(result->property("selectedText").toString(), "REPEAT", "result match");
            const auto selectionStart = result->property("selectionStart").toInt();
            const auto selectionEnd = result->property("selectionEnd").toInt();
            unchanged();

            // Wrapping the long line changes its layout only.
            const int longBlock = 1;
            CHECK(test_support::visualLines(result, longBlock) == 1 && test_support::visualLines(left, longBlock) == 1);
            scene.click("wrapLines");
            settleFor("wrapped long line", [&] {
                return test_support::visualLines(result, longBlock) > 1 && test_support::visualLines(left, longBlock) > 1;
            });
            unchanged();
            scene.click("wrapLines");
            settleFor("unwrapped long line", [&] { return test_support::visualLines(result, longBlock) == 1; });
            unchanged();

            // The whitespace differences are inside Git's folded conflict block.
            // Reveal the block through the toolbar before inspecting its spaces and tab.
            expectMarkersFolded(scene, true);
            toggleConflictMarkers(scene);
            expectMarkersFolded(scene, false);
            unchanged();
            auto *markers = scene.item("resultWhitespace");
            CHECK(test_support::paintedMarkers(markers) == 0);
            scene.click("showWhitespace");
            const auto *resultDocument = result->property("textDocument").value<QQuickTextDocument *>()->textDocument();
            QString shown;
            for (auto block = resultDocument->begin(); block.isValid(); block = block.next())
                if (block.isVisible()) shown += block.text() + QLatin1Char('\n');
            CHECK(shown.count(' ') == before.shown.count(' ') && shown.count('\t') == before.shown.count('\t'));
            CHECK(shown.count('\t') == 1 && shown.contains(QStringLiteral("space \n")));
            settleFor("whitespace markers", [&] {
                return test_support::markerKind(markers, true) == shown.count('\t')
                    && test_support::markerKind(markers, false) == shown.count(' ')
                    && test_support::paintedMarkers(markers) == shown.count('\t') + shown.count(' ');
            });
            CHECK(test_support::markerKind(scene.item("stage2Whitespace"), true) == 0);
            CHECK(test_support::markerKind(scene.item("stage3Whitespace"), true) == 1);
            CHECK(result->property("selectionStart").toInt() == selectionStart
                  && result->property("selectionEnd").toInt() == selectionEnd);
            unchanged();
            scene.click("showWhitespace");
            settleFor("whitespace hidden", [&] { return test_support::paintedMarkers(markers) == 0; });
            CHECK(result->property("selectionStart").toInt() == selectionStart
                  && result->property("selectionEnd").toInt() == selectionEnd);
            unchanged();

            // Sources accept mouse selection but not typing.
            clickText(scene, left);
            CHECK(left->hasActiveFocus());
            typeText(scene, "x");
            unchanged();

            // Clipboard and history shortcuts in the result.
            clickText(scene, result);
            CHECK(result->hasActiveFocus());
            keyClick(scene, Qt::Key_Home, Qt::ControlModifier);
            keyClick(scene, Qt::Key_End, Qt::ShiftModifier);
            expectText(result->property("selectedText").toString(), "REPEAT one", "keyboard selection");
            QGuiApplication::clipboard()->clear();
            keyClick(scene, Qt::Key_C, Qt::ControlModifier);
            expectText(QGuiApplication::clipboard()->text(), "REPEAT one", "copied");
            unchanged();
            keyClick(scene, Qt::Key_End, Qt::ControlModifier);
            keyClick(scene, Qt::Key_V, Qt::ControlModifier);
            const auto pasted = work(scene, "reading.txt");
            expectText(pasted.shown, before.shown + "REPEAT one", "pasted");
            CHECK(pasted.draft.decisions == before.draft.decisions);
            keyClick(scene, Qt::Key_Z, Qt::ControlModifier);
            CHECK(work(scene, "reading.txt") == before);
            keyClick(scene, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
            CHECK(work(scene, "reading.txt") == pasted);
            scene.click("undoCommand");
            CHECK(work(scene, "reading.txt") == before);
            CHECK(scene.file("story.txt").current().id == story.id && scene.file("story.txt").current().draft == story.draft);
            CHECK(repo.snapshot() == launched);
            QGuiApplication::clipboard()->clear();
            checkOffline(scene);
        });
        handoff.run("PRD.md:143 PRD.md:145 the handoff separates resolved, saved, unsaved, newer, failed and outside-app work and leaving keeps it outstanding", [] {
            Repository repo("merge", handoffPaths());
            Scene scene(repo);
            CHECK(scene.controller->files()->totalFiles() == 7);
            clickFile(scene, "done.txt");
            markResolvedByMouse(scene);
            CHECK(scene.progress().value("resolved").toBool());
            clickFile(scene, "newer.txt");
            markResolvedByMouse(scene);
            CHECK(scene.progress().value("resolved").toBool());
            appendToResult(scene, "again");
            clickFile(scene, "saved.txt");
            appendToResult(scene, "kept");
            scene.click("saveDraft");
            settleFor("saved draft", [&] { return scene.progress().value("saved").toBool(); });
            clickFile(scene, "typed.txt");
            appendToResult(scene, "typed");
            clickFile(scene, "locked/failing.txt");
            appendToResult(scene, "fails");
            {
                ReadOnlyDirectory locked(repo.path("locked"));
                markResolvedByMouse(scene);
            }
            // A failed repository write keeps the saved app draft and leaves the file unresolved.
            const auto failed = scene.progress();
            expectContains(scene.text("fileFailureNotice"), "Repository write failed", "failed status");
            CHECK(failed.value("saved").toBool() && !failed.value("resolved").toBool());
            CHECK(scene.text("resultText").endsWith(QStringLiteral("fails")));
            CHECK(readFile(repo.path("locked/failing.txt")) == "locked/failing.txt chosen by hand\n");
            CHECK(!repo.unmerged("locked/failing.txt").isEmpty());

            // The repository write succeeds but Git refuses the resolution: written, unresolved.
            clickFile(scene, "written.txt");
            const auto hook = repo.path(".git/hooks/post-index-change");
            writeFile(hook, "#!/bin/sh\nexit 1\n");
            CHECK(QFile::setPermissions(hook, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
            markResolvedByMouse(scene);
            CHECK(QFile::remove(hook));
            const auto written = scene.progress();
            CHECK(!written.value("resolved").toBool());
            expectContains(scene.text("fileFailureNotice"), "Written, unresolved", "written status");
            CHECK(readFile(repo.path("written.txt")) == "written.txt chosen by hand\n");
            CHECK(!repo.unmerged("written.txt").isEmpty());

            // Outstanding-work indicators are words, not only colours.
            expectContains(rowText(scene, "done.txt"), "Saved. Reviewed. Resolved.", "done row");
            expectContains(rowText(scene, "typed.txt"), "Not saved. Not reviewed. Not resolved.", "typed row");
            expectContains(rowText(scene, "newer.txt"), "Earlier result staged; current draft not applied", "newer row");

            auto *dialog = scene.object("handoffDialog");
            const auto opened = [&] { return dialog->property("opened").toBool(); };
            scene.click("openHandoff");
            settleFor("handoff opened", opened);
            expectText(scene.text("handoffOutcome"),
                       "6 of 7 files in this session remain outstanding. This session is not finished.", "handoff outcome");
            CHECK(!scene.item("handoffFinish")->isVisible() && !scene.controller->handoff().value("clean").toBool());
            const auto entries = handoffEntries(scene);
            const QMap<QString, QString> expected{{"done.txt", "published"}, {"newer.txt", "newer-draft"},
                {"saved.txt", "saved-draft"}, {"typed.txt", "undecided"}, {"written.txt", "failed"},
                {"locked/failing.txt", "failed"},
                {"removed.txt", "outside-app"}};
            CHECK(entries.size() == expected.size());
            for (auto entry = expected.cbegin(); entry != expected.cend(); ++entry) {
                const auto fields = entries.value(fileId(entry.key().toUtf8().constData()));
                expectText(fields.value("category").toString(), entry.value(), "handoff category");
                CHECK(fields.value("outstanding").toBool() == (entry.value() != QStringLiteral("published")));
            }
            const auto blockers = [&](const char *path) {
                return entries.value(fileId(path)).value("blockers").toStringList().join(QStringLiteral("; "));
            };
            expectContains(blockers("typed.txt"), "Current draft has no manual Save checkpoint", "unsaved blocker");
            CHECK(!blockers("saved.txt").contains(QStringLiteral("no manual Save checkpoint")));
            expectContains(blockers("newer.txt"), "Earlier result staged; current draft not applied", "newer blocker");
            expectContains(blockers("locked/failing.txt"), "Repository write failed", "failure blocker");
            expectContains(blockers("written.txt"), "Git resolution failed", "stage blocker");
            expectContains(blockers("removed.txt"), "Handle this file outside Branch Truce", "outside-app blocker");
            // an outside-app file has no app draft to Save, mark resolved, write or stage.
            expectText(blockers("removed.txt"), "Handle this file outside Branch Truce", "outside-app blockers");
            CHECK(blockers("done.txt").isEmpty());

            // Each route returns to its file.
            for (const auto *path : {"newer.txt", "locked/failing.txt", "removed.txt"}) {
                if (!opened()) scene.click("openHandoff");
                settleFor("handoff reopened", opened);
                scene.click(("handoffRoute-" + fileId(path)).toLatin1().constData());
                // Until its fade-out ends the closing dialog's overlay takes the next click.
                settleFor("handoff closed by route", [&] { return !dialog->property("visible").toBool(); });
                CHECK(scene.controller->selectedFileId() == fileId(path));
            }
            CHECK(scene.item("selectedProblem")->isVisible());

            // Leaving after the warning runs no Git command and keeps every outstanding file unresolved.
            int ended = 0;
            bool clean = true;
            QObject::connect(scene.controller.get(), &ApplicationController::sessionEnded, scene.controller.get(),
                             [&](const QString &, bool value) { ++ended; clean = value; });
            const auto beforeLeave = repo.snapshot();
            const auto traced = readFile(sandbox->argvLog);
            scene.click("openHandoff");
            settleFor("handoff opened for leave", opened);
            CHECK(scene.item("handoffLeave")->isEnabled() && !scene.item("handoffLeaveBlocker")->isVisible());
            scene.click("handoffLeave");
            if (ended != 1 || clean)
                throw std::runtime_error("leave ended=" + std::to_string(ended) + " clean=" + std::to_string(clean)
                    + " outcome=" + scene.controller->sessionOutcome().toStdString() + " opened=" + std::to_string(opened())
                    + " message=" + scene.controller->commandMessage().toStdString() + " blocker="
                    + scene.controller->handoff().value("leaveBlocker").toString().toStdString());
            CHECK(ended == 1 && !clean);
            expectContains(scene.controller->sessionOutcome(), "Left with 6 of 7 files outstanding.", "leave outcome");
            CHECK(repo.snapshot() == beforeLeave && readFile(sandbox->argvLog) == traced);
            for (const auto *path : {"saved.txt", "typed.txt", "written.txt", "locked/failing.txt", "removed.txt"})
                CHECK(!repo.unmerged(path).isEmpty());
            CHECK(repo.unmerged("done.txt").isEmpty() && repo.unmerged("newer.txt").isEmpty());
            // The summary speaks only for this session.
            expectContains(scene.controller->loader()->workflow(), "only the work opened at launch", "session scope");
            checkOffline(scene);
        });
        handoff.run("a newer draft goes back to the staged result from the file toolbar and the handoff", [] {
            Repository repo("merge", {{"newer.txt", QByteArray("newer base\n"), QByteArray("newer left\n"),
                                       QByteArray("newer right\n"), QByteArray("newer chosen by hand\n")}});
            Scene scene(repo);
            const auto id = fileId("newer.txt");
            clickFile(scene, "newer.txt");
            markResolvedByMouse(scene);
            CHECK(scene.progress().value("resolved").toBool());
            const auto staged = scene.text("resultText");
            const auto published = repo.snapshot();
            CHECK(repo.unmerged("newer.txt").isEmpty());
            auto *prompt = scene.object("discardStagedDialog");
            auto *handoff = scene.object("handoffDialog");
            const auto newerDraft = [&] {
                appendToResult(scene, "again");
                settleFor("newer draft", [&] { return scene.progress().value("earlierResolved").toBool(); });
                CHECK(scene.text("resultText").endsWith(QStringLiteral("again")));
            };
            newerDraft();
            const auto draft = scene.text("resultText");

            // The overflow entry asks first. The prompt names the file, says the draft is lost and
            // confirms with the destructive Discard button, labelled with what it does.
            clickOverflowAction(scene, "discardToStaged");
            settleFor("discard prompt opened", [&] { return prompt->property("opened").toBool(); });
            expectContains(prompt->property("subtitle").toString(), "The draft of newer.txt is lost", "discard prompt");
            CHECK(prompt->property("standardButtons").toInt() & QDialogButtonBox::Discard);
            expectText(dialogButton(scene, "discardStagedDialog", "confirmDiscardStaged")->property("text").toString(),
                       "Discard draft", "destructive button");
            CHECK(!scene.item("discardStagedError")->isVisible());
            // Cancelling changes nothing.
            clickDialogButton(scene, "discardStagedDialog", "Cancel");
            expectText(scene.text("resultText"), draft, "draft after cancel");
            CHECK(scene.progress().value("earlierResolved").toBool() && !scene.progress().value("resolved").toBool());
            CHECK(repo.snapshot() == published);

            // The handoff entry that says to discard back to the staged result offers it.
            scene.click("openHandoff");
            settleFor("handoff opened", [&] { return handoff->property("opened").toBool(); });
            auto entry = handoffEntries(scene).value(id);
            expectText(entry.value("category").toString(), "newer-draft", "handoff category");
            expectText(entry.value("actionLabel").toString(), "Mark resolved or discard back to the staged result", "route");
            const auto discardName = ("handoffDiscardStaged-" + id).toLatin1();
            expectText(scene.text(discardName.constData()), "Discard draft and use staged result", "handoff discard");
            scene.click(discardName.constData());
            settleFor("handoff closed for discard", [&] { return !handoff->property("visible").toBool(); });
            clickDialogButton(scene, "discardStagedDialog", "confirmDiscardStaged");
            settleFor("staged result restored", [&] { return scene.progress().value("resolved").toBool(); });
            expectText(scene.text("resultText"), staged, "staged result");
            CHECK(!scene.progress().value("earlierResolved").toBool());
            expectText(scene.controller->commandMessage(),
                       "Discarded the draft. The result staged in Git is current again; the repository file is unchanged.",
                       "discard message");
            expectText(handoffEntries(scene).value(id).value("category").toString(), "published", "handoff after discard");
            CHECK(scene.controller->handoff().value("clean").toBool());
            CHECK(repo.snapshot() == published);

            // A discard the recovery journal cannot record keeps the draft and shows an error.
            newerDraft();
            {
                QLockFile busy(scene.controller->files()->recoveryDirectory() + QStringLiteral("/journal.lock"));
                CHECK(busy.tryLock(0));
                clickOverflowAction(scene, "discardToStaged");
                test_support::clickItem(scene.window, dialogButton(scene, "discardStagedDialog", "confirmDiscardStaged"));
                settleFor("discard failure shown", [&] { return scene.item("discardStagedError")->isVisible(); });
                CHECK(prompt->property("opened").toBool());
                expectText(scene.item("discardStagedError")->property("text").toString(),
                           "Discard was not completed. The draft is kept until retry finishes the discard.", "discard error");
                CHECK(scene.item("discardStagedError")->property("type").toInt() == 3); // Kirigami.MessageType.Error
                CHECK(scene.controller->files()->hasPendingRecovery());
                expectText(QString::fromStdString(scene.file("newer.txt").current().draft.text), draft, "kept draft");
                clickDialogButton(scene, "discardStagedDialog", "Cancel");
                CHECK(scene.item("recoveryProblem")->isVisible());
            }
            // Retry finishes that same discard.
            scene.click("retryRecovery");
            settleFor("retried discard", [&] { return !scene.controller->files()->hasPendingRecovery(); });
            settleFor("staged result after retry", [&] { return scene.progress().value("resolved").toBool(); });
            expectText(QString::fromStdString(scene.file("newer.txt").current().draft.text), staged, "staged after retry");
            CHECK(repo.snapshot() == published);
            checkOffline(scene);
        });
        handoff.run("PRD.md:149 offline startup without an account resumes saved and unsaved drafts after closing", [] {
            Repository repo("merge", {
                {"saved.txt", QByteArray("saved base\n"), QByteArray("saved left\n"), QByteArray("saved right\n"), QByteArray("saved by hand\n")},
                {"typed.txt", QByteArray("typed base\n"), QByteArray("typed left\n"), QByteArray("typed right\n"), QByteArray("typed by hand\n")},
            });
            const auto launched = repo.snapshot();
            QString savedText;
            QString typedText;
            {
                Scene scene(repo);
                checkOffline(scene);
                clickFile(scene, "saved.txt");
                appendToResult(scene, "kept");
                scene.click("saveDraft");
                settleFor("saved draft", [&] { return scene.progress().value("saved").toBool(); });
                savedText = scene.text("resultText");
                clickFile(scene, "typed.txt");
                appendToResult(scene, "typed");
                typedText = scene.text("resultText");
                // Closing with work outstanding opens the handoff; leave is deliberate.
                auto *dialog = scene.object("handoffDialog");
                scene.window->close();
                settleFor("handoff opened by close", [&] { return dialog->property("opened").toBool(); });
                CHECK(scene.window->isVisible());
                scene.click("handoffLeave");
                CHECK(!scene.controller->sessionOutcome().isEmpty());
                checkOffline(scene);
            }
            CHECK(repo.snapshot() == launched);
            Scene reopened(repo);
            expectContains(reopened.controller->loader()->detail(), "Recovered app drafts", "recovery detail");
            clickFile(reopened, "saved.txt");
            expectText(reopened.text("resultText"), savedText, "restored saved draft");
            CHECK(reopened.progress().value("saved").toBool() && !reopened.progress().value("resolved").toBool());
            clickFile(reopened, "typed.txt");
            expectText(reopened.text("resultText"), typedText, "restored unsaved draft");
            CHECK(!reopened.progress().value("saved").toBool() && !reopened.progress().value("resolved").toBool());
            // Restored work stays editable offline.
            appendToResult(reopened, "!");
            expectText(reopened.text("resultText"), typedText + "!", "edit after resume");
            CHECK(!repo.unmerged("saved.txt").isEmpty() && !repo.unmerged("typed.txt").isEmpty());
            checkOffline(reopened);
        });
        handoff.run("a blocked startup lists each recovery session and restores the chosen one in the window", [] {
            Repository repo("merge", {{"shared.txt", QByteArray("shared base\n"), QByteArray("shared left\n"),
                                       QByteArray("shared right\n"), QByteArray("shared chosen by hand\n")}});
            const auto id = fileId("shared.txt").toStdString();
            const auto edit = [&](ApplicationController &controller, const std::string &text) {
                const auto *file = controller.files()->file(id);
                CHECK(file && controller.files()->editResult(id, file->current().id, {{0, 0}, text}) == Transition::Applied);
            };
            QString first;
            QString second;
            {
                // Two windows on one merge each keep their own edits.
                Scene one(repo);
                ApplicationController two(gitOnPath());
                CHECK(two.loader()->openRepository(repo.root));
                // The load runs Git and the recovery restore off the GUI thread, so it gets the
                // deadline of a process, not the layout deadline of settleFor.
                waitFor("second window", [&] { return !two.loader()->opening(); },
                        [&] { return loaderState(*two.loader()); });
                CHECK(two.loader()->startupState() == QStringLiteral("ready"));
                edit(*one.controller, "first window\n");
                edit(two, "second window\n");
                first = one.controller->files()->recoveryDirectory();
                second = two.files()->recoveryDirectory();
            }
            const auto firstName = QFileInfo(first).fileName();
            const auto secondName = QFileInfo(second).fileName();
            Scene scene(repo);
            expectContains(scene.controller->loader()->detail(), "restoration is blocked rather than guessing", "blocked detail");
            CHECK(scene.text("resultText").startsWith(QStringLiteral("shared chosen by hand")));
            for (const auto &name : {firstName, secondName}) {
                auto *row = test_support::waitForLayout(scene.window, "recoveryChoiceText-" + name);
                const auto shown = row->property("text").toString();
                expectContains(shown, name, "choice session");
                expectContains(shown, "last changed", "choice time");
                expectContains(shown, "Edited files: shared.txt", "choice files");
            }
            const auto repository = repo.snapshot();
            scene.click(("recoveryChoice-" + firstName).toLatin1().constData());
            waitFor("chosen session restored", [&] {
                return !scene.controller->loader()->opening() && scene.controller->loader()->recoveryChoices().isEmpty();
            }, [&] { return loaderState(*scene.controller->loader()); });
            CHECK(scene.controller->loader()->startupState() == QStringLiteral("ready"));
            CHECK(scene.controller->files()->recoveryDirectory() == first);
            expectContains(scene.controller->loader()->detail(), secondName + " was set aside, not deleted", "set aside");
            settleFor("restored text", [&] { return scene.text("resultText").startsWith(QStringLiteral("first window\n")); });
            CHECK(!scene.item("recoveryChoices")->isVisible());
            CHECK(QFileInfo::exists(second + "/set-aside") && QFileInfo::exists(second + "/journal"));
            // Choosing touched only app-owned recovery files.
            CHECK(repo.snapshot() == repository && !repo.unmerged("shared.txt").isEmpty());
            checkOffline(scene);
        });
        handoff.run("a resolved mergetool handoff drops the waiting text and a path Git has not launched keeps it", [] {
            Repository repo("merge", {
                {"launched.txt", QByteArray("launched base\n"), QByteArray("launched left\n"),
                 QByteArray("launched right\n"), QByteArray("launched chosen by hand\n")},
                {"later.txt", QByteArray("later base\n"), QByteArray("later left\n"), QByteArray("later right\n"),
                 QByteArray("later chosen by hand\n")},
            });
            Scene scene(repo);
            const QString waitText = QStringLiteral("Waiting for Git to launch this path. It is not resolved.");
            const auto shownTexts = [&] {
                QStringList texts;
                collectTexts(scene.window->contentItem(), texts);
                return texts.join('\n');
            };
            const auto state = [&] {
                return "waiting=" + scene.controller->mergetool()->waitingPath().toStdString() + " staging="
                    + scene.controller->parentStagingPath().toStdString() + " message="
                    + scene.controller->commandMessage().toStdString();
            };
            auto parent = startMergetool(repo, {"launched.txt"});
            waitFor("Git to launch launched.txt",
                    [&] { return scene.controller->mergetool()->waitingPath() == QByteArray("launched.txt"); }, state);
            CHECK(scene.controller->parentSessionActive());

            // A path Git has not launched yet says so while Git is still running.
            clickFile(scene, "later.txt");
            settleFor("waiting text on the unlaunched path", [&] { return scene.item("mergetoolWait")->isVisible(); });
            expectText(scene.text("mergetoolWait"), waitText, "unlaunched path");
            clickFile(scene, "launched.txt");
            settleFor("holding text on the launched path", [&] { return scene.item("mergetoolHolding")->isVisible(); });
            CHECK(!scene.item("mergetoolWait")->isVisible());

            markResolvedByMouse(scene);
            waitFor("git mergetool to exit", [&] { return parent->state() == QProcess::NotRunning; }, state);
            CHECK(parent->exitStatus() == QProcess::NormalExit && parent->exitCode() == 0);
            CHECK(repo.unmerged("launched.txt").isEmpty() && !repo.unmerged("later.txt").isEmpty());
            waitFor("the staged resolution", [&] {
                return scene.progress().value("resolved").toBool() && scene.controller->parentStagingPath().isEmpty();
            }, state);
            CHECK(scene.controller->files()->resolvedFiles() == 1);

            // Git has exited and the parent session stays open until the user ends it.
            CHECK(scene.controller->parentSessionActive() && !scene.controller->mergetoolWaiting());
            CHECK(!scene.item("mergetoolWait")->isVisible());
            const auto resolvedTexts = shownTexts();
            if (resolvedTexts.contains(QStringLiteral("It is not resolved"))
                || resolvedTexts.contains(QStringLiteral("Waiting for Git to launch")))
                throw std::runtime_error("resolved file still says it waits: " + resolvedTexts.toStdString());
            expectContains(rowText(scene, "launched.txt"), "Resolved", "resolved file status");
            // Editing after the handoff leaves an earlier result staged, and Git will not launch the path again.
            appendToResult(scene, "x");
            settleFor("earlier staged result", [&] { return scene.progress().value("earlierResolved").toBool(); });
            CHECK(!scene.item("mergetoolWait")->isVisible());

            // The file Git never launched keeps the waiting text after the handoff.
            clickFile(scene, "later.txt");
            settleFor("waiting text after the handoff", [&] { return scene.item("mergetoolWait")->isVisible(); });
            expectText(scene.text("mergetoolWait"), waitText, "unlaunched path after the handoff");
            CHECK(scene.item("endParentSession")->isVisible());
            checkOffline(scene);
        });
        handoff.run("clicking the launched path again after Git attached shows the waiting state and Leave unresolved", [] {
            Repository repo("merge", {
                {"launched.txt", QByteArray("launched base\n"), QByteArray("launched left\n"),
                 QByteArray("launched right\n"), QByteArray("launched chosen by hand\n")},
                {"other.txt", QByteArray("other base\n"), QByteArray("other left\n"), QByteArray("other right\n"),
                 QByteArray("other chosen by hand\n")},
            });
            Scene scene(repo);
            const auto state = [&] {
                return "waiting=" + scene.controller->mergetool()->waitingPath().toStdString() + " selected="
                    + scene.controller->selectedFileId().toStdString() + " message="
                    + scene.controller->commandMessage().toStdString();
            };
            const auto waitingShown = [&] {
                return scene.item("mergetoolHolding")->isVisible() && scene.item("leaveMergetool")->isVisible()
                    && !scene.item("mergetoolWait")->isVisible();
            };
            clickFile(scene, "launched.txt");
            const auto launchBytes = readFile(repo.path("launched.txt"));
            auto parent = startMergetool(repo, {"launched.txt"});
            waitFor("Git to launch launched.txt",
                    [&] { return scene.controller->mergetool()->waitingPath() == QByteArray("launched.txt"); }, state);
            settleFor("waiting state on the selected launched path", waitingShown);

            // Git is not waiting on the other file, so it has no Leave control.
            clickFile(scene, "other.txt");
            settleFor("waiting text on the other file", [&] { return scene.item("mergetoolWait")->isVisible(); });
            CHECK(!scene.controller->mergetoolWaiting());
            CHECK(!scene.item("mergetoolHolding")->isVisible() && !scene.item("leaveMergetool")->isVisible());

            // Clicking the launched path in the file list again brings both back.
            clickFile(scene, "launched.txt");
            settleFor("waiting state after re-selecting the launched path", waitingShown);
            CHECK(scene.controller->mergetoolWaiting());
            CHECK(scene.controller->mergetool()->waitingPath() == QByteArray("launched.txt"));
            CHECK(scene.item("leaveMergetool")->isEnabled());
            expectText(scene.text("leaveMergetool"), QStringLiteral("Leave unresolved"), "leave control");
            CHECK(parent->state() != QProcess::NotRunning);

            // The control releases Git: MERGED gets its launch bytes back and both paths stay unmerged.
            scene.click("leaveMergetool");
            waitFor("git mergetool to exit", [&] { return parent->state() == QProcess::NotRunning; }, state);
            CHECK(parent->exitStatus() == QProcess::NormalExit && parent->exitCode() != 0);
            CHECK(readFile(repo.path("launched.txt")) == launchBytes);
            CHECK(!repo.unmerged("launched.txt").isEmpty() && !repo.unmerged("other.txt").isEmpty());
            settleFor("leave control gone after leaving", [&] { return !scene.item("leaveMergetool")->isVisible(); });
            checkOffline(scene);
        });
        window.run("a 1280x720 logical screen fits the window with every control and text pane reachable", [] {
            auto paths = journeyPaths();
            paths.push_back({"image.bin", QByteArray("base\0", 5), QByteArray("left\0", 5), QByteArray("side\0", 5), std::nullopt});
            Repository repo("merge", paths);
            Scene scene(repo);
            CHECK(!scene.item("inventoryWarning")->isVisible() && scene.item("outsideAppCount")->isVisible());
            clickFile(scene, "image.bin");
            CHECK(scene.file("image.bin").support() == Support::OutsideApp);
            expectContains(scene.text("fileFacts"), "5 bytes", "captured binary size");
            clickFile(scene, "story.txt");
            expectContains(scene.text("fileFacts"), "UTF-8 · LF", "captured text format");
            for (const auto *name : mainControls) {
                auto *control = test_support::fileOverflowControl(name)
                    ? test_support::fileToolbarControl(scene.window, name, Scene::settleMs) : scene.item(name);
                if (test_support::accessibleName(control).isEmpty())
                    throw std::runtime_error(std::string("no accessible name: ") + name);
                CHECK(!test_support::accessibleName(control).isEmpty());
                if (test_support::fileOverflowControl(name)) test_support::closeFileToolbarMenu(scene.window);
            }
            // Moving between files and the file-wide choices sit in toolbar menus. Once a menu
            // opens, each entry is a visible item with an accessible name.
            const std::pair<const char *, std::vector<const char *>> menus[] = {
                {"navigateMenu", {"previousDifference", "nextDifference", "previousSavedOrReviewed", "nextSavedOrReviewed"}},
                {"sourceChoicesMenu", {"nonConflictingLeft", "nonConflictingRight", "nonConflictingBoth", "wholeFileLeft",
                                       "wholeFileRight"}},
            };
            const auto checkMenus = [&] {
                for (const auto &[menu, entries] : menus) {
                    scene.click(menu);
                    for (const auto *entry : entries) {
                        auto *item = test_support::waitForLayout(scene.window, QString::fromLatin1(entry), Scene::settleMs);
                        if (test_support::accessibleName(item).isEmpty())
                            throw std::runtime_error(std::string("no accessible name: ") + entry);
                        const auto rect = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
                        CHECK(QRectF(0, 0, scene.window->width(), scene.window->height()).contains(rect));
                    }
                    keyClick(scene, Qt::Key_Escape);
                    settleFor(std::string(menu) + " closed", [&] {
                        auto *first = test_support::visualItem(scene.window->contentItem(), QString::fromLatin1(entries.front()));
                        return !first || !first->isVisible();
                    });
                    if (test_support::fileOverflowControl(menu)) test_support::closeFileToolbarMenu(scene.window);
                }
            };
            checkMenus();
            // A roomy window lays out as before, without scrolling the page.
            CHECK(!scene.object("pageScroll")->property("overflowing").toBool());
            const auto lineHeight = scene.item("comparisonView")->property("rowHeight").toReal();
            CHECK(lineHeight > 0);
            // 1920x1080 at 150% scaling. The strict 960x540 fit requirement is
            // temporarily deferred by the owner.
            for (const auto &screen : {QSize(1280, 720), QSize(1280, 800)}) {
                const auto pane = checkFits(scene, screen.width(), screen.height());
                if (pane < 8 * lineHeight)
                    throw std::runtime_error("text panes " + std::to_string(pane) + " px high at "
                                             + std::to_string(screen.width()) + "x" + std::to_string(screen.height())
                                             + ", less than eight " + std::to_string(lineHeight) + " px lines");
                for (const auto *identity : {"stage2Identity", "stage3Identity"})
                    CHECK(!scene.item(identity)->property("truncated").toBool());
                checkMenus();
            }
            // Whether this page is taller than a 960x540 window depends on the fonts. It fits with
            // DejaVu Sans 9 pt, as in the CI images. Either way the wheel moves it only when it does not fit.
            scene.window->resize(960, 540);
            settleFor("window at 960x540", [&] { return scene.window->width() == 960 && scene.window->height() == 540; });
            checkWheelScroll(scene, "selectedPath");

            // at 1280x800 the handoff dialog wraps between words. File rows elide one
            // line each instead of wrapping, so nothing in the file list wraps at all.
            scene.window->resize(1280, 800);
            settleFor("window at 1280x800", [&] { return scene.window->width() == 1280 && scene.window->height() == 800; });
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            CHECK(expectWholeWords(scene.item("fileList"), "the file list at 1280x800") == 0);
            auto *dialog = scene.object("handoffDialog");
            scene.click("openHandoff");
            settleFor("handoff opened", [&] { return dialog->property("opened").toBool(); });
            test_support::waitForLayout(scene.window, QStringLiteral("handoffLeave"), Scene::settleMs);
            // Each blocker has a line of its own, so a row may not need to wrap at all.
            expectWholeWords(dialog->property("contentItem").value<QQuickItem *>(), "the handoff dialog at 1280x800");
            QMetaObject::invokeMethod(dialog, "close");
            settleFor("handoff closed", [&] { return !dialog->property("visible").toBool(); });

            // the source choices start shown, in the source panes' gutters, where they
            // take no height. With them the three panes keep eight lines and the panes and choices
            // stay reachable, from a roomy window down to 1280x720, and hiding them leaves every
            // text pane as high as it was. The owner waiver still excludes strict 960x540 fit.
            settleFor("source choices shown", [&] { return scene.item("scopedChoices")->isVisible(); });
            const std::vector<const char *> withChoices{"showSourceChoices", "sourceChoicesMenu", "undecidedCounter",
                "stage2Label", "resultLabel", "stage3Label", "stage2Viewport", "resultViewport", "stage3Viewport",
                "scopedChoices", "workflowStatus"};
            for (const auto &screen : {QSize(1500, 1040), QSize(1280, 800), QSize(1280, 720)}) {
                const auto size = std::to_string(screen.width()) + "x" + std::to_string(screen.height());
                const auto pane = checkFits(scene, screen.width(), screen.height(), withChoices);
                if (pane < 8 * lineHeight)
                    throw std::runtime_error("with source choices, text panes " + std::to_string(pane) + " px high at "
                                             + size + ", less than eight " + std::to_string(lineHeight) + " px lines");
                clickFileToolbarControl(scene, "showSourceChoices");
                settleFor("source choices hidden at " + size, [&] { return !scene.item("scopedChoices")->isVisible(); });
                const auto hidden = checkFits(scene, screen.width(), screen.height(),
                                              {"stage2Viewport", "resultViewport", "stage3Viewport"});
                if (hidden != pane)
                    throw std::runtime_error("at " + size + " the text panes are " + std::to_string(pane)
                                             + " px high with the source choices and " + std::to_string(hidden)
                                             + " px without them");
                clickFileToolbarControl(scene, "showSourceChoices");
                settleFor("source choices shown at " + size, [&] { return scene.item("scopedChoices")->isVisible(); });
            }
            checkOffline(scene);
        });
        window.run("selecting a file shows its first undecided region in all three panes", [] {
            QByteArray before;
            QByteArray after;
            for (int line = 1; line <= 120; ++line) before += "shared before " + QByteArray::number(line) + "\n";
            for (int line = 1; line <= 80; ++line) after += "shared after " + QByteArray::number(line) + "\n";
            // Git's conflict output stays the launch result, so the conflict starts at line 121.
            Repository repo("merge", {{"long.txt", before + "base value\n" + after, before + "left value\n" + after,
                                       before + "right value\n" + after, std::nullopt},
                                      {"short.txt", QByteArray("short base\n"), QByteArray("short left\n"),
                                       QByteArray("short right\n"), std::nullopt}});
            Scene scene(repo);
            clickFile(scene, "long.txt");
            waitFor("the first undecided region in every pane at launch", [&] { return firstUndecidedInView(scene).empty(); },
                    [&] { return firstUndecidedInView(scene); });
            const auto lineHeight = scene.item("comparisonView")->property("rowHeight").toReal();
            CHECK(lineHeight > 0);
            for (const auto &screen : {QSize(1280, 720), QSize(960, 540)}) {
                const auto size = std::to_string(screen.width()) + "x" + std::to_string(screen.height());
                const auto pane = checkFits(scene, screen.width(), screen.height(),
                                            {"fileList", "stage2Viewport", "resultViewport", "stage3Viewport"});
                if (pane < 8 * lineHeight)
                    throw std::runtime_error("text panes " + std::to_string(pane) + " px high at " + size
                                             + ", less than eight " + std::to_string(lineHeight) + " px lines");
                // The file rows sit at the top of the page. The pointer still rests on long.txt, whose
                // tooltip opens below it over short.txt under the KDE style; clickFile moves and
                // presses at once, so the tooltip must close before the press.
                scene.object("pageScroll")->setProperty("contentY", 0);
                clickFile(scene, "short.txt");
                clickFile(scene, "long.txt");
                waitFor("the first undecided region in every pane at " + size,
                        [&] { return firstUndecidedInView(scene).empty(); }, [&] { return firstUndecidedInView(scene); });
            }
            checkOffline(scene);
        });
        window.run("a choice clears hand edits on a line deleted in the editor and on text an edit left unchanged", [] {
            // The left side changes two lines in one section, and the user set the result to it before launch.
            const QByteArray left = "header\nleft first\nleft second\nbridge\nleft later\ntail\n";
            Repository repo("merge", {{"pair.txt", QByteArray("header\nbase first\nbase second\nbridge\nbase later\ntail\n"),
                                       left, QByteArray("header\nright first\nright second\nbridge\nright later\ntail\n"),
                                       left}});
            Scene scene(repo);
            clickFile(scene, "pair.txt");
            const auto region = [&](const char *label) { return regionFor(scene, QString::fromUtf8(label)); };
            const auto edited = [&](const char *label) {
                return region(label).value(QStringLiteral("handEdited")).toBool();
            };
            const auto noneEdited = [&] {
                const auto regions = scene.controller->selectedFile()->regions();
                return std::none_of(regions.begin(), regions.end(), [](const QVariant &value) {
                    return value.toMap().value(QStringLiteral("handEdited")).toBool();
                });
            };
            auto *editor = scene.item("resultText");
            const auto toLine = [&](int line) {
                clickText(scene, editor);
                CHECK(editor->hasActiveFocus());
                keyClick(scene, Qt::Key_Home, Qt::ControlModifier);
                for (int step = 1; step < line; ++step) keyClick(scene, Qt::Key_Down);
            };

            // Deleting a line of the section in the editor leaves its region marked but without a result range.
            toLine(2);
            keyClick(scene, Qt::Key_Down, Qt::ShiftModifier);
            keyClick(scene, Qt::Key_Delete);
            expectText(scene.text("resultText"), "header\nleft second\nbridge\nleft later\ntail\n", "deleted line");
            CHECK(edited("Left line 2") && !region("Left line 2").value(QStringLiteral("mapped")).toBool());
            const auto deleted = work(scene, "pair.txt");
            // Accepting the section around it puts the line back and clears the mark.
            clickChoice(scene, QString::fromUtf8("Left section, lines 2–3"), "Accept");
            CHECK(scene.item("manualReplacementNotice")->isVisible());
            clickDialogButton(scene, "replacementDialog", "Apply choice");
            expectText(scene.text("resultText"), utf8(left), "restored line");
            CHECK(noneEdited());
            CHECK(region("Left line 2").value(QStringLiteral("state")).toString() == QStringLiteral("accepted"));
            const auto restored = work(scene, "pair.txt");
            scene.click("undoCommand");
            CHECK(work(scene, "pair.txt") == deleted && edited("Left line 2"));
            scene.click("redoCommand");
            CHECK(work(scene, "pair.txt") == restored && noneEdited());

            // Typing a character and deleting it again leaves the text as the whole left side but marks it.
            const auto wholeLeft = [&] {
                scene.click("sourceChoicesMenu");
                test_support::clickItem(scene.window,
                    test_support::waitForLayout(scene.window, QStringLiteral("wholeFileLeft"), Scene::settleMs));
                clickDialogButton(scene, "wholeFileDialog", "Replace whole file");
            };
            wholeLeft();
            expectText(scene.text("resultText"), utf8(left), "whole left");
            CHECK(noneEdited());
            toLine(2);
            keyClick(scene, Qt::Key_End);
            typeText(scene, "X");
            keyClick(scene, Qt::Key_Backspace);
            expectText(scene.text("resultText"), utf8(left), "typed and deleted");
            CHECK(!noneEdited());
            const auto manual = work(scene, "pair.txt");
            const auto manualRegions = scene.controller->selectedFile()->regions();
            // The same whole-file choice keeps the text and decisions but replaces the edit.
            wholeLeft();
            expectText(scene.text("resultText"), utf8(left), "whole left again");
            CHECK(noneEdited() && scene.file("pair.txt").current().draft == manual.draft);
            scene.click("undoCommand");
            CHECK(work(scene, "pair.txt") == manual);
            CHECK(scene.controller->selectedFile()->regions() == manualRegions);
            scene.click("redoCommand");
            CHECK(noneEdited());
            checkOffline(scene);
        });
        window.run("lines from both sides of one multi-line conflict keep their text, decisions and undo", [] {
            // Both sides change the same three lines, and the user set the result to the common version before launch.
            const QByteArray base = "header\nbase one\nbase two\nbase three\ntail\n";
            Repository repo("merge", {{"lines.txt", base, QByteArray("header\nleft one\nleft two\nleft three\ntail\n"),
                                       QByteArray("header\nright one\nright two\nright three\ntail\n"), base}});
            Scene scene(repo);
            clickFile(scene, "lines.txt");
            const auto state = [&](const char *label) {
                return choiceRow(scene, QString::fromUtf8(label)).value(QStringLiteral("state")).toString();
            };
            const auto states = [&](const char *expected) {
                QStringList actual;
                for (const auto *label : {"Left line 2", "Left line 3", "Left line 4", "Right line 2", "Right line 3", "Right line 4"})
                    actual.push_back(state(label));
                expectText(actual.join(QLatin1Char(' ')), QString::fromUtf8(expected), "line decisions");
            };

            // Every line inside the conflict can be chosen, at the result line with the same index.
            for (const auto *label : {"Left line 2", "Left line 3", "Left line 4", "Right line 2", "Right line 3", "Right line 4"}) {
                const auto row = choiceRow(scene, QString::fromUtf8(label));
                CHECK(row.value(QStringLiteral("available")).toBool());
                CHECK(row.value(QStringLiteral("detail")).toString().isEmpty());
            }
            expectText(regionResult(scene, regionFor(scene, QStringLiteral("Left line 3"))), "base two\n", "left line 3 target");
            expectText(regionResult(scene, regionFor(scene, QStringLiteral("Right line 4"))), "base three\n", "right line 4 target");
            states("undecided undecided undecided undecided undecided undecided");
            std::vector<Work> history{work(scene, "lines.txt")};

            // The middle line from the left replaces only the launch line at its position.
            clickChoice(scene, "Left line 3", "Accept");
            CHECK(scene.item("manualReplacementNotice")->isVisible());
            expectText(scene.text("replacementBeforeText"), "base two\n", "replaced left target");
            expectText(scene.text("replacementAfterText"), "left two\n", "left replacement");
            clickDialogButton(scene, "replacementDialog", "Apply choice");
            expectText(scene.text("resultText"), "header\nbase one\nleft two\nbase three\ntail\n", "accept left line 3");
            states("undecided accepted undecided undecided undecided undecided");
            history.push_back(work(scene, "lines.txt"));

            // The last line from the right goes beside it in the same conflict.
            clickChoice(scene, "Right line 4", "Accept");
            expectText(scene.text("replacementBeforeText"), "base three\n", "replaced right target");
            expectText(scene.text("replacementAfterText"), "right three\n", "right replacement");
            clickDialogButton(scene, "replacementDialog", "Apply choice");
            expectText(scene.text("resultText"), "header\nbase one\nleft two\nright three\ntail\n", "accept right line 4");
            states("undecided accepted undecided undecided undecided accepted");
            history.push_back(work(scene, "lines.txt"));

            // Ignore decides against the first left line and keeps the text.
            clickChoice(scene, "Left line 2", "Ignore");
            CHECK(!scene.object("replacementDialog")->property("visible").toBool());
            expectText(scene.text("resultText"), "header\nbase one\nleft two\nright three\ntail\n", "ignore left line 2");
            states("ignored accepted undecided undecided undecided accepted");
            history.push_back(work(scene, "lines.txt"));

            // Undo walks back through text and decisions together, and redo replays them.
            for (std::size_t step = history.size() - 1; step-- > 0;) {
                scene.click("undoCommand");
                CHECK(work(scene, "lines.txt") == history[step]);
            }
            states("undecided undecided undecided undecided undecided undecided");
            CHECK(!scene.file("lines.txt").canUndo() && !scene.item("undoCommand")->isEnabled());
            for (std::size_t step = 1; step < history.size(); ++step) {
                scene.click("redoCommand");
                CHECK(work(scene, "lines.txt") == history[step]);
            }
            expectText(scene.text("resultText"), "header\nbase one\nleft two\nright three\ntail\n", "redone");
            states("ignored accepted undecided undecided undecided accepted");
            checkOffline(scene);
        });
        window.run("closing with input recovery could not keep asks to retry, and cancel keeps the window open", [] {
            Repository repo("merge", {{"kept.txt", QByteArray("kept base\n"), QByteArray("kept left\n"),
                                       QByteArray("kept right\n"), QByteArray("kept by hand\n")}});
            const auto launched = repo.snapshot();
            {
                Scene scene(repo);
                clickFile(scene, "kept.txt");
                auto &files = *scene.controller->files();
                auto *prompt = scene.object("recoveryCloseDialog");
                auto *handoff = scene.object("handoffDialog");
                const auto closeWindow = [&] {
                    scene.window->close();
                    settleFor("close guard opened", [&] { return prompt->property("opened").toBool(); });
                    CHECK(scene.window->isVisible() && !handoff->property("visible").toBool());
                };
                {
                    // Another holder of the journal lock makes the recovery commit fail, so the typed input
                    // stays pending.
                    QLockFile busy(files.recoveryDirectory() + QStringLiteral("/journal.lock"));
                    CHECK(busy.tryLock(0));
                    appendToResult(scene, "!");
                    settleFor("pending recovery", [&] { return files.hasPendingRecovery(); });
                    CHECK(scene.item("recoveryProblem")->isVisible());
                    closeWindow();
                    expectContains(prompt->property("subtitle").toString(), "Retry before closing", "close guard");
                    clickDialogButton(scene, "recoveryCloseDialog", "Cancel");
                    CHECK(scene.window->isVisible() && files.hasPendingRecovery() && !handoff->property("visible").toBool());
                    // Retrying while the journal is still busy keeps the input pending and the window open.
                    closeWindow();
                    clickDialogButton(scene, "recoveryCloseDialog", "Retry");
                    CHECK(scene.window->isVisible() && files.hasPendingRecovery() && !handoff->property("visible").toBool());
                    closeWindow();
                }
                // With the journal free, Retry keeps the input and the close goes on to the handoff.
                clickDialogButton(scene, "recoveryCloseDialog", "Retry");
                settleFor("handoff opened after retry", [&] { return handoff->property("opened").toBool(); });
                CHECK(scene.window->isVisible() && !files.hasPendingRecovery());
                expectText(QString::fromStdString(scene.file("kept.txt").current().draft.text), "kept by hand\n!", "kept input");
                scene.click("handoffLeave");
                CHECK(!scene.controller->sessionOutcome().isEmpty());
                checkOffline(scene);
            }
            CHECK(repo.snapshot() == launched);
            // The retried input is what a new window restores.
            Scene reopened(repo);
            clickFile(reopened, "kept.txt");
            expectText(reopened.text("resultText"), "kept by hand\n!", "restored input");
            checkOffline(reopened);
        });
        return results.report();
    }
};

int main(int argc, char **argv)
{
    // Child mode: git mergetool starts this binary as the real launcher.
    if (argc == 2 && std::strcmp(argv[1], "--mergetool") == 0) return runMergetoolLauncher(argc, argv, gitOnPath());
    Sandbox shared;
    if (!shared.root.isValid() || !shared.runtime.isValid()) return 2;
    sandbox = &shared;
    const auto base = shared.root.path();
    for (const auto *name : {"/home", "/config", "/cache", "/data", "/bin"})
        if (!QDir().mkpath(base + name)) return 2;
    // A private runtime directory keeps the mergetool socket out of the real one. The
    // app trusts a Git executable only in directories that other users cannot write.
    for (const auto &path : {shared.runtime.path(), base + "/bin"})
        if (!QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
            return 2;
    shared.bin = base + "/bin";
    shared.argvLog = base + "/git-argv";
    shared.networkMarker = base + "/network-attempt";
    const auto log = quoteShell(QFile::encodeName(shared.argvLog));
    const auto marker = quoteShell(QFile::encodeName(shared.networkMarker));
    {
        QFile guard(shared.bin + "/git");
        if (!guard.open(QIODevice::WriteOnly)) return 2;
        guard.write("#!/bin/sh\nfor argument in \"$@\"; do\n"
            "  case \"$argument\" in fetch|pull|push|clone|ls-remote|remote|submodule|credential) : > " + marker
            + "; exit 97 ;; esac\ndone\nprintf '%s\\n' \"$@\" >> " + log + "\nexec /usr/bin/git \"$@\"\n");
    }
    if (!QFile::setPermissions(shared.bin + "/git", QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
        return 2;
    qputenv("HOME", (base + "/home").toUtf8());
    qputenv("XDG_CONFIG_HOME", (base + "/config").toUtf8());
    qputenv("XDG_CACHE_HOME", (base + "/cache").toUtf8());
    qputenv("XDG_DATA_HOME", (base + "/data").toUtf8());
    qputenv("XDG_RUNTIME_DIR", shared.runtime.path().toUtf8());
    qputenv("PATH", (shared.bin + ":/usr/bin:/bin").toUtf8());
    qputenv("LC_ALL", "C.UTF-8");
    qputenv("GIT_CONFIG_NOSYSTEM", "1");
    qputenv("GIT_CONFIG_GLOBAL", "/dev/null");
    qputenv("GIT_TERMINAL_PROMPT", "0");
    // Any HTTP attempt goes to a closed local port instead of the network.
    for (const auto *name : {"http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY", "all_proxy", "ALL_PROXY"})
        qputenv(name, "http://127.0.0.1:9");
    for (const auto *name : {"GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY", "no_proxy", "NO_PROXY"})
        qunsetenv(name);
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName("BranchTruceJourneyAcceptance");
    // The suite owns its clipboard and window system; physical desktop input is out of scope.
    if (QGuiApplication::platformName() != QStringLiteral("offscreen")) return 2;
    // No argument runs every journey; "review", "handoff" and "window" select one ctest entry's share.
    const auto arguments = QCoreApplication::arguments();
    auto selection = Selection::All;
    if (arguments.size() == 2 && arguments.at(1) == QStringLiteral("review")) selection = Selection::Review;
    else if (arguments.size() == 2 && arguments.at(1) == QStringLiteral("handoff")) selection = Selection::Handoff;
    else if (arguments.size() == 2 && arguments.at(1) == QStringLiteral("window")) selection = Selection::Window;
    else if (arguments.size() != 1) return 2;
    const auto result = JourneyAcceptanceTests{}.run(selection);
    QGuiApplication::clipboard()->clear();
    return result;
}
