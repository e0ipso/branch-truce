// Handoff journeys: the handoff and leaving, staged drafts, startup recovery and git mergetool.
#include "acceptance/JourneyData.h"
#include "acceptance/JourneySteps.h"
#include "acceptance/JourneySuite.h"
#include "fixtures/Await.h"
#include "fixtures/QuickScene.h"
#include "fixtures/SyncFaults.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QMap>
#include <QProcess>

#include <csignal>

using test_support::awaitAligned;

using namespace branch_truce;
using test_support::readFile;
using test_support::settleFor;

namespace journey {
namespace {

// --- a newer draft goes back to the staged result ------------------------------------------

void newerDraftReturnsToTheStagedResult(Sandbox &sandbox)
{
    Repository repo({chosenByHand("newer.txt")});
    const auto faults = std::make_shared<test_support::SyncFaults>();
    Scene scene(sandbox, repo, faults);
    auto &app = scene.app();
    const auto id = fileId("newer.txt");
    clickFile(scene, "newer.txt");
    markResolvedByMouse(scene);
    CHECK(scene.progress().resolved);
    const auto staged = scene.text("resultText");
    const auto published = repo.snapshot();
    CHECK(!repo.unmerged("newer.txt"));
    auto *prompt = scene.object("discardStagedDialog");
    auto *handoff = scene.object("handoffDialog");
    const auto newerDraft = [&] {
        appendToResult(scene, "again");
        settleFor("newer draft", [&] { return scene.progress().earlierResolved; });
        CHECK(scene.text("resultText").endsWith(QStringLiteral("again")));
        test_support::awaitRecovery(*app.store());
        CHECK(!app.store()->hasPendingRecovery());
        CHECK(QString::fromStdString(scene.file("newer.txt").current().draft.text) == scene.text("resultText"));
    };
    newerDraft();
    const auto draft = scene.text("resultText");

    // The overflow entry asks first. The prompt names the file, says the draft is lost and
    // confirms with the destructive Discard button, labelled with what it does.
    clickOverflowAction(scene, "discardToStaged");
    settleFor("discard prompt opened", [&] { return prompt->property("opened").toBool(); });
    expectContains(prompt->property("subtitle").toString(), "The draft of newer.txt is lost", "discard prompt");
    CHECK(static_cast<unsigned int>(prompt->property("standardButtons").toInt())
          & static_cast<unsigned int>(QDialogButtonBox::Discard));
    expectText(dialogButton(scene, "discardStagedDialog", "confirmDiscardStaged")->property("text").toString(),
               "Discard draft", "destructive button");
    CHECK(!scene.item("discardStagedError")->isVisible());
    // Cancelling changes nothing.
    clickDialogButton(scene, "discardStagedDialog", "Cancel");
    expectText(scene.text("resultText"), draft, "draft after cancel");
    CHECK(scene.progress().earlierResolved && !scene.progress().resolved);
    CHECK(repo.snapshot() == published);

    // The handoff entry that says to discard back to the staged result offers it.
    scene.click("openHandoff");
    settleFor("handoff opened", [&] { return handoff->property("opened").toBool(); });
    const auto entry = scene.handoffEntries().value(id);
    expectText(entry.category, "newer-draft", "handoff category");
    expectText(entry.actionLabel, "Mark resolved or discard back to the staged result", "route");
    const auto discardName = ("handoffDiscardStaged-" + id).toLatin1();
    expectText(scene.text(discardName.constData()), "Discard draft and use staged result", "handoff discard");
    scene.click(discardName.constData());
    settleFor("handoff closed for discard", [&] { return !handoff->property("visible").toBool(); });
    clickDialogButton(scene, "discardStagedDialog", "confirmDiscardStaged");
    settleFor("staged result restored", [&] { return scene.progress().resolved; });
    expectText(scene.text("resultText"), staged, "staged result");
    CHECK(!scene.progress().earlierResolved);
    expectText(app.feedback()->message(),
               "Discarded the draft. The result staged in Git is current again; the repository file is unchanged.",
               "discard message");
    expectText(scene.handoffEntries().value(id).category, "published", "handoff after discard");
    CHECK(scene.handoffClean());
    CHECK(repo.snapshot() == published);

    // A discard the recovery journal cannot record keeps the draft and shows an error.
    newerDraft();
    {
        const test_support::ArmedFault failure(*faults, test_support::SyncFaults::Point::JournalSync);
        clickOverflowAction(scene, "discardToStaged");
        test_support::clickItem(scene.window(), dialogButton(scene, "discardStagedDialog", "confirmDiscardStaged"));
        settleFor("discard failure shown", [&] { return scene.item("discardStagedError")->isVisible(); });
        CHECK(faults->failures == 1);
        CHECK(prompt->property("opened").toBool());
        expectText(scene.item("discardStagedError")->property("text").toString(),
                   "Discard was not completed. The draft is kept until retry finishes the discard.", "discard error");
        CHECK(scene.item("discardStagedError")->property("type").toInt() == 3); // Kirigami.MessageType.Error
        CHECK(app.files()->hasPendingRecovery());
        expectText(QString::fromStdString(scene.file("newer.txt").current().draft.text), draft, "kept draft");
        clickDialogButton(scene, "discardStagedDialog", "Cancel");
        CHECK(scene.item("recoveryProblem")->isVisible());
    }
    // Retry finishes that same discard.
    scene.click("retryRecovery");
    settleFor("retried discard", [&] { return !app.files()->hasPendingRecovery(); });
    settleFor("staged result after retry", [&] { return scene.progress().resolved; });
    expectText(QString::fromStdString(scene.file("newer.txt").current().draft.text), staged, "staged after retry");
    CHECK(repo.snapshot() == published);
    checkOffline(scene);
}

// --- startup recovery ----------------------------------------------------------------------

std::vector<PathSpec> savedAndTypedPaths()
{
    return {{"saved.txt", QByteArray("saved base\n"), QByteArray("saved left\n"), QByteArray("saved right\n"),
             QByteArray("saved by hand\n")},
            {"typed.txt", QByteArray("typed base\n"), QByteArray("typed left\n"), QByteArray("typed right\n"),
             QByteArray("typed by hand\n")}};
}

// A new window restores the saved draft and the one never saved, keeps their status and still
// takes edits.
void reopenedWindowRestoresBothDrafts(Sandbox &sandbox, const Repository &repo, const QString &savedText,
                                      const QString &typedText)
{
    Scene reopened(sandbox, repo);
    expectContains(reopened.app().loader()->detail(), "Recovered app drafts", "recovery detail");
    clickFile(reopened, "saved.txt");
    expectText(reopened.text("resultText"), savedText, "restored saved draft");
    CHECK(reopened.progress().saved && !reopened.progress().resolved);
    clickFile(reopened, "typed.txt");
    expectText(reopened.text("resultText"), typedText, "restored unsaved draft");
    CHECK(!reopened.progress().saved && !reopened.progress().resolved);
    // Restored work stays editable offline.
    appendToResult(reopened, "!");
    expectText(reopened.text("resultText"), typedText + "!", "edit after resume");
    CHECK(repo.unmerged("saved.txt") && repo.unmerged("typed.txt"));
    checkOffline(reopened);
}

void offlineStartupResumesDraftsAfterClosing(Sandbox &sandbox)
{
    Repository repo(savedAndTypedPaths());
    const auto launched = repo.snapshot();
    QString savedText;
    QString typedText;
    {
        Scene scene(sandbox, repo);
        checkOffline(scene);
        clickFile(scene, "saved.txt");
        appendToResult(scene, "kept");
        scene.click("saveDraft");
        settleFor("saved draft", [&] { return scene.progress().saved; });
        savedText = scene.text("resultText");
        clickFile(scene, "typed.txt");
        appendToResult(scene, "typed");
        typedText = scene.text("resultText");
        // Closing with work outstanding opens the handoff; leave is deliberate.
        auto *dialog = scene.object("handoffDialog");
        scene.window()->close();
        settleFor("handoff opened by close", [&] { return dialog->property("opened").toBool(); });
        CHECK(scene.window()->isVisible());
        scene.click("handoffLeave");
        CHECK(!scene.app().completion()->outcome().isEmpty());
        checkOffline(scene);
    }
    CHECK(repo.snapshot() == launched);
    reopenedWindowRestoresBothDrafts(sandbox, repo, savedText, typedText);
}

// A window that ends without the handoff, as the app does on SIGTERM or SIGINT, leaves its
// typed input to the next window.
void sessionEndedWithoutHandoffRestoresTheDraft(Sandbox &sandbox)
{
    Repository repo(savedAndTypedPaths());
    const auto launched = repo.snapshot();
    QString typedText;
    {
        Scene scene(sandbox, repo);
        clickFile(scene, "typed.txt");
        appendToResult(scene, "typed");
        typedText = scene.text("resultText");
        settleFor("the typed input recorded", [&] { return !scene.app().files()->hasPendingRecovery(); });
        CHECK(scene.app().completion()->outcome().isEmpty());
        checkOffline(scene);
    }
    CHECK(repo.snapshot() == launched);
    Scene reopened(sandbox, repo);
    expectContains(reopened.app().loader()->detail(), "Recovered app drafts", "recovery detail");
    clickFile(reopened, "typed.txt");
    expectText(reopened.text("resultText"), typedText, "restored unsaved draft");
    CHECK(!reopened.progress().saved && !reopened.progress().resolved);
    clickFile(reopened, "saved.txt");
    expectText(reopened.text("resultText"), QStringLiteral("saved by hand\n"), "untouched file");
    CHECK(repo.unmerged("saved.txt") && repo.unmerged("typed.txt"));
    checkOffline(reopened);
}

void blockedStartupRestoresTheChosenSession(Sandbox &sandbox)
{
    Repository repo({chosenByHand("shared.txt")});
    const auto id = fileId("shared.txt").toStdString();
    const auto edit = [&](Application &controller, const std::string &text) {
        const auto *file = controller.store()->file(id);
        CHECK(
            file
            && awaitAligned(*controller.store(), controller.store()->editResult(id, file->current().id, {{0, 0}, text}))
                == Transition::Applied);
    };
    QString first;
    QString second;
    {
        // Two windows on one merge each keep their own edits.
        Scene one(sandbox, repo);
        Application two{sandbox.config(repo)};
        CHECK(two.loader()->openRepository(repo.root));
        // The load runs Git and the recovery restore off the GUI thread, so it gets the deadline of
        // a process, not the layout deadline of settleFor.
        waitFor(
            "the second window", [&] { return !two.loader()->opening(); }, [&] { return loaderState(*two.loader()); });
        CHECK(two.loader()->startupState() == SessionLoader::StartupState::Ready);
        edit(one.app(), "first window\n");
        edit(two, "second window\n");
        first = one.app().store()->recoveryDirectory();
        second = two.store()->recoveryDirectory();
    }
    const auto firstName = QFileInfo(first).fileName();
    const auto secondName = QFileInfo(second).fileName();
    Scene scene(sandbox, repo);
    auto &app = scene.app();
    expectContains(app.loader()->detail(), "restoration is blocked rather than guessing", "blocked detail");
    CHECK(scene.text("resultText").startsWith(QStringLiteral("shared chosen by hand")));
    for (const auto &name : {firstName, secondName}) {
        auto *row = test_support::waitForLayout(scene.window(), "recoveryChoiceText-" + name);
        const auto shown = row->property("text").toString();
        expectContains(shown, name, "choice session");
        expectContains(shown, "last changed", "choice time");
        expectContains(shown, "Edited files: shared.txt", "choice files");
    }
    const auto repository = repo.snapshot();
    // Recovery selection refuses while this window is still recording its launch captures.
    test_support::awaitRecovery(*app.store());
    scene.click(("recoveryChoice-" + firstName).toLatin1().constData());
    waitFor(
        "the chosen session to be restored",
        [&] { return !app.loader()->opening() && app.loader()->recoveryChoices().isEmpty(); },
        [&] { return loaderState(*app.loader()); });
    CHECK(app.loader()->startupState() == SessionLoader::StartupState::Ready);
    CHECK(app.store()->recoveryDirectory() == first);
    expectContains(app.loader()->detail(), secondName + " was set aside, not deleted", "set aside");
    settleFor("restored text", [&] { return scene.text("resultText").startsWith(QStringLiteral("first window\n")); });
    CHECK(!scene.item("recoveryChoices")->isVisible());
    CHECK(QFileInfo::exists(second + "/set-aside") && QFileInfo::exists(second + "/journal"));
    // Choosing touched only app-owned recovery files.
    CHECK(repo.snapshot() == repository && repo.unmerged("shared.txt"));
    checkOffline(scene);
}

// --- git mergetool -------------------------------------------------------------------------

constexpr QLatin1StringView kWaitText("Waiting for Git to launch this path. It is not resolved.");

std::function<std::string()> mergetoolState(Scene &scene)
{
    return [&scene] {
        auto &app = scene.app();
        return "waiting=" + app.mergetoolSession()->waitingPath().toStdString()
            + " staging=" + app.mergetool()->parentStagingPath().toStdString() + " selected="
            + app.selection()->fileId().toStdString() + " message=" + app.feedback()->message().toStdString();
    };
}

void waitForGitToLaunch(Scene &scene, const char *path)
{
    waitFor(
        std::string("Git to launch ") + path,
        [&] { return scene.app().mergetoolSession()->waitingPath() == QByteArray(path); }, mergetoolState(scene));
}

void waitForGitToExit(Scene &scene, QProcess &parent)
{
    waitFor("git mergetool to exit", [&] { return parent.state() == QProcess::NotRunning; }, mergetoolState(scene));
}

void resolvedHandoffDropsTheWaitingText(Sandbox &sandbox)
{
    Repository repo({chosenByHand("launched.txt"), chosenByHand("later.txt")});
    Scene scene(sandbox, repo);
    auto &app = scene.app();
    auto parent = startMergetool(sandbox, repo, {"launched.txt"});
    waitForGitToLaunch(scene, "launched.txt");
    CHECK(app.mergetool()->parentSessionActive());

    // A path Git has not launched yet says so while Git is still running.
    clickFile(scene, "later.txt");
    settleFor("waiting text on the unlaunched path", [&] { return scene.item("mergetoolWait")->isVisible(); });
    expectText(scene.text("mergetoolWait"), QString(kWaitText), "unlaunched path");
    clickFile(scene, "launched.txt");
    settleFor("holding text on the launched path", [&] { return scene.item("mergetoolHolding")->isVisible(); });
    CHECK(!scene.item("mergetoolWait")->isVisible());

    markResolvedByMouse(scene);
    waitForGitToExit(scene, *parent);
    CHECK(parent->exitStatus() == QProcess::NormalExit && parent->exitCode() == 0);
    CHECK(!repo.unmerged("launched.txt") && repo.unmerged("later.txt"));
    waitFor(
        "the staged resolution",
        [&] { return scene.progress().resolved && app.mergetool()->parentStagingPath().isEmpty(); },
        mergetoolState(scene));
    CHECK(app.files()->resolvedFiles() == 1);

    // Git has exited and the parent session stays open until the user ends it.
    CHECK(app.mergetool()->parentSessionActive() && !app.mergetool()->waiting());
    CHECK(!scene.item("mergetoolWait")->isVisible());
    const auto resolvedTexts = shownTexts(scene);
    if (resolvedTexts.contains(QStringLiteral("It is not resolved"))
        || resolvedTexts.contains(QStringLiteral("Waiting for Git to launch")))
        throw std::runtime_error("resolved file still says it waits: " + resolvedTexts.toStdString());
    expectContains(rowText(scene, "launched.txt"), "Resolved", "resolved file status");
    // Editing after the handoff leaves an earlier result staged, and Git will not launch the path again.
    appendToResult(scene, "x");
    settleFor("earlier staged result", [&] { return scene.progress().earlierResolved; });
    CHECK(!scene.item("mergetoolWait")->isVisible());

    // The file Git never launched keeps the waiting text after the handoff.
    clickFile(scene, "later.txt");
    settleFor("waiting text after the handoff", [&] { return scene.item("mergetoolWait")->isVisible(); });
    expectText(scene.text("mergetoolWait"), QString(kWaitText), "unlaunched path after the handoff");
    CHECK(scene.item("endParentSession")->isVisible());
    checkOffline(scene);
}

void reselectingTheLaunchedPathOffersLeave(Sandbox &sandbox)
{
    Repository repo({chosenByHand("launched.txt"), chosenByHand("other.txt")});
    Scene scene(sandbox, repo);
    auto &app = scene.app();
    const auto waitingShown = [&] {
        return scene.item("mergetoolHolding")->isVisible() && scene.item("leaveMergetool")->isVisible()
            && !scene.item("mergetoolWait")->isVisible();
    };
    clickFile(scene, "launched.txt");
    const auto launchBytes = readFile(repo.path("launched.txt"));
    auto parent = startMergetool(sandbox, repo, {"launched.txt"});
    waitForGitToLaunch(scene, "launched.txt");
    settleFor("waiting state on the selected launched path", waitingShown);

    // Git is not waiting on the other file, so it has no Leave control.
    clickFile(scene, "other.txt");
    settleFor("waiting text on the other file", [&] { return scene.item("mergetoolWait")->isVisible(); });
    CHECK(!app.mergetool()->waiting());
    CHECK(!scene.item("mergetoolHolding")->isVisible() && !scene.item("leaveMergetool")->isVisible());

    // Clicking the launched path in the file list again brings both back.
    clickFile(scene, "launched.txt");
    settleFor("waiting state after re-selecting the launched path", waitingShown);
    CHECK(app.mergetool()->waiting());
    CHECK(app.mergetoolSession()->waitingPath() == QByteArray("launched.txt"));
    CHECK(scene.item("leaveMergetool")->isEnabled());
    expectText(scene.text("leaveMergetool"), QStringLiteral("Leave unresolved"), "leave control");
    CHECK(parent->state() != QProcess::NotRunning);

    // The control releases Git: MERGED gets its launch bytes back and both paths stay unmerged.
    scene.click("leaveMergetool");
    waitForGitToExit(scene, *parent);
    CHECK(parent->exitStatus() == QProcess::NormalExit && parent->exitCode() != 0);
    CHECK(readFile(repo.path("launched.txt")) == launchBytes);
    CHECK(repo.unmerged("launched.txt") && repo.unmerged("other.txt"));
    settleFor("leave control gone after leaving", [&] { return !scene.item("leaveMergetool")->isVisible(); });
    checkOffline(scene);
}
// An interrupt to the git mergetool process group, as Ctrl+C in its terminal, ends Git and the
// launcher with a saved draft unresolved. The window drops the waiting state and Leave, Git keeps
// the conflict and the launch bytes, and the next window restores the saved draft.
void interruptedMergetoolKeepsTheSavedDraft(Sandbox &sandbox)
{
    Repository repo({chosenByHand("launched.txt"), chosenByHand("other.txt")});
    const auto launchBytes = readFile(repo.path("launched.txt"));
    QString draft;
    {
        Scene scene(sandbox, repo);
        auto &app = scene.app();
        clickFile(scene, "launched.txt");
        auto parent = startMergetool(sandbox, repo, {"launched.txt"});
        waitForGitToLaunch(scene, "launched.txt");
        settleFor("waiting state on the launched path", [&] {
            return scene.item("mergetoolHolding")->isVisible() && scene.item("leaveMergetool")->isVisible();
        });
        appendToResult(scene, "draft");
        scene.click("saveDraft");
        settleFor("saved draft", [&] { return scene.progress().saved; });
        draft = scene.text("resultText");
        parent->signalGroup(SIGINT);
        waitForGitToExit(scene, *parent);
        CHECK(parent->exitStatus() == QProcess::CrashExit || parent->exitCode() != 0);
        waitFor(
            "the window to release the launched path",
            [&] { return !app.mergetoolSession()->hasWaiter(QByteArrayLiteral("launched.txt")); },
            mergetoolState(scene));
        settleFor("waiting state cleared", [&] {
            return !app.mergetool()->waiting() && !scene.item("mergetoolHolding")->isVisible()
                && !scene.item("leaveMergetool")->isVisible();
        });
        // Git did not receive the draft as a finished result; the window keeps it.
        CHECK(readFile(repo.path("launched.txt")) == launchBytes);
        CHECK(repo.unmerged("launched.txt") && repo.unmerged("other.txt"));
        expectText(scene.text("resultText"), draft, "draft after the interrupt");
        CHECK(scene.progress().saved && !scene.progress().resolved);
        checkOffline(scene);
    }
    Scene reopened(sandbox, repo);
    expectContains(reopened.app().loader()->detail(), "Recovered app drafts", "recovery detail");
    clickFile(reopened, "launched.txt");
    expectText(reopened.text("resultText"), draft, "restored saved draft");
    CHECK(reopened.progress().saved && !reopened.progress().resolved);
    checkOffline(reopened);
}

// --- Open repository -----------------------------------------------------------------------

// Every file under a recovery store by relative path, session.lock included.
QMap<QString, QByteArray> recoveryRecords(const QString &store)
{
    QMap<QString, QByteArray> records;
    QDirIterator files(store, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const auto path = files.next();
        records.insert(QDir(store).relativeFilePath(path), readFile(path));
    }
    return records;
}

// The window refuses to open `other` with `reason`. The header button is disabled, and a direct
// loader call or a stale picker request shows the reason, changes nothing and shows no picker.
void expectOpenAnotherRefused(Scene &scene, const QString &other, const QString &reason)
{
    auto *loader = scene.app().loader();
    auto *store = scene.app().store();
    const auto state = loader->startupState();
    const auto root = loader->repositoryRoot();
    const auto recovery = store->recoveryDirectory();
    const auto files = store->size();
    CHECK(files > 0);
    auto *button = test_support::waitForLayout(scene.window(), QStringLiteral("openRepository"), Scene::settleMs);
    CHECK(button->isVisible() && !button->isEnabled());
    CHECK(!loader->canOpenAnother());
    expectText(loader->openAnotherRefusal(), reason, "the refusal");
    scene.setPickedDirectory(other);
    CHECK(!loader->openAnotherRepository(other));
    expectText(scene.app().feedback()->message(), reason, "the refusal notice");
    scene.app().feedback()->show({});
    loader->chooseAnotherRepository();
    expectText(scene.app().feedback()->message(), reason, "the refusal notice for the picker");
    test_support::waitForFrames(scene.window(), "the window after the refused switch");
    CHECK(scene.pickerCalls() == 0);
    CHECK(loader->startupState() == state && !loader->opening());
    CHECK(loader->repositoryRoot() == root);
    CHECK(store->recoveryDirectory() == recovery && store->size() == files);
}

// A Ready window listens for git mergetool launches on a socket named after its operation, so it
// cannot open another repository.
void listeningWindowRefusesToOpenAnother(Sandbox &sandbox)
{
    Repository repo({chosenByHand("listening.txt")});
    Repository other({chosenByHand("other.txt")});
    const auto repository = repo.snapshot();
    Scene scene(sandbox, repo);
    CHECK(scene.app().mergetoolSession()->listening());
    expectOpenAnotherRefused(scene, other.root,
                             QStringLiteral("This window receives git mergetool launches for the current operation, "
                                            "so it cannot open another repository. Start Branch Truce again to open "
                                            "one."));
    CHECK(repo.snapshot() == repository);
    checkOffline(scene);
}

// A window that git mergetool started belongs to that launch, so it cannot open another repository.
void mergetoolWindowRefusesToOpenAnother(Sandbox &sandbox)
{
    Repository repo({chosenByHand("launched.txt")});
    Repository other({chosenByHand("other.txt")});
    const auto repository = repo.snapshot();
    Scene scene(sandbox, repo, {.directory = {}, .launchedByMergetool = true});
    expectOpenAnotherRefused(scene, other.root,
                             QStringLiteral("A git mergetool launch started this window, so it cannot open another "
                                            "repository. Start Branch Truce again to open one."));
    CHECK(repo.snapshot() == repository);
    checkOffline(scene);
}

// A saved draft in a window that cannot listen for git mergetool launches, and its recovery store.
struct KeptWork {
    QString draft;
    QString store;
    QMap<QString, QByteArray> records;
};

KeptWork saveDraftWithoutMergetool(Scene &scene, const char *path)
{
    auto *loader = scene.app().loader();
    expectContains(loader->detail(), "Mergetool attach is unavailable", "the session detail");
    clickFile(scene, path);
    appendToResult(scene, "kept");
    scene.click("saveDraft");
    settleFor("saved draft", [&] { return scene.progress().saved; });
    test_support::awaitRecovery(*scene.app().store());
    CHECK(loader->holdsUserWork() && loader->canOpenAnother());
    CHECK(scene.item("openRepository")->isEnabled());
    KeptWork kept{scene.text("resultText"), scene.app().store()->recoveryDirectory(), {}};
    kept.records = recoveryRecords(kept.store);
    CHECK(kept.records.contains(QStringLiteral("session.lock")));
    return kept;
}

// Open repository asks first when the session holds user work, and names what happens to it.
void openRepositoryAsksFirst(Scene &scene)
{
    scene.click("openRepository");
    auto *dialog = scene.object("openRepositoryDialog");
    settleFor("the Open repository prompt", [&] { return dialog->property("opened").toBool(); });
    expectContains(dialog->property("subtitle").toString(), "drafts and decisions", "the Open repository prompt");
    expectText(dialogButton(scene, "openRepositoryDialog", "confirmOpenRepository")->property("text").toString(),
               "Open Another Repository…", "the confirm button");
    expectText(dialogButton(scene, "openRepositoryDialog", "keepSession")->property("text").toString(),
               "Keep This Session", "the cancel button");
    CHECK(scene.pickerCalls() == 0);
}

// Keep This Session closes the prompt, shows no picker and keeps the session and its draft.
void keepSessionKeepsTheDraft(Sandbox &sandbox)
{
    Repository repo({chosenByHand("kept.txt")});
    Repository other({chosenByHand("other.txt")});
    Scene scene(sandbox, repo, {.directory = {}, .listen = false});
    auto *loader = scene.app().loader();
    const auto kept = saveDraftWithoutMergetool(scene, "kept.txt");
    scene.setPickedDirectory(other.root);
    openRepositoryAsksFirst(scene);
    clickDialogButton(scene, "openRepositoryDialog", "keepSession");
    test_support::waitForFrames(scene.window(), "the window after Keep This Session");
    CHECK(scene.pickerCalls() == 0);
    CHECK(loader->startupState() == SessionLoader::StartupState::ReadyWithoutMergetool && !loader->opening());
    CHECK(loader->repositoryRoot() == repo.root);
    CHECK(scene.app().store()->recoveryDirectory() == kept.store);
    expectText(scene.text("resultText"), kept.draft, "the draft after Keep This Session");
    CHECK(scene.progress().saved);
    CHECK(recoveryRecords(kept.store) == kept.records);
    checkOffline(scene);
}

// Confirm shows the picker and opens the picked repository with a new recovery store. The old
// store keeps its records, and the draft comes back when the first repository opens again.
void confirmOpensAnotherAndKeepsTheDraft(Sandbox &sandbox)
{
    Repository repo({chosenByHand("kept.txt")});
    Repository other({chosenByHand("other.txt")});
    const auto otherRepository = other.snapshot();
    Scene scene(sandbox, repo, {.directory = {}, .listen = false});
    auto *loader = scene.app().loader();
    auto *store = scene.app().store();
    const auto kept = saveDraftWithoutMergetool(scene, "kept.txt");
    const auto repository = repo.snapshot();
    const auto openedOn = [&](const Repository &opened) {
        return [&] {
            return loader->startupState() == SessionLoader::StartupState::ReadyWithoutMergetool
                && loader->repositoryRoot() == opened.root;
        };
    };
    scene.setPickedDirectory(other.root);
    openRepositoryAsksFirst(scene);
    clickDialogButton(scene, "openRepositoryDialog", "confirmOpenRepository");
    waitFor("the picked repository to open", openedOn(other), [&] { return loaderState(*loader); });
    CHECK(scene.pickerCalls() == 1);
    clickFile(scene, "other.txt");
    expectText(scene.text("resultText"), "other chosen by hand\n", "the picked repository's result");
    CHECK(store->recoveryDirectory() != kept.store);
    // The old store keeps every record. Only its lease went with the session.
    auto records = kept.records;
    records.remove(QStringLiteral("session.lock"));
    CHECK(QFileInfo(kept.store).isDir() && recoveryRecords(kept.store) == records);
    CHECK(repo.snapshot() == repository && other.snapshot() == otherRepository);

    // The new session holds no user work, so the picker shows at once.
    test_support::awaitRecovery(*store);
    CHECK(!loader->holdsUserWork());
    scene.setPickedDirectory(repo.root);
    scene.click("openRepository");
    waitFor("the first repository to open again", openedOn(repo), [&] { return loaderState(*loader); });
    CHECK(scene.pickerCalls() == 2);
    CHECK(store->recoveryDirectory() == kept.store);
    clickFile(scene, "kept.txt");
    expectText(scene.text("resultText"), kept.draft, "the draft after opening the repository again");
    CHECK(scene.progress().saved);
    checkOffline(scene);
}
} // namespace

void addHandoffJourneys(JourneySuite &suite)
{
    const auto add = [&](const char *name, JourneySuite::Journey journey) {
        suite.add("handoff", name, std::move(journey));
    };
    add("a newer draft goes back to the staged result from the file toolbar and the handoff",
        newerDraftReturnsToTheStagedResult);
    add("PRD.md:149 offline startup without an account resumes saved and unsaved drafts after closing",
        offlineStartupResumesDraftsAfterClosing);
    add("PRD.md:147 a session that ends without the handoff, as on a signal, restores its unsaved draft in the next "
        "window",
        sessionEndedWithoutHandoffRestoresTheDraft);
    add("a blocked startup lists each recovery session and restores the chosen one in the window",
        blockedStartupRestoresTheChosenSession);
    add("a resolved mergetool handoff drops the waiting text and a path Git has not launched keeps it",
        resolvedHandoffDropsTheWaitingText);
    add("clicking the launched path again after Git attached shows the waiting state and Leave unresolved",
        reselectingTheLaunchedPathOffersLeave);
    add("PRD.md:155 interrupting git mergetool keeps a saved draft from Git and the next window restores it",
        interruptedMergetoolKeepsTheSavedDraft);
    add("a window that listens for git mergetool launches refuses to open another repository and changes nothing",
        listeningWindowRefusesToOpenAnother);
    add("a window that git mergetool started refuses to open another repository and changes nothing",
        mergetoolWindowRefusesToOpenAnother);
    add("Open repository with a saved draft asks first, and Keep This Session keeps the session and its draft",
        keepSessionKeepsTheDraft);
    add("confirming Open repository opens the picked repository and the old recovery store keeps the draft",
        confirmOpensAnotherAndKeepsTheDraft);
}

} // namespace journey
