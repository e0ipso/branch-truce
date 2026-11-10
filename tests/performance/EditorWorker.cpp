// The editor workload in the real window: the BranchTruce Main scene over an Application whose
// store holds one generated conflict, as a user would see it after selecting the file.
//
//   startup  load Main with no file selected, until its result pane is drawn.
//   load     select the file, until the result pane holds the result text, is laid out and drawn.
//            The correspondence is built before and is measured on its own; a file over
//            kLargeFileLines is confirmed first, as the user would.
//   key      type one letter at the start of the result, until the store has the new revision,
//            the pane has the letter and a frame is drawn.
//
// The product refuses a file over its text envelope. That is reported as an explicit failure:
// the envelope's limits are measured at, not beyond, what the product opens.
#include "app/Application.h"
#include "fixtures/ChoiceFile.h"
#include "fixtures/FixtureGit.h"
#include "fixtures/performance/WorkloadText.h"
#include "performance/Workload.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QKeyEvent>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QTextDocument>
#include <QTimer>

#include <iostream>
#include <sstream>

namespace workload {
namespace {

constexpr int kDrawDeadlineMs = 20000;
const auto kFileId = QStringLiteral("choices");

QQuickItem *findItem(QQuickItem *root, const QString &name)
{
    if (root->objectName() == name) return root;
    for (auto *child : root->childItems())
        if (auto *found = findItem(child, name)) return found;
    return nullptr;
}

QQuickItem *sceneItem(QQuickWindow *window, const QString &name)
{
    if (auto *found = window->findChild<QQuickItem *>(name)) return found;
    return findItem(window->contentItem(), name);
}

QTextDocument *documentOf(QQuickItem *editor)
{
    auto *quick = editor ? editor->property("textDocument").value<QQuickTextDocument *>() : nullptr;
    return quick ? quick->textDocument() : nullptr;
}

QString shownText(QQuickItem *editor)
{
    auto *document = documentOf(editor);
    return document ? document->toRawText().replace(QChar(0x2029), QChar(u'\n')) : QString();
}

// Runs the event loop until `done` holds when a frame has been drawn, or the deadline passes.
// Returns the wall time from the call to that frame, or nothing when the deadline passed.
template <typename Done> std::optional<std::uint64_t> untilDrawn(QQuickWindow *window, Done done)
{
    QElapsedTimer clock;
    clock.start();
    QEventLoop loop;
    std::optional<std::uint64_t> elapsed;
    QObject::connect(window, &QQuickWindow::frameSwapped, &loop, [&] {
        if (elapsed) return;
        if (done()) {
            elapsed = static_cast<std::uint64_t>(clock.elapsed());
            loop.quit();
        } else {
            window->update();
        }
    });
    QTimer::singleShot(kDrawDeadlineMs, &loop, &QEventLoop::quit);
    window->update();
    loop.exec();
    return elapsed;
}

std::string holdReason(const branch_truce::FileSession &file)
{
    using branch_truce::Support;
    switch (file.support()) {
    case Support::Supported: return "supported";
    case Support::OutsideApp: return "outside the app";
    case Support::LargeFile: return "held as a large file";
    case Support::Unreadable: break;
    }
    if (const auto &failure = file.loadFailure();
        failure && failure->reason == branch_truce::LoadFailureReason::TextTooLarge)
        return "held as unreadable: over the text envelope";
    return "held as unreadable";
}

} // namespace

int editorWorker(const WorkerArgs &args)
{
    if (qgetenv("QT_QPA_PLATFORM").isEmpty()) setenv("QT_QPA_PLATFORM", "offscreen", 0);
    if (qgetenv("QT_QUICK_BACKEND").isEmpty()) setenv("QT_QUICK_BACKEND", "software", 0);
    if (qEnvironmentVariable("QT_QPA_PLATFORM") != QLatin1String("offscreen")) {
        std::cout << "editor worker requires QT_QPA_PLATFORM=offscreen\n";
        return 2;
    }
    int argc = args.argc;
    QApplication app(argc, args.argv);
    const bool longLine = args.mode == "long";
    const auto left =
        longLine ? test_support::workloadLongLine(args.bytes, 'b') : test_support::workloadFile(args.lines, 0);
    const auto right =
        longLine ? test_support::workloadLongLine(args.bytes, 'c') : test_support::workloadFile(args.lines, 1);
    const auto result =
        longLine ? test_support::workloadLongLine(args.bytes, 'x') : test_support::workloadFile(args.lines, 2);
    std::uint64_t lineCount = 0;
    std::uint64_t maxLine = 0;
    inspect(result, lineCount, maxLine);
    const auto byteCount = static_cast<std::uint64_t>(result.size());
    const auto record = [&](const char *metric) {
        auto made = baseRecord("editor", args.name + "-" + metric, metric, byteCount, lineCount, maxLine);
        made.rssKb = vmHwmKb();
        return made;
    };
    const auto fail = [&](const std::string &outcome, const std::string &detail) {
        for (const char *metric : {"load", "key"}) {
            auto failed = record(metric);
            failed.outcome = outcome;
            failed.detail = detail;
            emitRecord(failed);
        }
    };

    const auto scratch = QString::fromStdString(args.scratch);
    branch_truce::Application::Config config;
    config.gitExecutable = QString::fromStdString(test_support::fixtureGit());
    config.recoveryDirectory = scratch + QStringLiteral("/recovery/session");
    config.mergetoolDirectory = scratch + QStringLiteral("/mergetool");
    branch_truce::Application controller{std::move(config)};
    auto *store = controller.store();
    if (!store->appendFile(test_support::choiceFile(left, right, result))) {
        fail("explicit-failure", "the store refused the file");
        return 0;
    }
    const auto id = kFileId.toStdString();
    if (store->file(id)->support() == branch_truce::Support::LargeFile)
        store->loadLargeFile(id); // The user's confirmation.
    if (const auto &file = *store->file(id); file.support() != branch_truce::Support::Supported) {
        fail("explicit-failure", "the product does not open this file: " + holdReason(file));
        return 0;
    }

    QElapsedTimer sceneClock;
    sceneClock.start();
    QQmlApplicationEngine engine;
    engine.setInitialProperties(controller.windowProperties());
    engine.loadFromModule("BranchTruce", "Main");
    auto *window =
        engine.rootObjects().size() == 1 ? qobject_cast<QQuickWindow *>(engine.rootObjects().front()) : nullptr;
    if (!window) {
        fail("explicit-failure", "the Main scene failed to load");
        return 0;
    }
    window->resize(1600, 1000);
    window->show();
    const auto drawnScene = untilDrawn(window, [&] { return sceneItem(window, QStringLiteral("fileList")); });
    if (args.startup) {
        auto startup = baseRecord("editor", "editor-startup", "startup", 0, 0, 0);
        startup.outcome = drawnScene ? "completed" : "explicit-failure";
        if (drawnScene) startup.elapsedMs = static_cast<std::uint64_t>(sceneClock.elapsed());
        startup.detail = "Main scene with no file selected, offscreen software, style "
            + QQuickStyle::name().toStdString() + ", excluded from the envelope";
        startup.rssKb = vmHwmKb();
        emitRecord(startup);
    }
    if (!drawnScene) {
        fail("explicit-failure", "the Main scene was not drawn");
        return 0;
    }

    const auto expected = QString::fromStdString(result);
    QQuickItem *editor = nullptr;
    const auto loaded = [&] {
        editor = sceneItem(window, QStringLiteral("resultText"));
        return editor && editor->isVisible() && editor->width() > 0 && editor->property("contentHeight").toDouble() > 0
            && shownText(editor) == expected;
    };
    QElapsedTimer loadClock;
    loadClock.start();
    if (!controller.selection()->select(kFileId)) {
        fail("explicit-failure", "the file could not be selected");
        return 0;
    }
    const auto drawnLoad = untilDrawn(window, loaded);
    auto load = record("load");
    load.rssKb = vmHwmKb();
    if (!drawnLoad) {
        const auto shown = shownText(editor);
        load.outcome = shown.isEmpty() || shown.size() == expected.size() ? "explicit-failure" : "truncated";
        std::ostringstream detail;
        detail << "result pane shows " << shown.size() << " of " << expected.size() << " UTF-16 units within "
               << kDrawDeadlineMs << " ms";
        load.detail = detail.str();
        emitRecord(load);
        auto key = record("key");
        key.outcome = "incomplete";
        key.detail = "skipped after the load was not completed";
        emitRecord(key);
        return 0;
    }
    load.elapsedMs = static_cast<std::uint64_t>(loadClock.elapsed());
    load.outcome = "completed";
    load.detail = "selected in the real window: three panes, highlighting and layout, until drawn";
    emitRecord(load);

    editor->forceActiveFocus();
    editor->setProperty("cursorPosition", 0);
    QCoreApplication::processEvents();
    const auto revision = store->file(id)->current().id;
    const auto typed = QStringLiteral("a") + expected;
    const auto acknowledged = [&] {
        const auto *file = store->file(id);
        return file && file->current().id != revision && shownText(editor) == typed;
    };
    QElapsedTimer keyClock;
    keyClock.start();
    QKeyEvent press(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
    QCoreApplication::sendEvent(editor, &press);
    const auto drawnKey = untilDrawn(window, acknowledged);
    auto key = record("key");
    key.rssKb = vmHwmKb();
    const auto *file = store->file(id);
    const bool stored = file && file->current().draft.text == "a" + result;
    if (drawnKey && stored) {
        key.elapsedMs = static_cast<std::uint64_t>(keyClock.elapsed());
        key.outcome = "completed";
        key.detail = "one ASCII letter at the start, in the store and the pane, until drawn";
    } else if (file && file->current().id == revision) {
        key.outcome = "unacknowledged";
        key.detail = "the store kept the revision from before the key";
    } else {
        key.outcome = "truncated";
        key.detail = "the store or the pane does not hold exactly the letter and the result";
    }
    emitRecord(key);
    return 0;
}

} // namespace workload
