// Real-parent mergetool fixtures: installed /usr/bin/git runs `git mergetool`,
// which runs the Branch Truce launcher, which waits on an in-process session.
// docs/mergetool-launcher.md records the tested Git version and configuration.

#include "app/ApplicationController.h"
#include "fixtures/mergetool/RealParent.h"
#include "TestHarness.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QTextStream>

#include <csignal>

using namespace branch_truce;
using namespace mergetool_fixture;

namespace {
QString launcherBinary;

ConflictPath conflict(const QByteArray &path, const QByteArray &launch)
{
    return {path, "base\n", "main\n", "side\n", launch};
}

void openController(ApplicationController &controller, const ParentRepository &repo)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&controller, &ApplicationController::launchChanged, &loop, [&] {
        if (!controller.opening()) loop.quit();
    });
    CHECK(controller.openRepository(repo.root));
    timeout.start(15000);
    if (controller.opening()) loop.exec();
    CHECK(!controller.opening());
    if (controller.startupState() != "ready")
        throw std::runtime_error("controller startup: " + controller.detail().toStdString());
    CHECK(controller.mergetoolListening());
}

const FileSession &opened(ApplicationController &controller, const QByteArray &path)
{
    const auto *file = controller.files()->file(path.toHex().toStdString());
    CHECK(file != nullptr);
    return *file;
}

QString select(ApplicationController &controller, const QByteArray &path)
{
    const auto id = QString::fromLatin1(path.toHex());
    CHECK(controller.selectFile(id));
    return id;
}

QString revisionOf(ApplicationController &controller, const QByteArray &path)
{
    return QString::number(static_cast<qulonglong>(opened(controller, path).current().id));
}

std::string draftOf(ApplicationController &controller, const QByteArray &path)
{
    return opened(controller, path).current().draft.text;
}

// Prepends a line to the result, so the draft differs from the bytes Git launched with.
void editDraft(ApplicationController &controller, const QByteArray &path, const QString &prefix)
{
    const auto id = select(controller, path);
    CHECK(controller.editResult(id, revisionOf(controller, path), 0, 0, prefix));
}

void waitForWaiter(ApplicationController &controller, const QByteArray &path)
{
    pump([&] { return controller.mergetoolWaitingPath() == path; },
         controller.commandMessage() + " | waiting=" + QString::fromLatin1(controller.mergetoolWaitingPath()));
}

bool parentBlocked(const QProcess &parent, ApplicationController &controller, const QByteArray &path)
{
    return parent.state() == QProcess::Running && controller.mergetoolHasWaiter(path);
}
// The Git temporary of one kind (LOCAL, REMOTE, BASE or BACKUP) for a path, which must exist exactly once.
QString temporary(const ParentRepository &repo, const QByteArray &path, const QString &kind)
{
    QStringList matches;
    for (const auto &file : repo.gitTemporaries(path))
        if (QFileInfo(file).fileName().contains(QLatin1Char('_') + kind + QLatin1Char('_'))) matches.append(file);
    CHECK(matches.size() == 1);
    return matches.value(0);
}

