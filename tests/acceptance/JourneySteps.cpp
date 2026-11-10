// The steps journeys take through the window and the checks they share.
#include "acceptance/JourneySteps.h"

#include "fixtures/Files.h"
#include "fixtures/QuickScene.h"

#include <QAccessible>
#include <QCoreApplication>
#include <QFile>
#include <QFont>
#include <QGuiApplication>
#include <QQuickTextDocument>
#include <QStandardPaths>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

using namespace branch_truce;
using test_support::settleFor;

namespace journey {
namespace {

// The section owns all of its precise lines, including lines after its first row.
QString rowOwner(Scene &scene, const Region &region)
{
    if (region.scope == QStringLiteral("section")) return region.id;
    for (const auto &other : scene.regions()) {
        if (other.scope == QStringLiteral("section") && other.side == region.side && other.section == region.section
            && other.source.lineBegin <= region.source.lineBegin && other.source.lineEnd >= region.source.lineEnd)
            return other.id;
    }
    return region.id;
}

// The source pane's state and every visible control, for a choice entry that did not appear.
std::string choiceMenuState(Scene &scene, const std::string &side)
{
    auto *window = scene.window();
    auto *editor = scene.item((side + "Text").c_str());
    std::string menus;
    const auto visit = [&](auto &&self, QQuickItem *item) -> void {
        if (!item->isVisible()) return;
        if (item->inherits("QQuickAbstractButton"))
            menus +=
                " [" + item->objectName().toStdString() + "=" + test_support::accessibleName(item).toStdString() + "]";
        for (auto *child : item->childItems()) self(self, child);
    };
    visit(visit, window->contentItem());
    for (auto *other : QGuiApplication::topLevelWindows()) {
        if (other == window || !other->isVisible()) continue;
        menus +=
            " [window=" + std::string(other->metaObject()->className()) + " " + other->objectName().toStdString() + "]";
        if (auto *quick = qobject_cast<QQuickWindow *>(other)) visit(visit, quick->contentItem());
    }
    for (auto *popup : window->findChildren<QObject *>()) {
        if (!popup->objectName().startsWith(QStringLiteral("choiceMore-"))
            && !popup->objectName().endsWith(QStringLiteral("ContextMenu")))
            continue;
        menus += " [popup=" + popup->objectName().toStdString()
            + " visible=" + std::to_string(popup->property("visible").toBool())
            + " opened=" + std::to_string(popup->property("opened").toBool())
            + " type=" + std::to_string(popup->property("popupType").toInt()) + "]";
    }
    return "source=" + side + " cursor=" + std::to_string(editor->property("cursorPosition").toInt())
        + " focus=" + std::to_string(editor->hasActiveFocus())
        + " selection=" + std::to_string(editor->property("selectionStart").toInt()) + ".."
        + std::to_string(editor->property("selectionEnd").toInt()) + "; visible controls:" + menus;
}
} // namespace

QQuickItem *stable(Scene &scene, const std::string &what, const std::function<QQuickItem *()> &find)
{
    return test_support::waitForStable(scene.window(), what, find, Scene::settleMs);
}

QQuickItem *descendant(QQuickItem *root, const std::function<bool(QQuickItem *)> &match)
{
    if (!root) return nullptr;
    if (match(root)) return root;
    for (auto *child : root->childItems())
        if (auto *found = descendant(child, match)) return found;
    return nullptr;
}

QPointF sceneOrigin(QQuickItem *item) { return item->mapToScene(QPointF(0, 0)); }

void collectTexts(QQuickItem *root, QStringList &texts)
{
    if (!root->isVisible()) return;
    const auto value = root->property("text");
    if (value.isValid() && !value.toString().isEmpty()) texts << value.toString();
    for (auto *child : root->childItems()) collectTexts(child, texts);
}

QString shownTexts(Scene &scene)
{
    QStringList texts;
    collectTexts(scene.window()->contentItem(), texts);
    return texts.join('\n');
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
    if (const auto *face = QAccessible::queryAccessibleInterface(row)) texts << face->text(QAccessible::Description);
    return texts.join('\n');
}

Work work(Scene &scene, const char *path) { return {scene.text("resultText"), scene.file(path).current().draft}; }

QString regionResult(Scene &scene, const Region &region)
{
    const auto begin = region.result.utf16Begin;
    const auto end = region.result.utf16End;
    if (begin < 0 || end < begin) throw std::runtime_error("region result range is unmapped");
    return scene.text("resultText").mid(begin, end - begin);
}

QString utf8(const QByteArray &bytes) { return QString::fromUtf8(bytes); }

void clickChoice(Scene &scene, const QString &label, const char *kind, int menuKey)
{
    // Accept and Ignore of the scope that owns a range are buttons on its first row; every other
    // choice is an entry of the row's context menu, choiceMore-<owner ID>, opened with the
    // requested source line's public context-menu gesture.
    auto *lanes = scene.item("scopedChoices");
    const auto region = scene.regionFor(label);
    const auto owner = rowOwner(scene, region);
    const auto name = QStringLiteral("choice") + QLatin1String(kind) + QLatin1Char('-') + region.id;
    const bool onRow = region.scope == QStringLiteral("section") && owner == region.id
        && (qstrcmp(kind, "Accept") == 0 || qstrcmp(kind, "Ignore") == 0);
    if (auto *target = onRow ? test_support::hoverChoiceControl(scene.window(), lanes, name) : nullptr) {
        test_support::clickItem(scene.window(), target);
        return;
    }
    const auto side = region.side.toStdString();
    test_support::sourceLineMenu(scene.window(), scene.item((side + "Text").c_str()),
                                 scene.item((side + "Viewport").c_str()), region.source.lineBegin, menuKey);
    QQuickItem *entry = nullptr;
    try {
        entry = stable(scene, (label + " " + name).toStdString(), [&] {
            return test_support::reachableButton(scene.window(), scene.window()->contentItem(), name);
        });
    } catch (const std::exception &error) {
        throw std::runtime_error(std::string(error.what()) + "; " + choiceMenuState(scene, side));
    }
    test_support::clickItem(scene.window(), entry);
}

QQuickItem *dialogButton(Scene &scene, const char *dialog, const QString &button)
{
    auto *popup = scene.object(dialog);
    settleFor(std::string(dialog) + " opened", [&] { return popup->property("opened").toBool(); });
    // The whole dialog is searched, so a Kirigami.Dialog footer works too.
    auto *root = test_support::popupRoot(popup);
    return stable(scene, std::string(dialog) + " " + button.toStdString(),
                  [&] { return test_support::namedButton(root, button); });
}

void clickDialogButton(Scene &scene, const char *dialog, const QString &button)
{
    test_support::clickItem(scene.window(), dialogButton(scene, dialog, button));
    auto *popup = scene.object(dialog);
    settleFor(std::string(dialog) + " closed", [&] { return !popup->property("visible").toBool(); });
}

void clickFile(Scene &scene, const char *path)
{
    const auto id = fileId(path);
    auto *list = scene.item("fileList");
    auto *files = scene.app().files();
    int row = -1;
    for (int i = 0; i < files->rowCount(); ++i)
        if (files->data(files->index(i), SessionModel::FileIdRole).toString() == id) row = i;
    CHECK(row >= 0);
    constexpr int contain = 4; // ListView.Contain
    CHECK(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, row), Q_ARG(int, contain)));
    auto *target = stable(scene, std::string("file row ") + path, [&] {
        return descendant(list, [&](QQuickItem *candidate) {
            return candidate->inherits("QQuickAbstractButton") && candidate->property("fileId").toString() == id;
        });
    });
    test_support::clickItem(scene.window(), target);
    settleFor(std::string("selection of ") + path, [&] { return scene.app().selection()->fileId() == id; });
}

