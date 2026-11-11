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
using test_support::commandResult;

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

void addPaneSynchronizationCases(Group &scenes)
{
    scenes.run("scrolling the left source moves the result and the right source to the mapped lines", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, insertionFile());
        auto *left = fixture.item("stage2Viewport");
        auto *result = fixture.item("resultViewport");
        auto *right = fixture.item("stage3Viewport");
        auto *editor = fixture.item("resultText");
        settle([&] { return editor->property("lineCount").toInt() > 400 && left->height() > 0; });
        settlePanes(fixture);
        const auto line = paneLineHeight(editor);
        // With every visible left line past the insertion, the result and the
        // right source show the same lines 30 lines further down.
        wheelTo(fixture, "stage2Viewport", 80 * line);
        CHECK(std::abs(contentY(result) - (contentY(left) + 30 * line)) < 1);
        CHECK(std::abs(contentY(right) - contentY(result)) < 1);
        // Nothing scrolls on once the movement has ended.
        const std::array<qreal, 3> settled{contentY(left), contentY(result), contentY(right)};
        test_support::waitForFrames(fixture.window, "the scrolled panes are drawn");
        CHECK((std::array<qreal, 3>{contentY(left), contentY(result), contentY(right)}) == settled);

        // Five new lines at the top of the result move the mapping with them.
        select(fixture, 0, 0);
        for (int typed = 0; typed < 5; ++typed) key(fixture, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
        settle([&] { return fixture.file().current().draft.text.starts_with("\n\n\n\n\nshared 1\n"); });
        wheelTo(fixture, "stage2Viewport", 120 * line);
        CHECK(std::abs(contentY(result) - (contentY(left) + 35 * line)) < 1);
        CHECK(std::abs(contentY(right) - (contentY(left) + 30 * line)) < 1);
    });
    scenes.run("a synchronised scroll settles in the same event-loop turn without feedback", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, insertionFile());
        auto *left = fixture.item("stage2Viewport");
        auto *result = fixture.item("resultViewport");
        auto *right = fixture.item("stage3Viewport");
        auto *editor = fixture.item("resultText");
        settle([&] { return editor->property("lineCount").toInt() > 400 && left->height() > 0; });
        settlePanes(fixture);
        const auto line = paneLineHeight(editor);
        // The left source has keyboard focus, so its own scrolling leads.
        fixture.item("stage2Text")->forceActiveFocus();
        for (const qreal top : {100 * line, 140 * line, 90 * line}) {
            CHECK(left->setProperty("contentY", top));
            // Read before any event is processed: the other panes moved in the same turn.
            CHECK(std::abs(contentY(result) - (top + 30 * line)) < 1);
            CHECK(std::abs(contentY(right) - (top + 30 * line)) < 1);
            test_support::waitForFrames(fixture.window, "the synchronised panes are drawn");
            CHECK(contentY(left) == top);
            CHECK(std::abs(contentY(result) - (top + 30 * line)) < 1);
            CHECK(std::abs(contentY(right) - (top + 30 * line)) < 1);
        }
        // A pane the user is not scrolling does not lead the others.
        const auto held = contentY(left);
        CHECK(right->setProperty("contentY", contentY(right) + 10 * line));
        test_support::waitForFrames(fixture.window, "the right pane's scroll is drawn");
        CHECK(contentY(left) == held);
        CHECK(std::abs(contentY(result) - (held + 30 * line)) < 1);
    });
    scenes.run("an ambiguous passage scrolls the other panes proportionally without jumping", [] {
        // 40 equal lines on the left against 20 in the result cannot be aligned line by line.
        const auto before = numberedLines("before", 1, 80);
        const auto after = numberedLines("after", 1, 200);
        std::string left = before;
        std::string result = before;
        for (int line = 0; line < 40; ++line) left += "same\n";
        for (int line = 0; line < 20; ++line) result += "same\n";
        left += after;
        result += after;
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, test_support::choiceFile(left, result, result));
        bool ambiguous = false;
        for (const auto &value : controller.selectedFile()->comparison()->passages()) {
            const auto passage = value.toMap();
            ambiguous = ambiguous
                || (passage.value("side") == "stage2" && passage.value("kind") == "ambiguous"
                    && passage.value("sourceLineBegin").toInt() <= 81 && passage.value("sourceLineEnd").toInt() >= 121);
        }
        CHECK(ambiguous);
        auto *leftView = fixture.item("stage2Viewport");
        auto *resultView = fixture.item("resultViewport");
        auto *editor = fixture.item("resultText");
        settle([&] { return editor->property("lineCount").toInt() > 300 && leftView->height() > 0; });
        settlePanes(fixture);
        const auto line = paneLineHeight(editor);
        fixture.item("stage2Text")->forceActiveFocus();
        CHECK(leftView->setProperty("contentY", 30 * line));
        CHECK(std::abs(contentY(resultView) - 30 * line) < 1);
        qreal previous = contentY(resultView);
        for (int offset = 4; 30 * line + offset <= 170 * line; offset += 4) {
            const qreal top = 30 * line + offset;
            CHECK(leftView->setProperty("contentY", top));
            const auto now = contentY(resultView);
            // The result never runs backwards or ahead of the left.
            if (now < previous - 0.01 || now - previous > 4.01)
                throw std::runtime_error("result jumped from " + std::to_string(previous) + " to " + std::to_string(now)
                                         + " at left " + std::to_string(top));
            previous = now;
        }
        // Past the passage the result is 20 lines behind the left.
        CHECK(std::abs(contentY(resultView) - (contentY(leftView) - 20 * line)) < 1);
    });
    scenes.run("the shown earlier version scrolls with the other panes", [] {
        auto sources = *insertionFile().snapshots();
        const auto earlier = sources.stage2->worktree.utf8;
        sources.earlier = SourceSnapshot{"earlier-object", "100644",
                                         earlier,          test_support::choiceText(earlier),
                                         "earlier-commit", "Common earlier version"};
        const auto launch = sources.launchResult.text.utf8;
        Application controller(test_support::windowConfig());
        Fixture fixture(
            controller,
            FileSession("choices", sources.path, {launch, {}, 0}, {.snapshots = sources, .initializeChoices = true}));
        click(fixture, "showEarlier");
        auto *left = fixture.item("stage2Viewport");
        auto *earlierView = fixture.item("earlierViewport");
        auto *editor = fixture.item("resultText");
        settle([&] { return earlierView->height() > 0 && editor->property("lineCount").toInt() > 400; });
        const auto line = paneLineHeight(editor);
        wheelTo(fixture, "stage2Viewport", 80 * line);
        // The earlier version equals the left source, so it shows the same lines.
        CHECK(std::abs(contentY(earlierView) - contentY(left)) < 1);
    });
    scenes.run("the earlier version catches up with the other panes when it is first shown", [] {
        auto sources = *insertionFile().snapshots();
        const auto earlier = sources.stage2->worktree.utf8;
        sources.earlier = SourceSnapshot{"earlier-object", "100644",
                                         earlier,          test_support::choiceText(earlier),
                                         "earlier-commit", "Common earlier version"};
        const auto launch = sources.launchResult.text.utf8;
        Application controller(test_support::windowConfig());
        Fixture fixture(
            controller,
            FileSession("choices", sources.path, {launch, {}, 0}, {.snapshots = sources, .initializeChoices = true}));
        auto *left = fixture.item("stage2Viewport");
        auto *earlierView = fixture.item("earlierViewport");
        auto *editor = fixture.item("resultText");
        settle([&] { return editor->property("lineCount").toInt() > 400 && left->height() > 0; });
        settlePanes(fixture);
        CHECK(!earlierView->isVisible());
        const auto line = paneLineHeight(editor);
        // The panes scroll while the earlier version is hidden, so it is still at the top.
        wheelTo(fixture, "stage2Viewport", 80 * line);
        CHECK(contentY(earlierView) == 0);
        click(fixture, "showEarlier");
        settleFor("the earlier version to catch up", [&] {
            return earlierView->isVisible() && earlierView->height() > 0
                && std::abs(contentY(earlierView) - contentY(left)) < 1;
        });
        // It equals the left source, so it shows the left source's lines.
        CHECK(contentY(earlierView) > 70 * line);
        CHECK(std::abs(contentY(earlierView) - contentY(left)) < 1);
    });
    scenes.run("Find in a source scrolls the result and the other source to the passage", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, insertionFile());
        click(fixture, "openPassageSearch");
        auto *left = fixture.item("stage2Viewport");
        auto *result = fixture.item("resultViewport");
        auto *right = fixture.item("stage3Viewport");
        auto *editor = fixture.item("resultText");
        settle([&] { return editor->property("lineCount").toInt() > 400 && left->height() > 0; });
        settlePanes(fixture);
        const auto line = paneLineHeight(editor);
        CHECK(contentY(left) < 200 * line);
        CHECK(fixture.item("searchScope")->property("currentText").toString() == "Left source");
        CHECK(fixture.item("searchQuery")->setProperty("text", "shared 300"));
        click(fixture, "findPassage");
        auto *found = fixture.item("stage2Text");
        CHECK(found->property("selectedText").toString() == "shared 300");
        settleReplacement(fixture);
        // The left source shows the passage, and the others the lines mapped to it.
        const auto cursor = found->property("cursorRectangle").toRectF();
        const auto shown = found->y() + cursor.top() - contentY(left);
        CHECK(shown >= 0 && shown + cursor.height() <= left->height());
        CHECK(contentY(left) > 200 * line);
        CHECK(std::abs(contentY(result) - (contentY(left) + 30 * line)) < 1);
        CHECK(std::abs(contentY(right) - contentY(result)) < 1);
    });
    scenes.run("within-file navigation scrolls both sources to the result's difference", [] {
        const auto left = numberedLines("shared", 1, 400);
        const auto right = numberedLines("shared", 1, 50) + numberedLines("right only", 1, 30)
            + numberedLines("shared", 51, 249) + "right late\n" + numberedLines("shared", 301, 100);
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, test_support::choiceFile(left, right, right));
        auto *leftView = fixture.item("stage2Viewport");
        auto *result = fixture.item("resultViewport");
        auto *rightView = fixture.item("stage3Viewport");
        auto *editor = fixture.item("resultText");
        settle([&] { return editor->property("lineCount").toInt() > 400 && leftView->height() > 0; });
        settlePanes(fixture);
        const auto line = paneLineHeight(editor);
        const auto late = static_cast<int>(fixture.text().indexOf("right late"));
        CHECK(late > 0);
        select(fixture, 0, 0);
        for (int click = 0; click < 5 && editor->property("selectionStart").toInt() != late; ++click)
            clickMenuEntry(fixture, "navigateMenu", "nextDifference");
        CHECK(editor->property("selectionStart").toInt() == late);
        settleReplacement(fixture);
        CHECK(cursorInView(fixture));
        CHECK(contentY(result) > 250 * line);
        // Past the insertion the left source is 30 lines behind; the right source equals the result.
        CHECK(std::abs(contentY(leftView) - (contentY(result) - 30 * line)) < 1);
        CHECK(std::abs(contentY(rightView) - contentY(result)) < 1);
    });
}

