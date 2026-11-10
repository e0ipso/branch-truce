// Review journeys: the three panes, scoped choices, editing, reading aids and Save.
#include "acceptance/JourneyData.h"
#include "acceptance/JourneySteps.h"
#include "acceptance/JourneySuite.h"
#include "fixtures/QuickScene.h"

#include <QAccessible>
#include <QClipboard>
#include <QGuiApplication>
#include <QKeySequence>
#include <QQuickTextDocument>
#include <QTextBlock>
#include <QTextDocument>

#include <vector>

using namespace branch_truce;
using test_support::readFile;
using test_support::settleFor;

namespace journey {
namespace {

// The selected file's sources sit around its editable result, each named for its role and commit.
void expectSourcesAround(Scene &scene, const Repository &repo, const char *path, const QString &leftLabel,
                         const QString &rightLabel)
{
    clickFile(scene, path);
    expectText(scene.text("selectedPath"), QString::fromLatin1(path), "selected path");
    // These fixture refs point exactly at the verified source commits in both merge and rebase.
    CHECK(scene.app().loader()->stage2Name() == QStringLiteral("main"));
    CHECK(scene.app().loader()->stage3Name() == QStringLiteral("side"));
    expectText(scene.text("stage2Label"), "main", "left version name");
    expectText(scene.text("stage3Label"), "side", "right version name");
    expectText(scene.text("stage2Identity"), leftLabel.section(": ", 0, 0) + " · " + utf8(repo.leftCommit()).left(10),
               "left role and commit");
    expectText(scene.text("stage3Identity"), rightLabel.section(": ", 0, 0) + " · " + utf8(repo.rightCommit()).left(10),
               "right role and commit");
    // The full commit and blob ids stay reachable as accessible text.
    const auto described = [&](const char *name) {
        const auto *face = QAccessible::queryAccessibleInterface(scene.item(name));
        return face ? face->text(QAccessible::Description) : QString();
    };
    for (const auto *name : {"stage2Label", "stage2Identity"}) {
        expectContains(described(name), utf8(repo.leftCommit()), "left commit description");
        expectContains(described(name), utf8(repo.stageObject(2, path)), "left blob description");
    }
    for (const auto *name : {"stage3Label", "stage3Identity"}) {
        expectContains(described(name), utf8(repo.rightCommit()), "right commit description");
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

// --- the full inventory keeps truthful sources ---------------------------------------------

// Git's marker blocks fold into one row each while the buffer keeps them, and the overflow
// menu's toggle shows them without changing the text, decisions or history.
void toggleMarkersKeepsTheWork(Scene &scene)
{
    clickFile(scene, "reading.txt");
    CHECK(scene.text("resultText").startsWith(QStringLiteral("REPEAT one\n"))
          && scene.text("resultText").contains(QStringLiteral("<<<<<<< HEAD\n")));
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
}

// Earlier context is shown on demand and hidden without touching the result or its selection.
void earlierContextKeepsTheResult(Scene &scene, const Repository &repo)
{
    clickFile(scene, "notes.txt");
    const auto before = work(scene, "notes.txt");
    auto *selectedResult = scene.item("resultText");
    CHECK(QMetaObject::invokeMethod(selectedResult, "select", Q_ARG(int, 0), Q_ARG(int, 5)));
    const auto resultSelection = selectedResult->property("selectedText");
    auto *earlier = scene.item("earlierText");
    CHECK(scene.item("showEarlier")->isEnabled() && !earlier->isVisible());
    scene.click("showEarlier");
    settleFor("earlier context shown", [&] { return earlier->isVisible(); });
    expectText(earlier->property("text").toString(), utf8(repo.stageText(1, "notes.txt")), "earlier text");
    expectContains(scene.text("earlierLabel"), "Common earlier version", "earlier label");
    CHECK(earlier->property("readOnly").toBool());
    const auto earlierSize = scene.window()->size();
    for (const int height : {720, 800}) {
        scene.window()->resize(1280, height);
        test_support::waitForLayout(scene.window(), QStringLiteral("earlierViewport"), Scene::settleMs);
        CHECK(scene.item("earlierLabel")->property("font") == scene.item("stage2Label")->property("font"));
        const auto evidence = inspectReadOnlyPane(scene, "earlierText", "earlierLabel");
        Scene::reveal(earlier);
        test_support::retainUiEvidence(scene.window(), QStringLiteral("earlier-context-%1").arg(height), evidence);
        CHECK(work(scene, "notes.txt") == before);
    }
    scene.window()->resize(earlierSize);
    scene.click("showEarlier");
    settleFor("earlier context hidden", [&] { return !earlier->isVisible(); });
    CHECK(work(scene, "notes.txt") == before);
    CHECK(selectedResult->property("selectedText") == resultSelection);
}

// The unsupported deletion stays listed with its reason and no editor, and is never a draft.
void outsideAppFileKeepsItsReason(Scene &scene)
{
    clickFile(scene, "removed.txt");
    CHECK(scene.item("selectedProblem")->isVisible() && !scene.item("resultText")->isVisible());
    expectContains(scene.text("selectedProblem"), "outside Branch Truce", "outside-app reason");
    for (const auto &label : {rowText(scene, "removed.txt"), scene.text("selectedProblem")}) {
        if (label.contains(QStringLiteral("draft"), Qt::CaseInsensitive)
            || label.contains(QStringLiteral("Manual Save checkpoint")))
            throw std::runtime_error("outside-app file labelled as a draft: " + label.toStdString());
    }
    expectContains(scene.item("selectedProblem")->property("explanation").toString(), "git add",
                   "outside-app next step");
    expectContains(rowText(scene, "story.txt"), "Not saved.", "supported row");
}

void fullInventoryKeepsTruthfulSources(Sandbox &sandbox)
{
    Repository repo(GitOperation::Merge, journeyPaths());
    const auto launched = repo.snapshot();
    Scene scene(sandbox, repo);
    auto &app = scene.app();
    CHECK(app.loader()->operation() == QStringLiteral("Merge"));
    expectText(scene.text("repositorySubtitle"), "Merging side into main", "merge operation with branches");
    CHECK(app.files()->totalFiles() == 4 && app.files()->outsideAppFiles() == 1);
    CHECK(!scene.item("inventoryWarning")->isVisible());
    expectContains(scene.text("outsideAppCount"), "1 file to handle outside the app", "outside-app count");
    for (const auto *path : {"story.txt", "notes.txt", "reading.txt", "removed.txt"})
        expectContains(rowText(scene, path), QString::fromLatin1(path), "file row");
    expectContains(rowText(scene, "removed.txt"), "Handle outside the app", "outside-app row");
    const auto left = "Current checkout: " + utf8(repo.leftCommit()).left(10);
    const auto right = "Merged commit: " + utf8(repo.rightCommit()).left(10);
    for (const auto *path : {"story.txt", "notes.txt", "reading.txt"})
        expectSourcesAround(scene, repo, path, left, right);
    // A file edited before launch keeps that content exactly.
    clickFile(scene, "notes.txt");
    expectText(scene.text("resultText"), QString::fromUtf8("notes edited before launch \xc3\xa9\n"), "pre-launch edit");
    CHECK(!scene.text("resultText").contains(QStringLiteral("<<<<<<<")));
    toggleMarkersKeepsTheWork(scene);
    earlierContextKeepsTheResult(scene, repo);
    outsideAppFileKeepsItsReason(scene);

    // Saving the three supported results leaves the session and file warnings.
    for (const auto *path : {"story.txt", "notes.txt", "reading.txt"}) {
        clickFile(scene, path);
        scene.click("saveDraft");
        settleFor(std::string("save of ") + path, [&] { return scene.progress().saved; });
        // Save writes the buffer, folded marker lines included.
        CHECK(scene.file(path).checkpoint()
              && QString::fromStdString(scene.file(path).checkpoint()->draft.text) == scene.text("resultText"));
    }
    CHECK(!scene.item("inventoryWarning")->isVisible());
    CHECK(scene.item("outsideAppCount")->isVisible());
    clickFile(scene, "removed.txt");
    expectContains(scene.item("selectedProblem")->property("explanation").toString(), "outstanding",
                   "outside-app notice");
    CHECK(app.files()->resolvedFiles() == 0);
    CHECK(repo.snapshot() == launched);
    checkOffline(scene);
}

// --- rebasing keeps the arrangement --------------------------------------------------------

void rebaseKeepsTheArrangement(Sandbox &sandbox)
{
    Repository repo(GitOperation::Rebase,
                    {
                        {"story.txt", storyBase, storyLeft, storyRight, std::nullopt},
                        {"added.txt", std::nullopt, QByteArray("added on the left\n"),
                         QByteArray("added on the right\n"), std::nullopt},
                    });
    Scene scene(sandbox, repo);
    CHECK(scene.app().loader()->operation() == QStringLiteral("Rebase"));
    expectText(scene.text("repositorySubtitle"), "Rebasing side onto main", "rebase operation with branches");
    CHECK(scene.app().files()->totalFiles() == 2 && scene.app().files()->outsideAppFiles() == 0);
    const auto left = "Current rebased state: " + utf8(repo.leftCommit()).left(10);
    const auto right = "Commit being replayed: " + utf8(repo.rightCommit()).left(10);
    expectSourcesAround(scene, repo, "story.txt", left, right);
    CHECK(scene.item("showEarlier")->isEnabled());
    expectSourcesAround(scene, repo, "added.txt", left, right);
    const auto before = work(scene, "added.txt");
    auto *toggle = scene.item("showEarlier");
    CHECK(!toggle->isEnabled() && !scene.item("earlierText")->isVisible());
    expectText(toggle->property("text").toString(), "Common earlier version unavailable", "unavailable earlier");
    CHECK(work(scene, "added.txt") == before);
    checkOffline(scene);
}

// --- scoped choices and ordered combinations -----------------------------------------------

QString stateOf(Scene &scene, const char *label) { return scene.choiceRow(QString::fromUtf8(label)).state; }

// Each choice decides its scope; the history records the text and decisions after each one.
void chooseLinesAndSections(Scene &scene, std::vector<Work> &history)
{
    // Replacing pre-launch content asks first; cancelling changes nothing.
    clickChoice(scene, "Left line 2", "Accept");
    CHECK(scene.item("manualReplacementNotice")->isVisible());
    expectText(scene.text("replacementBeforeText"), "base first\n", "replaced scope");
    expectText(scene.text("replacementAfterText"), "left first\n", "replacement");
    clickDialogButton(scene, "replacementDialog", "Cancel");
    CHECK(work(scene, "story.txt") == history.back());
    CHECK(stateOf(scene, "Left line 2") == QStringLiteral("undecided"));

    // A line choice decides only its side; the competitor stays available.
    clickChoice(scene, "Left line 2", "Accept");
    clickDialogButton(scene, "replacementDialog", "Apply choice");
    expectText(scene.text("resultText"), "header\nleft first\nbridge one\nbridge two\nbridge three\nbase later\ntail\n",
               "accept left");
    CHECK(stateOf(scene, "Left line 2") == QStringLiteral("accepted")
          && stateOf(scene, "Right line 2") == QStringLiteral("undecided"));
    CHECK(scene.choiceRow("Right line 2").available);
    CHECK(stateOf(scene, "Left line 6") == QStringLiteral("undecided")
          && stateOf(scene, "Right line 6") == QStringLiteral("undecided"));
    history.push_back(work(scene, "story.txt"));

    // Resolving the other region with one side explicitly rejects its competitor.
    clickChoice(scene, QString::fromUtf8("Right section, lines 6–6"), "Resolve");
    clickDialogButton(scene, "replacementDialog", "Apply choice");
    expectText(scene.text("resultText"),
               "header\nleft first\nbridge one\nbridge two\nbridge three\nright later\ntail\n", "resolve right");
    CHECK(stateOf(scene, "Right line 6") == QStringLiteral("accepted")
          && stateOf(scene, "Left line 6") == QStringLiteral("ignored"));
    CHECK(stateOf(scene, "Left line 2") == QStringLiteral("accepted")
          && stateOf(scene, "Right line 2") == QStringLiteral("undecided"));
    history.push_back(work(scene, "story.txt"));

    // Ignore decides against a contribution without changing the result.
    clickChoice(scene, "Right line 2", "Ignore");
    CHECK(!scene.object("replacementDialog")->property("visible").toBool());
    CHECK(work(scene, "story.txt").shown == history.back().shown);
    CHECK(stateOf(scene, "Right line 2") == QStringLiteral("ignored")
          && stateOf(scene, "Left line 2") == QStringLiteral("accepted"));
    history.push_back(work(scene, "story.txt"));
    CHECK(!(history.back() == history[history.size() - 2]));
}

// An inspectable order: preview Right then Left, cancel without a trace, then apply it.
void previewAndApplyOrder(Scene &scene, std::vector<Work> &history)
{
    auto *orderedResult = scene.item("resultText");
    CHECK(QMetaObject::invokeMethod(orderedResult, "select", Q_ARG(int, 0), Q_ARG(int, 6)));
    const auto orderedSelection = orderedResult->property("selectedText");
    clickChoice(scene, QString::fromUtf8("Right section, lines 2–2"), "PreviewRightFirst");
    expectText(scene.object("replacementDialog")->property("title").toString(), "Preview Right then Left",
               "order title");
    expectText(scene.text("replacementAfterText"), "right first\nleft first\n", "ordered preview");
    const auto ordered = work(scene, "story.txt");
    const auto orderedRevision = scene.file("story.txt").current().id;
    const bool orderedUndo = scene.file("story.txt").canUndo();
    const bool orderedRedo = scene.file("story.txt").canRedo();
    const auto previewSize = scene.window()->size();
    for (const int height : {720, 800}) {
        scene.window()->resize(1280, height);
        test_support::waitForLayout(scene.window(), QStringLiteral("replacementAfterText"), Scene::settleMs);
        CHECK(scene.item("replacementBeforeLabel")->property("font")
              == scene.item("replacementAfterLabel")->property("font"));
        inspectReadOnlyPane(scene, "replacementBeforeText", "replacementBeforeLabel");
        const auto evidence = inspectReadOnlyPane(scene, "replacementAfterText", "replacementAfterLabel");
        test_support::retainUiEvidence(scene.window(), QStringLiteral("ordered-choice-preview-%1").arg(height),
                                       evidence);
        CHECK(work(scene, "story.txt") == ordered);
    }
    clickDialogButton(scene, "replacementDialog", "Cancel");
    CHECK(work(scene, "story.txt") == ordered && scene.file("story.txt").current().id == orderedRevision);
    CHECK(scene.file("story.txt").canUndo() == orderedUndo && scene.file("story.txt").canRedo() == orderedRedo);
    CHECK(orderedResult->property("selectedText") == orderedSelection);
    scene.window()->resize(previewSize);
    clickChoice(scene, QString::fromUtf8("Right section, lines 2–2"), "PreviewRightFirst");
    clickDialogButton(scene, "replacementDialog", "Apply choice");
    expectText(scene.text("resultText"),
               "header\nright first\nleft first\nbridge one\nbridge two\nbridge three\nright later\ntail\n",
               "ordered combination");
    CHECK(stateOf(scene, "Right line 2") == QStringLiteral("accepted")
          && stateOf(scene, "Left line 2") == QStringLiteral("accepted"));
    history.push_back(work(scene, "story.txt"));
}

// Undo by shortcut and button walks back through text and decisions together, redo by every
// platform binding replays them, and the shared actions agree with the toolbar.
void undoAndRedoWalkTheHistory(Scene &scene, const std::vector<Work> &history, const Work &typed)
{
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
        if (step % 2)
            keyClick(scene, Qt::Key_Z, Qt::ControlModifier);
        else
            scene.click("undoCommand");
        CHECK(work(scene, "story.txt") == history[step]);
    }
    CHECK(!scene.file("story.txt").canUndo() && !scene.item("undoCommand")->isEnabled());
    for (std::size_t step = 1; step < history.size(); ++step) {
        if (step % 2) {
            // TextArea handles all platform redo bindings when it has focus.
            clickText(scene, scene.item("resultText"));
            CHECK(scene.item("resultText")->hasActiveFocus());
            keyClick(scene, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        } else
            scene.click("redoCommand");
        CHECK(work(scene, "story.txt") == history[step]);
    }
    scene.click("redoCommand");
    clickText(scene, scene.item("resultText"));
    CHECK(scene.item("resultText")->hasActiveFocus());
    keyClick(scene, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    CHECK(work(scene, "story.txt") == typed && !scene.file("story.txt").canRedo());
    CHECK(!scene.item("redoCommand")->isEnabled());
    CHECK(!sharedRedo->property("enabled").toBool() && !menuRedo->property("enabled").toBool());
}

void scopedChoicesShareOneHistory(Sandbox &sandbox)
{
    Repository repo(GitOperation::Merge, journeyPaths());
    Scene scene(sandbox, repo);
    clickFile(scene, "notes.txt");
    const auto notes = scene.file("notes.txt").current();
    clickFile(scene, "story.txt");
    std::vector<Work> history{work(scene, "story.txt")};
    chooseLinesAndSections(scene, history);
    previewAndApplyOrder(scene, history);
    // Direct typing joins the same history.
    appendToResult(scene, "ok");
    expectText(scene.text("resultText"),
               "header\nright first\nleft first\nbridge one\nbridge two\nbridge three\nright later\ntail\nok", "typed");
    CHECK(scene.file("story.txt").current().draft.decisions == history.back().draft.decisions);
    undoAndRedoWalkTheHistory(scene, history, work(scene, "story.txt"));
    // Other files are untouched.
    CHECK(scene.file("notes.txt").current().id == notes.id && scene.file("notes.txt").current().draft == notes.draft);
    checkOffline(scene);
}

// --- typing in an accepted line marks its region manual ------------------------------------

void typingMarksTheRegionManual(Sandbox &sandbox)
{
    Repository repo(GitOperation::Merge, journeyPaths());
    Scene scene(sandbox, repo);
    clickFile(scene, "story.txt");
    CHECK(scene.app().selectedFile()->fileName() == QStringLiteral("story.txt"));
    const auto region = [&](const char *label) { return scene.regionFor(QString::fromUtf8(label)); };

    // The launch result differs from both sides, but nobody has typed in it yet.
    const auto launched = scene.regions();
    CHECK(!launched.empty());
    for (const auto &each : launched) {
        CHECK(!each.handEdited);
        CHECK(each.state != QStringLiteral("manual"));
    }

    clickChoice(scene, "Left line 2", "Accept");
    clickDialogButton(scene, "replacementDialog", "Apply choice");
    CHECK(region("Left line 2").state == QStringLiteral("accepted"));
    CHECK(!region("Left line 2").handEdited);
    expectText(regionResult(scene, region("Left line 2")), "left first\n", "accepted range");

    // Typing inside the accepted line in the editor marks only that region.
    auto *editor = scene.item("resultText");
    clickText(scene, editor);
    CHECK(editor->hasActiveFocus());
    keyClick(scene, Qt::Key_Home, Qt::ControlModifier);
    keyClick(scene, Qt::Key_Down);
    for (int step = 0; step < 4; ++step) keyClick(scene, Qt::Key_Right);
    typeText(scene, " hand");
    expectText(scene.text("resultText"),
               "header\nleft hand first\nbridge one\nbridge two\nbridge three\nbase later\ntail\n", "typed");
    const auto typed = work(scene, "story.txt");
    CHECK(region("Left line 2").state == QStringLiteral("manual"));
    CHECK(region("Left line 2").handEdited);
    expectText(regionResult(scene, region("Left line 2")), "left hand first\n", "edited range");
    CHECK(!region("Left line 6").handEdited && !region("Right line 6").handEdited);
    CHECK(region("Left line 6").state == QStringLiteral("undecided"));

    // An ordered combination over that line replaces the typed text and clears the mark.
    clickChoice(scene, QString::fromUtf8("Right section, lines 2–2"), "PreviewLeftFirst");
    CHECK(scene.item("manualReplacementNotice")->isVisible());
    expectText(scene.text("replacementBeforeText"), "left hand first\n", "replaced edit");
    clickDialogButton(scene, "replacementDialog", "Apply choice");
    expectText(scene.text("resultText"),
               "header\nleft first\nright first\nbridge one\nbridge two\nbridge three\nbase later\ntail\n", "combined");
    CHECK(!region("Left line 2").handEdited && !region("Right line 2").handEdited);
    CHECK(region("Left line 2").state == QStringLiteral("accepted"));
    CHECK(region("Right line 2").state == QStringLiteral("accepted"));

    // Undo brings back the typed text together with its mark.
    scene.click("undoCommand");
    CHECK(work(scene, "story.txt") == typed);
    CHECK(region("Left line 2").handEdited);
    CHECK(region("Left line 2").state == QStringLiteral("manual"));
    checkOffline(scene);
}

// --- Save keeps an app-only draft and Mark resolved ----------------------------------------

void saveKeepsAnAppOnlyDraftAndMarkResolvedPublishes(Sandbox &sandbox)
{
    Repository repo(GitOperation::Merge, journeyPaths());
    const auto launched = repo.snapshot();
    Scene scene(sandbox, repo);
    auto &app = scene.app();

    // Save checkpoints an unfinished draft in the app only.
    clickFile(scene, "notes.txt");
    appendToResult(scene, "draft");
    CHECK(!scene.progress().saved && !scene.progress().reviewed && !scene.progress().resolved);
    scene.click("saveDraft");
    settleFor("notes saved", [&] { return scene.progress().saved; });
    CHECK(scene.progress().saved && !scene.progress().reviewed && !scene.progress().resolved);
    CHECK(scene.progress().undecided > 0);
    CHECK(repo.snapshot() == launched);
    CHECK(!repo.unmerged("notes.txt").isEmpty());

    // Mark resolved with undecided choices asks for confirmation; cancelling keeps everything.
    clickFile(scene, "story.txt");
    appendToResult(scene, "manual");
    const auto edited = work(scene, "story.txt");
    CHECK(!scene.progress().saved);
    scene.click("markResolved");
    settleFor("resolution confirmation", [&] { return app.commands()->resolutionConfirmation(); });
    expectContains(app.feedback()->message(), "intentional", "confirmation message");
    scene.click("cancelResolution");
    CHECK(!app.commands()->resolutionConfirmation());
    CHECK(work(scene, "story.txt") == edited);
    CHECK(!scene.progress().reviewed && !scene.progress().saved);
    CHECK(repo.snapshot() == launched);

    // Confirming saves, writes and resolves the displayed result without a prior Save.
    markResolvedByMouse(scene);
    const auto progress = scene.progress();
    if (!progress.resolved) throw std::runtime_error("not resolved: " + app.feedback()->message().toStdString());
    CHECK(progress.saved && progress.reviewed);
    CHECK(progress.undecided > 0);
    expectContains(rowText(scene, "story.txt"), "Resolved", "resolved status");
    CHECK(work(scene, "story.txt") == edited);
    CHECK(readFile(repo.path("story.txt")) == edited.shown.toUtf8());
    CHECK(repo.unmerged("story.txt").isEmpty());
    const auto staged = repo.git({"ls-files", "-s", "--", "story.txt"});
    CHECK(staged.startsWith("100644 " + repo.git({"hash-object", "--", "story.txt"}).trimmed() + " 0\t"));
    CHECK(app.files()->resolvedFiles() == 1);
    // Only that file changed; the saved draft stays off the repository.
    CHECK(readFile(repo.path("notes.txt")) == launched.value("notes.txt"));
    CHECK(!repo.unmerged("notes.txt").isEmpty() && !repo.unmerged("reading.txt").isEmpty());

    // Editing again makes a newer draft that invalidates review and is distinguished from Git's copy.
    appendToResult(scene, "+");
    const auto newer = scene.progress();
    CHECK(!newer.reviewed && newer.earlierResolved);
    expectContains(scene.text("staleStagedNotice"), "Earlier result staged; current draft not applied",
                   "newer draft status");
    CHECK(readFile(repo.path("story.txt")) == edited.shown.toUtf8());
    checkOffline(scene);
}

// --- scoped search, wrapping, whitespace and clipboard -------------------------------------

// What every reading aid must leave alone: the result, its history and both sources.
struct Untouched {
    Scene &scene;
    const Repository &repo;
    Work before;
    void check() const
    {
        CHECK(work(scene, "reading.txt") == before && !scene.file("reading.txt").canUndo());
        expectText(scene.text("stage2Text"), utf8(repo.stageText(2, "reading.txt")), "left source");
        expectText(scene.text("stage3Text"), utf8(repo.stageText(3, "reading.txt")), "right source");
    }
};

// The result's selection, which the reading aids after the search must keep.
struct Selection {
    int start = 0;
    int end = 0;
};

// Find walks the scoped pane over repeated passages, wraps to the first match, keeps its query
// across closing, and moves to the result when the scope is chosen with the mouse. Returns the
// result match it leaves selected.
Selection searchWalksTheScopedPane(Scene &scene, const Untouched &untouched)
{
    auto *left = scene.item("stage2Text");
    auto *result = scene.item("resultText");
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
    untouched.check();
    // Choose the result scope with the mouse.
    scene.click("searchScope");
    auto *scopePopup = scene.item("searchScope")->property("popup").value<QObject *>();
    CHECK(scopePopup);
    settleFor("search scope list", [&] { return scopePopup->property("opened").toBool(); });
    auto *option = stable(scene, "Editable result option", [&] {
        return test_support::namedButton(test_support::popupRoot(scopePopup), QStringLiteral("Editable result"));
    });
    test_support::clickItem(scene.window(), option);
    settleFor("result scope", [&] {
        return scene.item("searchScope")->property("currentText").toString() == QStringLiteral("Editable result");
    });
    scene.click("findPassage");
    expectContains(scene.text("searchStatus"), "Passage in Editable result.", "result status");
    CHECK(result->property("selectionStart").toInt() == 0);
    expectText(result->property("selectedText").toString(), "REPEAT", "result match");
    const Selection selection{result->property("selectionStart").toInt(), result->property("selectionEnd").toInt()};
    untouched.check();
    return selection;
}

// Wrapping changes the long line's layout only.
void wrappingChangesLayoutOnly(Scene &scene, const Untouched &untouched)
{
    auto *left = scene.item("stage2Text");
    auto *result = scene.item("resultText");
    const int longBlock = 1;
    CHECK(test_support::visualLines(result, longBlock) == 1 && test_support::visualLines(left, longBlock) == 1);
    scene.click("wrapLines");
    settleFor("wrapped long line", [&] {
        return test_support::visualLines(result, longBlock) > 1 && test_support::visualLines(left, longBlock) > 1;
    });
    untouched.check();
    scene.click("wrapLines");
    settleFor("unwrapped long line", [&] { return test_support::visualLines(result, longBlock) == 1; });
    untouched.check();
}

// Space and tab markers paint the whitespace-only difference and keep the selection.
void whitespaceMarkersKeepTheSelection(Scene &scene, const Untouched &untouched, const Selection &selection)
{
    auto *result = scene.item("resultText");
    // The whitespace differences are inside Git's folded conflict block. Reveal the block through
    // the toolbar before inspecting its spaces and tab.
    expectMarkersFolded(scene, true);
    toggleConflictMarkers(scene);
    expectMarkersFolded(scene, false);
    untouched.check();
    auto *markers = scene.item("resultWhitespace");
    CHECK(test_support::paintedMarkers(markers) == 0);
    scene.click("showWhitespace");
    const auto *resultDocument = result->property("textDocument").value<QQuickTextDocument *>()->textDocument();
    QString shown;
    for (auto block = resultDocument->begin(); block.isValid(); block = block.next())
        if (block.isVisible()) shown += block.text() + QLatin1Char('\n');
    CHECK(shown.count(' ') == untouched.before.shown.count(' ')
          && shown.count('\t') == untouched.before.shown.count('\t'));
    CHECK(shown.count('\t') == 1 && shown.contains(QStringLiteral("space \n")));
    settleFor("whitespace markers", [&] {
        return test_support::markerKind(markers, true) == shown.count('\t')
            && test_support::markerKind(markers, false) == shown.count(' ')
            && test_support::paintedMarkers(markers) == shown.count('\t') + shown.count(' ');
    });
    CHECK(test_support::markerKind(scene.item("stage2Whitespace"), true) == 0);
    CHECK(test_support::markerKind(scene.item("stage3Whitespace"), true) == 1);
    CHECK(result->property("selectionStart").toInt() == selection.start
          && result->property("selectionEnd").toInt() == selection.end);
    untouched.check();
    scene.click("showWhitespace");
    settleFor("whitespace hidden", [&] { return test_support::paintedMarkers(markers) == 0; });
    CHECK(result->property("selectionStart").toInt() == selection.start
          && result->property("selectionEnd").toInt() == selection.end);
    untouched.check();
}

// Sources take mouse selection but not typing; the result takes the clipboard and history keys.
void clipboardAndHistoryKeys(Scene &scene, const Untouched &untouched)
{
    auto *left = scene.item("stage2Text");
    auto *result = scene.item("resultText");
    clickText(scene, left);
    CHECK(left->hasActiveFocus());
    typeText(scene, "x");
    untouched.check();
    clickText(scene, result);
    CHECK(result->hasActiveFocus());
    keyClick(scene, Qt::Key_Home, Qt::ControlModifier);
    keyClick(scene, Qt::Key_End, Qt::ShiftModifier);
    expectText(result->property("selectedText").toString(), "REPEAT one", "keyboard selection");
    QGuiApplication::clipboard()->clear();
    keyClick(scene, Qt::Key_C, Qt::ControlModifier);
    expectText(QGuiApplication::clipboard()->text(), "REPEAT one", "copied");
    untouched.check();
    keyClick(scene, Qt::Key_End, Qt::ControlModifier);
    keyClick(scene, Qt::Key_V, Qt::ControlModifier);
    const auto pasted = work(scene, "reading.txt");
    expectText(pasted.shown, untouched.before.shown + "REPEAT one", "pasted");
    CHECK(pasted.draft.decisions == untouched.before.draft.decisions);
    keyClick(scene, Qt::Key_Z, Qt::ControlModifier);
    CHECK(work(scene, "reading.txt") == untouched.before);
    keyClick(scene, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    CHECK(work(scene, "reading.txt") == pasted);
    scene.click("undoCommand");
    CHECK(work(scene, "reading.txt") == untouched.before);
}

void readingAidsKeepTheWork(Sandbox &sandbox)
{
    Repository repo(GitOperation::Merge, journeyPaths());
    const auto launched = repo.snapshot();
    Scene scene(sandbox, repo);
    clickFile(scene, "story.txt");
    const auto story = scene.file("story.txt").current();
    clickFile(scene, "reading.txt");
    const Untouched untouched{scene, repo, work(scene, "reading.txt")};
    const auto selection = searchWalksTheScopedPane(scene, untouched);
    wrappingChangesLayoutOnly(scene, untouched);
    whitespaceMarkersKeepTheSelection(scene, untouched, selection);
    clipboardAndHistoryKeys(scene, untouched);
    CHECK(scene.file("story.txt").current().id == story.id && scene.file("story.txt").current().draft == story.draft);
    CHECK(repo.snapshot() == launched);
    QGuiApplication::clipboard()->clear();
    checkOffline(scene);
}
} // namespace

void addReviewJourneys(JourneySuite &suite)
{
    const auto add = [&](const char *name, JourneySuite::Journey journey) {
        suite.add("review", name, std::move(journey));
    };
    add("PRD.md:127 PRD.md:139 PRD.md:157 the full inventory keeps truthful sources around the preserved editable "
        "result",
        fullInventoryKeepsTruthfulSources);
    add("PRD.md:139 rebasing keeps the arrangement with replay identities and explains missing earlier context",
        rebaseKeepsTheArrangement);
    add("PRD.md:157 scoped choices and ordered combinations over pre-launch edits share one undo history with typing",
        scopedChoicesShareOneHistory);
    add("PRD.md:41 PRD.md:49 typing in an accepted line marks its region manual until a choice replaces it",
        typingMarksTheRegionManual);
    add("PRD.md:135 PRD.md:153 Save keeps an app-only draft and Mark resolved without a prior Save confirms, writes "
        "and resolves",
        saveKeepsAnAppOnlyDraftAndMarkResolvedPublishes);
    add("PRD.md:137 PRD.md:141 scoped search, wrapping, whitespace, mouse inspection and clipboard shortcuts keep the "
        "work",
        readingAidsKeepTheWork);
}

} // namespace journey
