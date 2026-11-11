// Navigation and scrolling: within-file and file-list navigation, synchronised scrolling, and the
// cursor and viewport across undo, redo and choices.
#include "ComparisonCases.h"
#include "ComparisonEditor.h"
#include "ComparisonGeometry.h"
#include "ComparisonNavigationChecks.h"
#include "ComparisonSessionChecks.h"
#include "ComparisonTextProbes.h"
#include "fixtures/AppSession.h"
#include "fixtures/Await.h"

using test_support::await;

using namespace comparison_scene;

namespace {
void addFileListNavigationCases(Group &scenes)
{
    scenes.run("revision progress, within-file navigation, unfinished files and file-list location", [] {
        const std::string result = "same\nAAA\nmid\nBBB\n";
        const std::string left = "same\nleftA\nmid\nleftB\n";
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, test_support::choiceFile(left, result, result));
        const auto revision = fixture.file().current().id;
        CHECK(revision == 1);
        CHECK(fixture.file().facts().undecided == 4);
        expectProgress(*controller.selection(), *controller.selectedFile(), false, false, false, false,
                       QStringLiteral("1"), {}, {}, {}, 4);

        select(fixture, 0, 0);
        clickMenuEntry(fixture, "navigateMenu", "nextDifference");
        expectNavigation(fixture, *controller.navigator(), *controller.selection(), QStringLiteral("difference"),
                         QStringLiteral("choices"), QStringLiteral("result"), 0, 2, 5, 9,
                         QStringLiteral("Difference 1 of 2 · result bytes 5-9"));
        clickMenuEntry(fixture, "navigateMenu", "nextDifference");
        expectNavigation(fixture, *controller.navigator(), *controller.selection(), QStringLiteral("difference"),
                         QStringLiteral("choices"), QStringLiteral("result"), 1, 2, 13, 17,
                         QStringLiteral("Difference 2 of 2 · result bytes 13-17"));
        clickMenuEntry(fixture, "navigateMenu", "nextDifference");
        expectNavigation(fixture, *controller.navigator(), *controller.selection(), QStringLiteral("difference"),
                         QStringLiteral("choices"), QStringLiteral("result"), 0, 2, 5, 9,
                         QStringLiteral("Difference 1 of 2 · result bytes 5-9"));
        clickMenuEntry(fixture, "navigateMenu", "previousDifference");
        expectNavigation(fixture, *controller.navigator(), *controller.selection(), QStringLiteral("difference"),
                         QStringLiteral("choices"), QStringLiteral("result"), 1, 2, 13, 17,
                         QStringLiteral("Difference 2 of 2 · result bytes 13-17"));

        // Contribution decisions remain four, while this markerless draft has no conflicts.
        for (const auto *name : {"nextUndecided", "previousUndecided", "undecidedCounter"})
            CHECK(!fixture.item(name)->isEnabled());
        CHECK(controller.selectedFile()->progress().value("conflicts").toInt() == 0);
        // Explicit legacy source-decision callers retain their exact four targets.
        select(fixture, 0, 0);
        CHECK(controller.navigator()->navigate(FileNavigator::Mode::Undecided, 1));
        expectNavigation(fixture, *controller.navigator(), *controller.selection(), QStringLiteral("undecided"),
                         QStringLiteral("choices"), QStringLiteral("result"), 0, 4, 5, 9,
                         QStringLiteral("Undecided 1 of 4 · Left · result bytes 5-9"), QStringLiteral("pick/1/5/11"));
        CHECK(controller.navigator()->navigate(FileNavigator::Mode::Undecided, 1));
        expectNavigation(fixture, *controller.navigator(), *controller.selection(), QStringLiteral("undecided"),
                         QStringLiteral("choices"), QStringLiteral("result"), 1, 4, 5, 9,
                         QStringLiteral("Undecided 2 of 4 · Right · result bytes 5-9"), QStringLiteral("pick/2/5/9"));
        CHECK(controller.navigator()->navigate(FileNavigator::Mode::Undecided, 1));
        expectNavigation(fixture, *controller.navigator(), *controller.selection(), QStringLiteral("undecided"),
                         QStringLiteral("choices"), QStringLiteral("result"), 2, 4, 13, 17,
                         QStringLiteral("Undecided 3 of 4 · Left · result bytes 13-17"),
                         QStringLiteral("pick/1/15/21"));
        CHECK(controller.navigator()->navigate(FileNavigator::Mode::Undecided, 1));
        expectNavigation(fixture, *controller.navigator(), *controller.selection(), QStringLiteral("undecided"),
                         QStringLiteral("choices"), QStringLiteral("result"), 3, 4, 13, 17,
                         QStringLiteral("Undecided 4 of 4 · Right · result bytes 13-17"),
                         QStringLiteral("pick/2/13/17"));
        CHECK(controller.navigator()->navigate(FileNavigator::Mode::Undecided, 1));
        expectNavigation(fixture, *controller.navigator(), *controller.selection(), QStringLiteral("undecided"),
                         QStringLiteral("choices"), QStringLiteral("result"), 0, 4, 5, 9,
                         QStringLiteral("Undecided 1 of 4 · Left · result bytes 5-9"), QStringLiteral("pick/1/5/11"));
        CHECK(controller.navigator()->navigate(FileNavigator::Mode::Undecided, -1));
        expectNavigation(fixture, *controller.navigator(), *controller.selection(), QStringLiteral("undecided"),
                         QStringLiteral("choices"), QStringLiteral("result"), 3, 4, 13, 17,
                         QStringLiteral("Undecided 4 of 4 · Right · result bytes 13-17"),
                         QStringLiteral("pick/2/13/17"));
        CHECK(controller.selection()->fileId() == QStringLiteral("choices"));
        CHECK(fixture.file().current().id == revision);
        CHECK(fixture.text().toStdString() == result);

        CHECK(fixture.item("saveDraft")->isEnabled());
        click(fixture, "saveDraft");
        CHECK(controller.feedback()->message()
              == QStringLiteral("Draft saved in Branch Truce; repository file unchanged."));
        CHECK(fixture.file().publication().checkpoint() && *fixture.file().publication().checkpoint() == revision);
        CHECK(!fixture.file().publication().written() && !fixture.file().publication().staged()
              && !fixture.file().publication().reviewed());
        expectProgress(*controller.selection(), *controller.selectedFile(), true, false, false, false,
                       QStringLiteral("1"), QStringLiteral("1"), {}, {}, 4);

        // Progress keeps the review, write and stage revisions separate.
        CHECK(await(controller.store()->beginMarkResolved("choices", revision, true)) == Transition::Applied);
        CHECK(fixture.file().publication().reviewed() && fixture.file().publication().reviewed()->revision == revision);
        CHECK(!fixture.file().publication().written() && !fixture.file().publication().staged()
              && !fixture.file().facts().resolved);
        expectProgress(*controller.selection(), *controller.selectedFile(), true, true, false, false,
                       QStringLiteral("1"), QStringLiteral("1"), QStringLiteral("1"), {}, 4);
        CHECK(await(controller.store()->recordWrite("choices", revision, Outcome::Succeeded)) == Transition::Applied);
        CHECK(
            await(controller.store()->recordStage("choices", revision, Outcome::Succeeded, "verified-object", "100644"))
            == Transition::Applied);
        CHECK(fixture.file().facts().resolved && fixture.file().publication().staged()->revision == revision);
        CHECK(fixture.text().toStdString() == result);
        expectProgress(*controller.selection(), *controller.selectedFile(), true, true, true, false,
                       QStringLiteral("1"), QStringLiteral("1"), QStringLiteral("1"), QStringLiteral("1"), 4);

        CHECK(controller.store()->appendFile(FileSession(
            "outside", "diagram.png", {}, {.support = Support::OutsideApp, .supportDetail = "image content"})));
        CHECK(controller.files()->outsideAppFiles() == 1);
        CHECK(fixture.file().facts().resolved);
        click(fixture, "nextUnfinished");
        expectNavigation(fixture, *controller.navigator(), *controller.selection(), QStringLiteral("unfinished"),
                         QStringLiteral("outside"), QStringLiteral("outside-editor"), 0, 1, 0, 0,
                         QStringLiteral("Unfinished file · outside the editor · diagram.png"));
        const auto unfinished = controller.navigator()->target();
        CHECK(unfinished.value(QStringLiteral("support")).value<FileStates::SupportKind>()
              == FileStates::SupportKind::OutsideApp);
        CHECK(!unfinished.value(QStringLiteral("resolved")).toBool());
        CHECK(unfinished.value(QStringLiteral("path")).toString() == QStringLiteral("diagram.png"));
        CHECK(fixture.item("selectedProblem")->property("text").toString()
              == QStringLiteral("This file must be resolved outside Branch Truce"));
        CHECK(fixture.item("selectedProblem")
                  ->property("explanation")
                  .toString()
                  .contains(QStringLiteral("outstanding")));
        expectDiagnostic(fixture, QStringLiteral("image content"));
        const auto *outside = controller.store()->file("outside");
        CHECK(outside && outside->support() == Support::OutsideApp);
        CHECK(outside->supportDetail() == "image content");
        CHECK(outside->current().draft.text.empty() && !outside->facts().resolved);
        click(fixture, "previousUnfinished");
        expectNavigation(fixture, *controller.navigator(), *controller.selection(), QStringLiteral("unfinished"),
                         QStringLiteral("outside"), QStringLiteral("outside-editor"), 0, 1, 0, 0,
                         QStringLiteral("Unfinished file · outside the editor · diagram.png"));
        CHECK(fixture.file().support() == Support::Supported && fixture.file().facts().resolved);

        CHECK(controller.selection()->select(QStringLiteral("choices")));
        settle([&] { return fixture.text().toStdString() == result; });
        auto newer = fixture.file().current().draft;
        newer.text += "newer\n";
        CHECK(await(controller.store()->replaceDraft("choices", revision, newer)) == Transition::Applied);
        CHECK(fixture.file().current().id == revision + 1);
        CHECK(fixture.file().publication().staged() && fixture.file().publication().staged()->revision == revision);
        CHECK(fixture.file().facts().earlierStaged && !fixture.file().facts().resolved
              && fixture.file().facts().unsaved);
        settle([&] { return fixture.text().toStdString() == newer.text; });
        expectProgress(*controller.selection(), *controller.selectedFile(), false, false, false, true,
                       QStringLiteral("2"), {}, {}, QStringLiteral("1"), 4);

        const auto heldRevision = fixture.file().current().id;
        const auto heldText = fixture.text();
        select(fixture, 1, 6);
        auto *editor = fixture.item("resultText");
        const int cursor = editor->property("cursorPosition").toInt();
        const int selectionStart = editor->property("selectionStart").toInt();
        const int selectionEnd = editor->property("selectionEnd").toInt();
        if (cursor != 6 || selectionStart != 1 || selectionEnd != 6)
            throw std::runtime_error("held selection " + std::to_string(selectionStart) + "-"
                                     + std::to_string(selectionEnd) + " cursor " + std::to_string(cursor));
        CHECK(cursor == 6 && selectionStart == 1 && selectionEnd == 6);
        CHECK(fixture.window->property("fileListVisible").toBool() && fixture.item("fileList")->isVisible());
        CHECK(fixture.item("toggleFileList")->property("checked").toBool());
        CHECK(test_support::accessibleName(fixture.item("toggleFileList")) == QStringLiteral("Show file list"));
        click(fixture, "toggleFileList");
        CHECK(!fixture.window->property("fileListVisible").toBool() && !fixture.item("fileList")->isVisible());
        CHECK(!fixture.item("toggleFileList")->property("checked").toBool());
        CHECK(test_support::accessibleName(fixture.item("toggleFileList")) == QStringLiteral("Show file list"));
        CHECK(controller.selection()->fileId() == QStringLiteral("choices"));
        CHECK(fixture.file().current().id == heldRevision && fixture.file().facts().unsaved);
        CHECK(fixture.text() == heldText && fixture.text().toStdString() == newer.text);
        CHECK(editor->property("cursorPosition").toInt() == cursor);
        CHECK(editor->property("selectionStart").toInt() == selectionStart);
        CHECK(editor->property("selectionEnd").toInt() == selectionEnd);
        click(fixture, "toggleFileList");
        CHECK(fixture.window->property("fileListVisible").toBool() && fixture.item("fileList")->isVisible());
        CHECK(fixture.item("toggleFileList")->property("checked").toBool());
        CHECK(test_support::accessibleName(fixture.item("toggleFileList")) == QStringLiteral("Show file list"));
        CHECK(controller.selection()->fileId() == QStringLiteral("choices"));
        CHECK(fixture.file().current().id == heldRevision && fixture.text() == heldText
              && fixture.file().facts().unsaved);
        CHECK(editor->property("cursorPosition").toInt() == cursor);
        CHECK(editor->property("selectionStart").toInt() == selectionStart);
        CHECK(editor->property("selectionEnd").toInt() == selectionEnd);
        const auto *stillOutside = controller.store()->file("outside");
        CHECK(stillOutside && stillOutside->support() == Support::OutsideApp);
        CHECK(stillOutside->supportDetail() == "image content" && stillOutside->current().draft.text.empty());
    });
    scenes.run("toggling the file list restores the result viewport scroll position", [] {
        std::string result;
        for (int line = 0; line < 200; ++line) result += std::string(240, static_cast<char>('a' + line % 26)) + "\n";
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, test_support::choiceFile("left\n", result, result));
        auto *viewport = fixture.item("resultViewport");
        settle([&] {
            return viewport->property("contentHeight").toReal() > viewport->height() * 2
                && viewport->property("contentWidth").toReal() > viewport->width() * 2;
        });
        // Let the initial file placement run before establishing the scroll
        // position whose preservation this test exercises.
        test_support::waitForStable(fixture.window, "initial result viewport", [&] { return viewport; }, 60);
        // Keep the cursor at the top so that scrolling away leaves it out of view.
        select(fixture, 0, 0);
        CHECK(viewport->setProperty("contentY", 600.0));
        CHECK(viewport->setProperty("contentX", 40.0));
        settle([&] {
            return viewport->property("contentY").toReal() == 600.0 && viewport->property("contentX").toReal() == 40.0;
        });
        // Move the viewport between the note and the restore, as the relayout does.
        const auto moved = QObject::connect(
            fixture.item("fileList"), &QQuickItem::visibleChanged, viewport,
            [viewport] {
                viewport->setProperty("contentY", 0.0);
                viewport->setProperty("contentX", 0.0);
            },
            Qt::DirectConnection);
        click(fixture, "toggleFileList");
        QObject::disconnect(moved);
        CHECK(!fixture.window->property("fileListVisible").toBool());
        settle([&] { return fixture.item("resultText")->width() > 0; });
        CHECK(viewport->property("contentY").toReal() == 600.0);
        CHECK(viewport->property("contentX").toReal() == 40.0);
        click(fixture, "toggleFileList");
        CHECK(fixture.window->property("fileListVisible").toBool());
        settle([&] { return fixture.item("resultText")->width() > 0; });
        CHECK(viewport->property("contentY").toReal() == 600.0);
        CHECK(viewport->property("contentX").toReal() == 40.0);
    });
    scenes.run("dragging the split handle resizes the file list within its limits and the toggle collapses it", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller);
        auto *sidebar = test_support::waitForLayout(fixture.window, QStringLiteral("fileList"), 60)->parentItem();
        auto *split = sidebar->parentItem();
        while (split && !QByteArray(split->metaObject()->className()).contains("SplitView"))
            split = split->parentItem();
        CHECK(split);
        const auto minimum = qmlValue(sidebar, QStringLiteral("SplitView.minimumWidth")).toReal();
        const auto maximum = [&] { return qmlValue(sidebar, QStringLiteral("SplitView.maximumWidth")).toReal(); };
        // The split puts its handles beside the items, as its own children.
        const auto handle = [&]() -> QQuickItem * {
            const auto edge = sidebar->mapToScene(QPointF(sidebar->width(), 0)).x();
            for (auto *child : split->childItems()) {
                if (child->isVisible() && child->width() > 0 && child->width() < 40
                    && std::abs(child->mapToScene(QPointF()).x() - edge) < 2)
                    return child;
            }
            throw std::runtime_error("no split handle beside the file list");
        };
        // Drags the handle by a distance, kept inside the window, a few pixels at a time.
        const auto drag = [&](int by) {
            auto *grip = handle();
            const auto from = grip->mapToScene(QPointF(grip->width() / 2, grip->height() / 2)).toPoint();
            const auto to = std::clamp(from.x() + by, 5, fixture.window->width() - 5);
            QTest::mousePress(fixture.window, Qt::LeftButton, Qt::NoModifier, from);
            for (int step = 1; step <= 10; ++step)
                QTest::mouseMove(fixture.window, QPoint(from.x() + (to - from.x()) * step / 10, from.y()));
            QTest::mouseRelease(fixture.window, Qt::LeftButton, Qt::NoModifier, QPoint(to, from.y()));
            test_support::waitForFrames(fixture.window, "the split is laid out after the drag");
        };
        const auto start = sidebar->width();
        CHECK(start >= minimum && start + 80 < maximum());
        drag(80);
        CHECK(std::abs(sidebar->width() - (start + 80)) < 2);
        // It stops at half the work area, and at its minimum.
        drag(fixture.window->width());
        CHECK(std::abs(sidebar->width() - maximum()) < 1);
        drag(-fixture.window->width());
        CHECK(std::abs(sidebar->width() - minimum) < 1);
        drag(120);
        const auto chosen = sidebar->width();
        CHECK(std::abs(chosen - (minimum + 120)) < 2);
        click(fixture, "toggleFileList");
        CHECK(!fixture.window->property("fileListVisible").toBool());
        settle([&] { return !sidebar->isVisible(); });
        click(fixture, "toggleFileList");
        CHECK(fixture.window->property("fileListVisible").toBool());
        settle([&] { return sidebar->isVisible() && sidebar->width() > 0; });
        CHECK(std::abs(sidebar->width() - chosen) < 2);
    });
}