// While Git waits on the tool, its temporaries exist and the session's intake holds the same bytes.
void checkIntakeMatchesGit(const ParentRepository &repo, const QString &intake, const QByteArray &path)
{
    CHECK(repo.gitTemporaries(path).size() == 4);
    CHECK(readFile(intake + "/local") == readFile(temporary(repo, path, "LOCAL")));
    CHECK(readFile(intake + "/remote") == readFile(temporary(repo, path, "REMOTE")));
    CHECK(readFile(intake + "/base") == readFile(temporary(repo, path, "BASE")));
    CHECK(readFile(intake + "/local") == "main\n" && readFile(intake + "/remote") == "side\n");
    CHECK(readFile(intake + "/base") == "base\n");
    CHECK(readFile(intake + "/merged") == readFile(repo.path(path)));
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (argc != 2) {
        QTextStream(stderr) << "usage: mergetool-parent-tests <branch-truce>\n";
        return 2;
    }
    launcherBinary = QString::fromLocal8Bit(argv[1]);
    {
        QProcess version;
        version.start("/usr/bin/git", {"--version"});
        CHECK(version.waitForFinished(5000) && version.exitCode() == 0);
        QTextStream(stdout) << "parent: /usr/bin/" << version.readAllStandardOutput();
    }
    test_support::Suite suite("MergetoolParentTests");

    suite.run("Save checkpoints in the app only while installed Git stays blocked and nothing is staged", [] {
        ParentRepository repo(launcherBinary, {conflict("tracked.txt", "launch bytes\n"),
                                               conflict("other.txt", "other launch bytes\n")});
        ScopedEnvironment environment(repo.environment);
        ApplicationController controller;
        openController(controller, repo);
        const auto indexBefore = repo.index();
        auto parent = startParent(repo, ParentRepository::Tool, {"tracked.txt"});
        waitForWaiter(controller, "tracked.txt");
        CHECK(controller.parentSessionActive());
        editDraft(controller, "tracked.txt", QStringLiteral("draft kept in the app\n"));
        const auto draft = draftOf(controller, "tracked.txt");
        CHECK(controller.saveDraft(select(controller, "tracked.txt"), revisionOf(controller, "tracked.txt")));
        CHECK(controller.commandMessage().contains(QStringLiteral("repository file unchanged")));
        CHECK(opened(controller, "tracked.txt").checkpoint());
        CHECK(opened(controller, "tracked.txt").checkpoint()->draft.text == draft);
        CHECK(!opened(controller, "tracked.txt").facts().resolved);
        // Git is still inside the tool command; a launcher that returned would let it stage or restore.
        holdFor(1000, [&] { return parentBlocked(*parent, controller, "tracked.txt"); },
                QStringLiteral("the parent was released by Save"));
        CHECK(readFile(repo.path("tracked.txt")) == "launch bytes\n");
        CHECK(repo.index() == indexBefore);
        CHECK(repo.unmerged("tracked.txt") && repo.unmerged("other.txt"));
        CHECK(controller.leaveMergetool());
        waitParent(*parent, 1, controller.commandMessage());
        CHECK(repo.index() == indexBefore);
        CHECK(readFile(repo.path("tracked.txt")) == "launch bytes\n");
        CHECK(opened(controller, "tracked.txt").checkpoint()->draft.text == draft);
    });

    suite.run("Git stages the launched path itself and resolution waits for the exact acknowledged object", [] {
        ParentRepository repo(launcherBinary, {conflict("tracked.txt", "launch bytes\n"),
                                               conflict("other.txt", "other launch bytes\n")});
        ScopedEnvironment environment(repo.environment);
        ApplicationController controller;
        openController(controller, repo);
        auto parent = startParent(repo, ParentRepository::GatedTool, {"tracked.txt"});
        waitForWaiter(controller, "tracked.txt");
        editDraft(controller, "tracked.txt", QStringLiteral("reviewed in the app\n"));
        const auto id = select(controller, "tracked.txt");
        const auto expected = QByteArray::fromStdString(draftOf(controller, "tracked.txt"));
        CHECK(controller.markResolved(id, revisionOf(controller, "tracked.txt"), true));
        // The launcher returned 0; the parent now holds before its own `git add`.
        pump([&] { return repo.exits() == "tracked.txt 0\n"; }, controller.commandMessage() + " | launcher still waiting");
        const auto offer = controller.parentStagingOffer(id);
        CHECK(offer && offer->path == "tracked.txt");
        CHECK(offer->canonicalObjectId == repo.objectOf(expected));
        CHECK(readFile(repo.path("tracked.txt")) == expected);
        holdFor(800, [&] {
            return !opened(controller, "tracked.txt").facts().resolved && repo.unmerged("tracked.txt");
        }, QStringLiteral("resolution was reported before the parent staged the path"));
        CHECK(parent->state() == QProcess::Running);
        repo.release();
        waitParent(*parent, 0, controller.commandMessage());
        pump([&] { return opened(controller, "tracked.txt").facts().resolved; }, controller.commandMessage());
        CHECK(!repo.unmerged("tracked.txt"));
        CHECK(repo.stagedObject("tracked.txt") == offer->canonicalObjectId);
        CHECK(repo.git({"show", ":tracked.txt"}) == expected);
        CHECK(repo.unmerged("other.txt"));
        CHECK(repo.stagedObject("notes.txt") == repo.notesObject);
        CHECK(!controller.parentStagingOffer(id));
    });

    suite.run("bytes the parent stages after the launcher are not acknowledged as the reviewed result", [] {
        ParentRepository repo(launcherBinary, {conflict("tracked.txt", "launch bytes\n")});
        ScopedEnvironment environment(repo.environment);
        ApplicationController controller;
        openController(controller, repo);
        auto parent = startParent(repo, ParentRepository::TamperTool, {"tracked.txt"});
        waitForWaiter(controller, "tracked.txt");
        editDraft(controller, "tracked.txt", QStringLiteral("reviewed in the app\n"));
        const auto id = select(controller, "tracked.txt");
        CHECK(controller.markResolved(id, revisionOf(controller, "tracked.txt"), true));
        waitParent(*parent, 0, controller.commandMessage());
        CHECK(repo.git({"show", ":tracked.txt"}) == ParentRepository::TamperedBytes);
        pump([&] {
            return controller.commandMessage().contains(QStringLiteral("does not contain the reviewed result"));
        }, controller.commandMessage());
        CHECK(!opened(controller, "tracked.txt").facts().resolved);
        CHECK(opened(controller, "tracked.txt").checkpoint());
    });

    suite.run("leave restores the parent-visible file after an outside edit and keeps the saved draft", [] {
        ParentRepository repo(launcherBinary, {conflict("tracked.txt", "launch bytes\n")});
        ScopedEnvironment environment(repo.environment);
        ApplicationController controller;
        openController(controller, repo);
        const auto indexBefore = repo.index();
        auto parent = startParent(repo, ParentRepository::Tool, {"tracked.txt"});
        waitForWaiter(controller, "tracked.txt");
        const auto intake = controller.mergetoolIntakes().front();
        editDraft(controller, "tracked.txt", QStringLiteral("draft before leave\n"));
        const auto draft = draftOf(controller, "tracked.txt");
        CHECK(controller.saveDraft(select(controller, "tracked.txt"), revisionOf(controller, "tracked.txt")));
        writeFile(repo.path("tracked.txt"), "written by another editor\n");
        CHECK(controller.leaveMergetool());
        waitParent(*parent, 1, controller.commandMessage());
        CHECK(readFile(repo.path("tracked.txt")) == "launch bytes\n");
        CHECK(repo.index() == indexBefore);
        CHECK(repo.gitTemporaries("tracked.txt").isEmpty());
        CHECK(opened(controller, "tracked.txt").checkpoint()->draft.text == draft);
        CHECK(!opened(controller, "tracked.txt").facts().resolved);
        CHECK(readFile(intake + "/merged") == "launch bytes\n");
    });

    suite.run("a hard quit that destroys the session fails the tool and a new session recovers the draft", [] {
        ParentRepository repo(launcherBinary, {conflict("tracked.txt", "launch bytes\n")});
        ScopedEnvironment environment(repo.environment);
        const auto indexBefore = repo.index();
        std::string draft;
        auto controller = std::make_unique<ApplicationController>();
        openController(*controller, repo);
        auto parent = startParent(repo, ParentRepository::Tool, {"tracked.txt"});
        waitForWaiter(*controller, "tracked.txt");
        editDraft(*controller, "tracked.txt", QStringLiteral("draft across sessions\n"));
        draft = draftOf(*controller, "tracked.txt");
        CHECK(controller->saveDraft(select(*controller, "tracked.txt"), revisionOf(*controller, "tracked.txt")));
        controller.reset();
        waitParent(*parent, 1, QStringLiteral("closed session"));
        CHECK(readFile(repo.path("tracked.txt")) == "launch bytes\n");
        CHECK(repo.index() == indexBefore);
        ApplicationController reopened;
        openController(reopened, repo);
        CHECK(opened(reopened, "tracked.txt").checkpoint());
        CHECK(opened(reopened, "tracked.txt").checkpoint()->draft.text == draft);
        CHECK(!opened(reopened, "tracked.txt").facts().resolved);
    });

    suite.run("a terminal interrupt stops Git and the launcher without staging and keeps the saved draft", [] {
        ParentRepository repo(launcherBinary, {conflict("tracked.txt", "launch bytes\n")});
        ScopedEnvironment environment(repo.environment);
        ApplicationController controller;
        openController(controller, repo);
        const auto indexBefore = repo.index();
        auto parent = startParent(repo, ParentRepository::Tool, {"tracked.txt"});
        waitForWaiter(controller, "tracked.txt");
        editDraft(controller, "tracked.txt", QStringLiteral("draft before the interrupt\n"));
        const auto draft = draftOf(controller, "tracked.txt");
        CHECK(controller.saveDraft(select(controller, "tracked.txt"), revisionOf(controller, "tracked.txt")));
        parent->signalGroup(SIGINT);
        pump([&] { return parent->state() == QProcess::NotRunning; }, QStringLiteral("interrupted parent still running"));
        CHECK(parent->exitStatus() == QProcess::CrashExit || parent->exitCode() != 0);
        pump([&] { return !controller.mergetoolHasWaiter("tracked.txt"); }, QStringLiteral("waiter kept after interrupt"));
        CHECK(readFile(repo.path("tracked.txt")) == "launch bytes\n");
        CHECK(repo.index() == indexBefore);
        CHECK(opened(controller, "tracked.txt").checkpoint()->draft.text == draft);
        CHECK(!opened(controller, "tracked.txt").facts().resolved);
        // Git 2.43 has no interrupt trap: its BACKUP, LOCAL, REMOTE and BASE files stay in the worktree.
        CHECK(repo.gitTemporaries("tracked.txt").size() == 4);
    });

    suite.run("repeated invocations of one path keep each intake while Git removes its own temporaries", [] {
        ParentRepository repo(launcherBinary, {conflict("tracked.txt", "launch bytes\n")});
        ScopedEnvironment environment(repo.environment);
        ApplicationController controller;
        openController(controller, repo);
        auto first = startParent(repo, ParentRepository::Tool, {"tracked.txt"});
        waitForWaiter(controller, "tracked.txt");
        CHECK(controller.mergetoolIntakes().size() == 1);
        const auto firstIntake = controller.mergetoolIntakes().front();
        checkIntakeMatchesGit(repo, firstIntake, "tracked.txt");
        const auto firstTemporaries = repo.gitTemporaries("tracked.txt");
        select(controller, "tracked.txt");
        CHECK(controller.leaveMergetool());
        waitParent(*first, 1, controller.commandMessage());
        CHECK(repo.gitTemporaries("tracked.txt").isEmpty());
        CHECK(repo.unmerged("tracked.txt"));

        auto second = startParent(repo, ParentRepository::Tool, {"tracked.txt"});
        pump([&] {
            return controller.mergetoolIntakes().size() == 2 && controller.mergetoolWaitingPath() == "tracked.txt";
        }, controller.commandMessage());
        const auto secondIntake = controller.mergetoolIntakes().at(1);
        CHECK(secondIntake != firstIntake);
        checkIntakeMatchesGit(repo, secondIntake, "tracked.txt");
        for (const auto &file : repo.gitTemporaries("tracked.txt")) CHECK(!firstTemporaries.contains(file));
        editDraft(controller, "tracked.txt", QStringLiteral("resolved on the second run\n"));
        const auto expected = QByteArray::fromStdString(draftOf(controller, "tracked.txt"));
        CHECK(controller.markResolved(select(controller, "tracked.txt"), revisionOf(controller, "tracked.txt"), true));
        waitParent(*second, 0, controller.commandMessage());
        pump([&] { return opened(controller, "tracked.txt").facts().resolved; }, controller.commandMessage());
        CHECK(repo.git({"show", ":tracked.txt"}) == expected);
        CHECK(repo.gitTemporaries("tracked.txt").isEmpty());
        for (const auto &intake : {firstIntake, secondIntake}) {
            CHECK(readFile(intake + "/merged") == "launch bytes\n");
            CHECK(readFile(intake + "/local") == "main\n" && readFile(intake + "/base") == "base\n");
        }
    });

    suite.run("multi-file Git keeps the next path blocked until the previous one is acknowledged, per path", [] {
        ParentRepository repo(launcherBinary, {conflict("alpha.txt", "same launch\n"),
                                               conflict("beta.txt", "same launch\n")});
        ScopedEnvironment environment(repo.environment);
        ApplicationController controller;
        openController(controller, repo);
        const auto betaEntries = repo.git({"ls-files", "--stage", "-z", "--", "beta.txt"});
        auto parent = startParent(repo, ParentRepository::GatedTool, {});
        // Git 2.43 visits unmerged paths in name order.
        waitForWaiter(controller, "alpha.txt");
        const auto beta = select(controller, "beta.txt");
        CHECK(!controller.markResolved(beta, revisionOf(controller, "beta.txt"), true));
        CHECK(controller.commandMessage().contains(QStringLiteral("Waiting for Git to launch this path")));
        editDraft(controller, "alpha.txt", QStringLiteral("identical result\n"));
        const auto expected = QByteArray::fromStdString(draftOf(controller, "alpha.txt"));
        CHECK(controller.markResolved(select(controller, "alpha.txt"), revisionOf(controller, "alpha.txt"), true));
        pump([&] { return repo.exits() == "alpha.txt 0\n"; }, controller.commandMessage());
        holdFor(600, [&] {
            return controller.mergetoolIntakes().size() == 1 && !opened(controller, "alpha.txt").facts().resolved
                && repo.unmerged("alpha.txt") && repo.unmerged("beta.txt");
        }, QStringLiteral("the next path or a resolution moved before Git staged alpha.txt"));
        repo.release();
        pump([&] {
            return opened(controller, "alpha.txt").facts().resolved && controller.mergetoolWaitingPath() == "beta.txt";
        }, controller.commandMessage() + " | waiting=" + QString::fromLatin1(controller.mergetoolWaitingPath()));
        const auto object = repo.objectOf(expected);
        CHECK(repo.stagedObject("alpha.txt") == object);
        holdFor(600, [&] {
            return parentBlocked(*parent, controller, "beta.txt") && !opened(controller, "beta.txt").facts().resolved
                && repo.git({"ls-files", "--stage", "-z", "--", "beta.txt"}) == betaEntries;
        }, QStringLiteral("beta.txt was released or acknowledged by the alpha.txt staging"));
        CHECK(controller.mergetoolIntakes().size() == 2);
        CHECK(readFile(controller.mergetoolIntakes().at(1) + "/merged") == "same launch\n");
        editDraft(controller, "beta.txt", QStringLiteral("identical result\n"));
        CHECK(QByteArray::fromStdString(draftOf(controller, "beta.txt")) == expected);
        CHECK(controller.markResolved(beta, revisionOf(controller, "beta.txt"), true));
        pump([&] { return repo.exits() == "alpha.txt 0\nbeta.txt 0\n"; }, controller.commandMessage());
        // The object Git already holds for alpha.txt does not acknowledge beta.txt.
        holdFor(600, [&] {
            return !opened(controller, "beta.txt").facts().resolved && repo.unmerged("beta.txt");
        }, QStringLiteral("beta.txt was acknowledged before Git staged it"));
        repo.release();
        waitParent(*parent, 0, controller.commandMessage());
        pump([&] { return opened(controller, "beta.txt").facts().resolved; }, controller.commandMessage());
        CHECK(repo.stagedObject("beta.txt") == object && repo.stagedObject("alpha.txt") == object);
        CHECK(repo.stagedObject("notes.txt") == repo.notesObject);
    });

    suite.run("an external monitor configured during the wait keeps the parent blocked instead of releasing it", [] {
        ParentRepository repo(launcherBinary, {conflict("tracked.txt", "launch bytes\n")});
        ScopedEnvironment environment(repo.environment);
        ApplicationController controller;
        openController(controller, repo);
        const auto indexBefore = repo.index();
        auto parent = startParent(repo, ParentRepository::Tool, {"tracked.txt"});
        waitForWaiter(controller, "tracked.txt");
        const auto sentinel = repo.probe + QStringLiteral("/monitor-ran");
        const auto monitor = repo.probe + QStringLiteral("/monitor");
        writeFile(monitor, "#!/bin/sh\n: > " + shellQuote(sentinel).toUtf8() + "\nexit 1\n");
        CHECK(QFile::setPermissions(monitor, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        repo.git({"config", "core.fsmonitor", monitor});
        editDraft(controller, "tracked.txt", QStringLiteral("must not reach the parent\n"));
        controller.markResolved(select(controller, "tracked.txt"), revisionOf(controller, "tracked.txt"), true);
        holdFor(1500, [&] {
            return parentBlocked(*parent, controller, "tracked.txt") && !QFile::exists(sentinel)
                && !opened(controller, "tracked.txt").facts().resolved;
        }, controller.commandMessage() + QStringLiteral(" | the parent was released with a monitor configured"));
        CHECK(controller.commandMessage().contains(QStringLiteral("filesystem monitor setting: core.fsmonitor")));
        // Fixture inspections below must not run the monitor either.
        repo.git({"config", "--unset", "core.fsmonitor"});
        CHECK(!QFile::exists(sentinel));
        CHECK(repo.index() == indexBefore);
        // Mark resolved wrote the parent-owned file before the guard refused the release.
        CHECK(readFile(repo.path("tracked.txt")) == QByteArray::fromStdString(draftOf(controller, "tracked.txt")));
        CHECK(controller.leaveMergetool());
        waitParent(*parent, 1, controller.commandMessage());
        CHECK(readFile(repo.path("tracked.txt")) == "launch bytes\n");
        CHECK(repo.index() == indexBefore);
        CHECK(!QFile::exists(sentinel));
    });

    return suite.report();
}