void keyClick(Scene &scene, Qt::Key key, Qt::KeyboardModifiers modifiers)
{
    QTest::keyClick(scene.window(), key, modifiers);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

void typeText(Scene &scene, const char *text)
{
    // QTest::keyClicks has no QWindow overload.
    for (const char *c = text; *c; ++c) QTest::keyClick(scene.window(), *c);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

void clickText(Scene &scene, QQuickItem *editor)
{
    CHECK(editor->isVisible() && editor->width() > 20 && editor->height() > 8);
    const auto point = editor->mapToScene(QPointF(20, 8)).toPoint();
    QTest::mouseClick(scene.window(), Qt::LeftButton, Qt::NoModifier, point);
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

void markResolvedByMouse(Scene &scene)
{
    auto &app = scene.app();
    scene.click("markResolved");
    settleFor("resolution confirmation", [&] { return app.commands()->resolutionConfirmation(); });
    scene.click("confirmResolution");
    waitFor(
        "the resolution of " + scene.text("selectedPath").toStdString(),
        [&] { return !app.commands()->resolutionBusy() && !app.commands()->resolutionConfirmation(); },
        [&] {
            return "busy=" + std::to_string(app.commands()->resolutionBusy())
                + " confirmation=" + std::to_string(app.commands()->resolutionConfirmation()) + " status="
                + app.selectedFile()->status().toStdString() + " message=" + app.feedback()->message().toStdString();
        });
}

void clickOverflowAction(Scene &scene, const char *name)
{
    auto *toolbar = scene.item("fileToolBar");
    test_support::clickItem(scene.window(), stable(scene, "file toolbar overflow", [&] {
                                return test_support::visualButton(toolbar, [](QQuickItem *button) {
                                    return test_support::accessibleName(button) == QStringLiteral("More Actions");
                                });
                            }));
    auto *action = scene.object(name);
    test_support::clickItem(scene.window(), stable(scene, std::string(name) + " entry", [&] {
                                return test_support::visualButton(
                                    scene.window()->contentItem(), [&](QQuickItem *entry) {
                                        return entry->property("action").value<QObject *>() == action;
                                    });
                            }));
}

void clickFileToolbarControl(Scene &scene, const char *name)
{
    test_support::clickItem(scene.window(), test_support::fileToolbarControl(scene.window(), name, Scene::settleMs));
}

void toggleConflictMarkers(Scene &scene)
{
    clickOverflowAction(scene, "showConflictMarkers");
    test_support::closeFileToolbarMenu(scene.window());
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
        throw std::runtime_error(std::string(what) + ": [" + actual.toStdString() + "] lacks [" + expected.toStdString()
                                 + "]");
    CHECK(actual.contains(expected));
}

void checkOffline(Scene &scene)
{
    CHECK(!scene.sandbox().git().networkAttempted());
    const auto trace = scene.gitCalls();
    CHECK(trace.contains("rev-parse\n") && trace.contains("cat-file\n"));
    QStringList texts;
    collectTexts(scene.window()->contentItem(), texts);
    CHECK(!texts.isEmpty());
    for (const auto &text : texts) {
        for (const auto *word : {"sign in", "sign-in", "log in", "login", "account", "subscription", "payment",
                                 "license key", "register"}) {
            if (text.contains(QLatin1String(word), Qt::CaseInsensitive))
                throw std::runtime_error("account prompt in scene text: " + text.toStdString());
        }
    }
}

void expectMarkersFolded(Scene &scene, bool folded)
{
    // The folded rows layer draws a row only while the blocks are folded. The line numbers are
    // the buffer's, so the line after a folded block keeps its own number.
    const auto *document =
        scene.item("resultText")->property("textDocument").value<QQuickTextDocument *>()->textDocument();
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

QJsonObject inspectReadOnlyPane(Scene &scene, const char *name, const char *header)
{
    auto *editor = scene.item(name);
    auto *label = scene.item(header);
    auto *main = scene.item("resultText");
    const auto advance = [](QQuickItem *text) {
        auto *quick = text->property("textDocument").value<QQuickTextDocument *>();
        CHECK(quick && quick->textDocument()->begin().next().isValid());
        QRectF first, second;
        CHECK(QMetaObject::invokeMethod(text, "positionToRectangle", Q_RETURN_ARG(QRectF, first), Q_ARG(int, 0)));
        const auto next = quick->textDocument()->begin().next().position();
        CHECK(QMetaObject::invokeMethod(text, "positionToRectangle", Q_RETURN_ARG(QRectF, second), Q_ARG(int, next)));
        return second.top() - first.top();
    };
    settleFor(std::string(name) + " shared native font and row geometry", [&] {
        return editor->property("font").value<QFont>() == main->property("font").value<QFont>() && advance(editor) > 0
            && std::abs(advance(editor) - advance(main)) < 0.5;
    });
    CHECK(editor->property("readOnly").toBool());
    CHECK(editor->property("textFormat").toInt() == 0);
    CHECK(label->property("textFormat").toInt() == 0 && !label->property("text").toString().isEmpty());
    const auto original = editor->property("text").toString();
    CHECK(!original.isEmpty());
    const int selected = static_cast<int>(std::min<qsizetype>(5, original.size()));
    editor->forceActiveFocus(Qt::TabFocusReason);
    settleFor(std::string(name) + " native focus", [&] { return editor->hasActiveFocus(); });
    QTest::keyClick(scene.window(), Qt::Key_Home, Qt::ControlModifier);
    for (int index = 0; index < selected; ++index) QTest::keyClick(scene.window(), Qt::Key_Right, Qt::ShiftModifier);
    CHECK(editor->property("selectedText").toString() == original.left(selected));
    QTest::keyClick(scene.window(), Qt::Key_X);
    CHECK(editor->property("text").toString() == original);
    CHECK(editor->property("selectedText").toString() == original.left(selected));
    const auto font = editor->property("font").value<QFont>();
    return {{"stage", name},
            {"synthetic_stress", false},
            {"font_family", font.family()},
            {"font_point_size", font.pointSizeF()},
            {"row_advance", advance(editor)},
            {"header", label->property("text").toString()},
            {"read_only", true},
            {"focused_selection", editor->property("selectedText").toString()}};
}

std::string loaderState(const SessionLoader &loader)
{
    return "startup " + QVariant::fromValue(loader.startupState()).toString().toStdString() + ", "
        + std::to_string(loader.recoveryChoices().size()) + " recovery choices: " + loader.detail().toStdString();
}

std::unique_ptr<mergetool_fixture::ParentProcess> startMergetool(const Sandbox &sandbox, const Repository &repo,
                                                                 const QStringList &paths)
{
    repo.git({"config", "mergetool.truce-launcher.cmd",
              QStringLiteral("MERGED=\"$MERGED\" BASE=\"$BASE\" LOCAL=\"$LOCAL\" REMOTE=\"$REMOTE\" ")
                  + test_support::shellQuote(QCoreApplication::applicationFilePath())
                  + QStringLiteral(" --mergetool")});
    repo.git({"config", "mergetool.truce-launcher.trustExitCode", "true"});
    repo.git({"config", "mergetool.keepBackup", "false"});
    const auto setsid = QStandardPaths::findExecutable(QStringLiteral("setsid"));
    if (setsid.isEmpty()) throw std::runtime_error("setsid is not on PATH");
    auto process = std::make_unique<mergetool_fixture::ParentProcess>();
    process->setProcessEnvironment(sandbox.mergetoolEnvironment(repo));
    process->setWorkingDirectory(repo.root());
    // setsid replaces itself with Git when, as here, its caller is not a group leader.
    process->start(setsid,
                   QStringList{"--wait", test_support::gitExecutable(), "--no-pager", "--literal-pathspecs",
                               "mergetool", "--no-prompt", "--tool=truce-launcher", "--"}
                       + paths);
    CHECK(process->waitForStarted(5000));
    return process;
}

ReadOnlyDirectory::ReadOnlyDirectory(QString path) : path_(std::move(path))
{
    CHECK(QFile::setPermissions(path_, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
}

ReadOnlyDirectory::~ReadOnlyDirectory()
{
    QFile::setPermissions(path_, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

} // namespace journey