void addHistoryPlacementCases(Groups groups)
{
    const auto undoRedoKeepViewport = [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, insertionFile());
        auto *editor = fixture.item("resultText");
        auto *left = fixture.item("stage2Viewport");
        auto *result = fixture.item("resultViewport");
        auto *right = fixture.item("stage3Viewport");
        const auto line = paneLineHeight(editor);
        placeResultCursor(fixture, QStringLiteral("shared 20\n"), 10 * line);
        const auto offset = editor->property("cursorPosition").toInt();
        key(fixture, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
        settle([&] { return fixture.file().current().draft.text.find("xshared 20\n") != std::string::npos; });
        settleReplacement(fixture);
        CHECK(editor->property("cursorPosition").toInt() == offset + 1);
        const std::array<QQuickItem *, 3> panes{left, result, right};
        const std::array<qreal, 3> typed{contentY(left), contentY(result), contentY(right)};
        CHECK(std::abs(typed[1] - 10 * line) < 1);

        key(fixture, Qt::Key_Z, Qt::ControlModifier);
        CHECK(fixture.file().current().draft.text.find("xshared") == std::string::npos);
        settleReplacement(fixture);
        CHECK_EQ(editor->property("cursorPosition").toInt(), offset);
        CHECK(editor->property("selectionStart").toInt() == editor->property("selectionEnd").toInt());
        // No pane follows the replaced text to its end.
        for (std::size_t pane = 0; pane < panes.size(); ++pane)
            CHECK(std::abs(contentY(panes[pane]) - typed[pane]) < 1);

        key(fixture, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        CHECK(fixture.file().current().draft.text.find("xshared 20\n") != std::string::npos);
        settleReplacement(fixture);
        // The cursor follows the restored insertion, as it did when the character was typed.
        CHECK(editor->property("cursorPosition").toInt() == offset + 1);
        for (std::size_t pane = 0; pane < panes.size(); ++pane)
            CHECK(std::abs(contentY(panes[pane]) - typed[pane]) < 1);
    };
    // The view restores the result viewport once, with no reapply after the next frame, so
    // let the window draw frames after the undo before reading the panes.
    const auto undoKeepsViewportAfterFrames = [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, insertionFile());
        auto *editor = fixture.item("resultText");
        auto *result = fixture.item("resultViewport");
        const auto line = paneLineHeight(editor);
        placeResultCursor(fixture, QStringLiteral("shared 200\n"), 220 * line);
        const auto offset = editor->property("cursorPosition").toInt();
        CHECK(cursorInView(fixture));
        key(fixture, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
        settle([&] { return fixture.file().current().draft.text.find("xshared 200\n") != std::string::npos; });
        settleReplacement(fixture);
        const auto typed = contentY(result);
        CHECK(std::abs(typed - 220 * line) < 1);
        int frames = 0;
        const auto counter =
            QObject::connect(fixture.window, &QQuickWindow::frameSwapped, fixture.window, [&frames] { ++frames; });
        key(fixture, Qt::Key_Z, Qt::ControlModifier);
        CHECK(fixture.file().current().draft.text.find("xshared") == std::string::npos);
        settleFor("three frames after the undo", [&] {
            if (frames < 3) fixture.window->update();
            return frames >= 3;
        });
        QObject::disconnect(counter);
        settleReplacement(fixture);
        CHECK_EQ(editor->property("cursorPosition").toInt(), offset);
        CHECK(cursorInView(fixture));
        if (std::abs(contentY(result) - typed) >= 1)
            throw std::runtime_error("result viewport moved from " + std::to_string(typed) + " to "
                                     + std::to_string(contentY(result)) + " after " + std::to_string(frames)
                                     + " frames");
    };
    const std::string undoRedoName = "undo and redo keep the result cursor and scroll position, and the sources stay";
    const std::string framesName = "undo keeps the result viewport after the window draws";
    groups.display.run(undoRedoName, undoRedoKeepViewport);
    groups.display.run(framesName, undoKeepsViewportAfterFrames);
}

}

void comparison_scene::addNavigationCases(Groups groups)
{
    addFileListNavigationCases(groups.scenes);
    addHistoryPlacementCases(groups);
}