void addHistoryPlacementCases(Groups groups)
{
    auto &scenes = groups.scenes;
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
    scenes.run(undoRedoName, undoRedoKeepViewport);
    scenes.run(framesName, undoKeepsViewportAfterFrames);
    groups.display.run(undoRedoName, undoRedoKeepViewport);
    groups.display.run(framesName, undoKeepsViewportAfterFrames);
    scenes.run("undo keeps a result selection on the same text", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, insertionFile());
        auto *editor = fixture.item("resultText");
        auto *result = fixture.item("resultViewport");
        const auto line = paneLineHeight(editor);
        placeResultCursor(fixture, QStringLiteral("shared 20\n"), 10 * line);
        key(fixture, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
        settle([&] { return fixture.file().current().draft.text.find("xshared 20\n") != std::string::npos; });
        // Select "shared 22" backwards, so the cursor is at its start.
        const auto selected = static_cast<int>(fixture.text().indexOf(QStringLiteral("shared 22")));
        select(fixture, selected + 9, selected);
        key(fixture, Qt::Key_Z, Qt::ControlModifier);
        settleReplacement(fixture);
        CHECK(fixture.text().indexOf(QStringLiteral("shared 22")) == selected - 1);
        CHECK(editor->property("selectionStart").toInt() == selected - 1);
        CHECK(editor->property("selectionEnd").toInt() == selected + 8);
        CHECK(editor->property("cursorPosition").toInt() == selected - 1);
        CHECK(std::abs(contentY(result) - 10 * line) < 1);
    });
    scenes.run("applying a choice keeps the result cursor and scroll position", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, insertionFile());
        auto *editor = fixture.item("resultText");
        auto *result = fixture.item("resultViewport");
        const auto line = paneLineHeight(editor);
        // Above the 30 inserted lines that the left source lacks.
        placeResultCursor(fixture, QStringLiteral("shared 20\n"), 10 * line);
        const auto offset = editor->property("cursorPosition").toInt();
        const auto before = fixture.text();
        const auto &target = test_support::choiceTarget(fixture.file(), SourceSide::Stage2, ChoiceScope::Section);
        CHECK(test_support::previewResult(
            controller,
            controller.choices()->previewChoice("choices", QString::number(fixture.file().current().id),
                                                QString::fromStdString(target.source.contributionId),
                                                ChoicePreviewFlow::Action::ResolveUsingSide)));
        CHECK(commandResult(controller, controller.choices()->applyChoice(true)));
        CHECK(fixture.text() != before && !fixture.text().contains(QStringLiteral("right only")));
        settleReplacement(fixture);
        CHECK_EQ(editor->property("cursorPosition").toInt(), offset);
        CHECK(std::abs(contentY(result) - 10 * line) < 1);
    });
    scenes.run("a choice above the result cursor keeps it on its text and in view", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, insertionFile());
        auto *editor = fixture.item("resultText");
        const auto line = paneLineHeight(editor);
        // Below the 30 inserted lines, with the cursor near the top of the view.
        placeResultCursor(fixture, QStringLiteral("shared 120\n"), 148 * line);
        CHECK(cursorInView(fixture));
        const auto &target = test_support::choiceTarget(fixture.file(), SourceSide::Stage2, ChoiceScope::Section);
        CHECK(test_support::previewResult(
            controller,
            controller.choices()->previewChoice("choices", QString::number(fixture.file().current().id),
                                                QString::fromStdString(target.source.contributionId),
                                                ChoicePreviewFlow::Action::ResolveUsingSide)));
        CHECK(commandResult(controller, controller.choices()->applyChoice(true)));
        CHECK(!fixture.text().contains(QStringLiteral("right only")));
        settleReplacement(fixture);
        // The removed lines took the cursor's line out of the kept view, so the view follows it.
        CHECK_EQ(editor->property("cursorPosition").toInt(), fixture.text().indexOf(QStringLiteral("shared 120\n")));
        CHECK(cursorInView(fixture));
    });
    scenes.run("two separate changes keep the result cursor on the unchanged text between them", [] {
        // The sources differ on lines 20 and 350, and the result holds the right source.
        const auto first = numberedLines("shared", 1, 19);
        const auto middle = numberedLines("shared", 21, 329);
        const auto last = numberedLines("shared", 351, 50);
        const auto left = first + "left first\n" + middle + "left last\n" + last;
        const auto right = first + "right first\n" + middle + "right last\n" + last;
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, test_support::choiceFile(left, right, right));
        auto *editor = fixture.item("resultText");
        auto *result = fixture.item("resultViewport");
        const auto line = paneLineHeight(editor);
        const auto marker = QStringLiteral("shared 150\n");
        placeResultCursor(fixture, marker, 140 * line);
        const auto revision = [&] { return QString::number(fixture.file().current().id); };
        const auto kept = [&] {
            settleReplacement(fixture);
            CHECK_EQ(editor->property("cursorPosition").toInt(), fixture.text().indexOf(marker));
            CHECK(editor->property("selectionStart").toInt() == editor->property("selectionEnd").toInt());
            CHECK(std::abs(contentY(result) - 140 * line) < 1);
        };
        CHECK(test_support::previewResult(
            controller,
            controller.choices()->previewWholeFile("choices", revision(), ChoicePreviewFlow::Side::LeftSide)));
        CHECK(commandResult(controller, controller.choices()->applyWholeFile()));
        CHECK(fixture.text().contains(QStringLiteral("left first\n"))
              && fixture.text().contains(QStringLiteral("left last\n")));
        kept();
        CHECK(commandResult(controller, controller.commands()->undo("choices", revision())));
        CHECK(fixture.text().contains(QStringLiteral("right first\n"))
              && fixture.text().contains(QStringLiteral("right last\n")));
        kept();
        CHECK(commandResult(controller, controller.commands()->redo("choices", revision())));
        CHECK(fixture.text().contains(QStringLiteral("left first\n"))
              && fixture.text().contains(QStringLiteral("left last\n")));
        kept();
    });
}

}

void comparison_scene::addNavigationCases(Groups groups)
{
    addFileListNavigationCases(groups.scenes);
    addPaneSynchronizationCases(groups.scenes);
    addHistoryPlacementCases(groups);
}
