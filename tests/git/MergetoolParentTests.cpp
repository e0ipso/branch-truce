// Real-parent mergetool fixtures: installed /usr/bin/git runs `git mergetool`,
// which runs the Branch Truce launcher, which waits on an in-process session.
// docs/mergetool-launcher.md records the tested Git version and configuration.

#include "app/ApplicationController.h"
#include "fixtures/GitOnPath.h"
#include "app/MergetoolProtocol.h"
#include "fixtures/mergetool/GitGate.h"
#include "fixtures/mergetool/RealParent.h"
#include "TestHarness.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTextStream>

#include <csignal>
#include <sys/stat.h>

using namespace branch_truce;
using namespace mergetool_fixture;

namespace {
QString launcherBinary;

ConflictPath conflict(const QByteArray &path, const QByteArray &launch)
{
    return {path, "base\n", "main\n", "side\n", launch};
}

// The session in this process listens in the fixture's private runtime directory, not a fallback.
QString checkSocketDirectory(const ParentRepository &repo)
{
    CHECK(qEnvironmentVariable("XDG_RUNTIME_DIR") == repo.runtimeDirectory());
    const auto socket = mergetoolSocketDirectory();
    if (socket != repo.runtimeDirectory() + QStringLiteral("/branch-truce"))
        throw std::runtime_error("socket directory outside the private runtime directory: " + socket.toStdString());
    return socket;
}

void openController(ApplicationController &controller, const ParentRepository &repo)
{
    checkSocketDirectory(repo);
    CHECK(controller.loader()->openRepository(repo.root));
    // Opening takes about 0.5 s on an idle host. At load averages of 25 to 36 on 8 cores it
    // averaged 2.5 s and some opens took over 10 s.
    pump([&] { return !controller.loader()->opening(); }, QStringLiteral("the repository opening did not finish"));
    CHECK(!controller.loader()->opening());
    if (controller.loader()->startupState() != "ready")
        throw std::runtime_error("controller startup: " + controller.loader()->detail().toStdString());
    CHECK(controller.mergetoolListening());
    CHECK(QFileInfo(checkSocketDirectory(repo)).isDir());
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

// Edits and saves an app-only draft for each path. Returns the saved text per path.
QMap<QByteArray, std::string> saveDrafts(ApplicationController &controller, const QList<QByteArray> &paths)
{
    QMap<QByteArray, std::string> drafts;
    for (const auto &path : paths) {
        editDraft(controller, path, QStringLiteral("draft kept by leave: %1\n").arg(QString::fromLatin1(path)));
        CHECK(controller.saveDraft(select(controller, path), revisionOf(controller, path)));
        drafts.insert(path, draftOf(controller, path));
    }
    return drafts;
}

// A new session on the same repository restores every saved draft, still unresolved.
void checkRecovered(const ParentRepository &repo, const QMap<QByteArray, std::string> &drafts)
{
    ApplicationController reopened(gitOnPath());
    openController(reopened, repo);
    for (auto draft = drafts.cbegin(); draft != drafts.cend(); ++draft) {
        const auto &file = opened(reopened, draft.key());
        CHECK(file.checkpoint() && file.checkpoint()->draft.text == draft.value());
        CHECK(!file.facts().resolved);
    }
}

// Every sessionEnded emission of one controller.
struct SessionEnd {
    int count = 0;
    QString outcome;
    bool clean = true;
};

void watchSessionEnd(ApplicationController &controller, SessionEnd &end)
{
    QObject::connect(&controller, &ApplicationController::sessionEnded, &controller,
                     [&end](const QString &outcome, bool clean) {
        ++end.count;
        end.outcome = outcome;
        end.clean = clean;
    });
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

ino_t inodeOf(const QString &path)
{
    struct stat status {};
    CHECK(::stat(QFile::encodeName(path).constData(), &status) == 0);
    return status.st_ino;
}

// Leaves two recovery stores that both edited other.txt, so the next session's
// restore stays blocked on a choice. Returns the session of the "chosen" store.
QString twoEditedStores(const ParentRepository &repo)
{
    ApplicationController chosen(gitOnPath());
    openController(chosen, repo);
    // A second window on the operation finds the socket taken and does not listen.
    ApplicationController other(gitOnPath());
    CHECK(other.loader()->openRepository(repo.root));
    pump([&] { return !other.loader()->opening(); }, QStringLiteral("the second window's repository opening did not finish"));
    CHECK(other.loader()->startupState() == "ready" && !other.mergetoolListening());
    editDraft(chosen, "other.txt", QStringLiteral("chosen\n"));
    editDraft(other, "other.txt", QStringLiteral("other\n"));
    return QFileInfo(chosen.files()->recoveryDirectory()).fileName();
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
        ApplicationController controller(gitOnPath());
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
        ApplicationController controller(gitOnPath());
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

    // TRUCE-0100, TRUCE-0101: the in-window recovery choice reloads the files while
    // Git waits. The chosen store captured tracked.txt before Git moved MERGED
    // aside and copied it back, so only the inode and timestamps changed. The
    // waiting launcher's MERGED copy holds the captured launch bytes, so the
    // restore accepts the copy and Mark resolved hands the path back to Git.
    suite.run("a recovery choice made while Git waits keeps the launched path editable and Mark resolved hands it back to Git", [] {
        ParentRepository repo(launcherBinary, {conflict("tracked.txt", "launch bytes\n"),
                                               conflict("other.txt", "other launch bytes\n")});
        ScopedEnvironment environment(repo.environment);
        const auto session = twoEditedStores(repo);
        ApplicationController controller(gitOnPath());
        openController(controller, repo);
        const auto launchInode = inodeOf(repo.path("tracked.txt"));
        auto parent = startParent(repo, ParentRepository::Tool, {"tracked.txt"});
        waitForWaiter(controller, "tracked.txt");
        // Git's copy is a new file with the same bytes.
        CHECK(inodeOf(repo.path("tracked.txt")) != launchInode);
        CHECK(readFile(repo.path("tracked.txt")) == "launch bytes\n");
        CHECK(readFile(controller.mergetoolIntakes().front() + "/merged") == "launch bytes\n");
        CHECK(controller.loader()->chooseRecoverySession(session));
        pump([&] { return !controller.loader()->opening(); }, controller.loader()->detail());
        CHECK(controller.loader()->startupState() == "ready" && controller.loader()->recoveryChoices().isEmpty());
        CHECK(draftOf(controller, "other.txt").starts_with("chosen\n"));
        CHECK(opened(controller, "tracked.txt").snapshots()->launchResult.text.bytes == "launch bytes\n");
        waitForWaiter(controller, "tracked.txt");
        const auto id = select(controller, "tracked.txt");
        // TRUCE-0102: Git rechecks the index and conversion policy before the error clears.
        pump([&] { return !controller.files()->hasRecoveryFailure(id.toStdString()); }, controller.commandMessage());
        CHECK(controller.mergetoolWaiting());
        editDraft(controller, "tracked.txt", QStringLiteral("reviewed after the choice\n"));
        const auto expected = QByteArray::fromStdString(draftOf(controller, "tracked.txt"));
        CHECK(controller.markResolved(id, revisionOf(controller, "tracked.txt"), true));
        waitParent(*parent, 0, controller.commandMessage());
        pump([&] { return opened(controller, "tracked.txt").facts().resolved; }, controller.commandMessage());
        CHECK(!repo.unmerged("tracked.txt"));
        CHECK(repo.git({"show", ":tracked.txt"}) == expected);
        CHECK(readFile(repo.path("tracked.txt")) == expected);
        CHECK(repo.unmerged("other.txt"));
        CHECK(repo.stagedObject("notes.txt") == repo.notesObject);
        CHECK(!controller.mergetoolHasWaiter("tracked.txt"));
    });

    suite.run("an outside edit of the launched path before a recovery choice keeps its recovery error and stages nothing", [] {
        ParentRepository repo(launcherBinary, {conflict("tracked.txt", "launch bytes\n"),
                                               conflict("other.txt", "other launch bytes\n")});
        ScopedEnvironment environment(repo.environment);
        const auto session = twoEditedStores(repo);
        const auto indexBefore = repo.index();
        ApplicationController controller(gitOnPath());
        openController(controller, repo);
        auto parent = startParent(repo, ParentRepository::Tool, {"tracked.txt"});
        waitForWaiter(controller, "tracked.txt");
        writeFile(repo.path("tracked.txt"), "written by another editor\n");
        CHECK(controller.loader()->chooseRecoverySession(session));
        pump([&] { return !controller.loader()->opening(); }, controller.loader()->detail());
        CHECK(controller.loader()->startupState() == "ready" && controller.loader()->recoveryChoices().isEmpty());
        waitForWaiter(controller, "tracked.txt");
        const auto id = select(controller, "tracked.txt");
        CHECK(controller.files()->hasRecoveryFailure(id.toStdString()));
        CHECK(!controller.markResolved(id, revisionOf(controller, "tracked.txt"), true));
        holdFor(800, [&] {
            return parentBlocked(*parent, controller, "tracked.txt") && repo.index() == indexBefore
                && !opened(controller, "tracked.txt").facts().resolved;
        }, controller.commandMessage() + QStringLiteral(" | the path was staged or the parent released"));
        CHECK(readFile(repo.path("tracked.txt")) == "written by another editor\n");
        const auto draft = draftOf(controller, "tracked.txt");
        CHECK(controller.leaveMergetool());
        waitParent(*parent, 1, controller.commandMessage());
        CHECK(readFile(repo.path("tracked.txt")) == "launch bytes\n");
        CHECK(repo.index() == indexBefore);
        CHECK(repo.unmerged("tracked.txt") && repo.unmerged("other.txt"));
        CHECK(!opened(controller, "tracked.txt").facts().resolved);
        CHECK(draftOf(controller, "tracked.txt") == draft);
        CHECK(!controller.mergetoolHasWaiter("tracked.txt"));
    });

    // TRUCE-0102: the waiting launcher's MERGED copy still holds the launch
    // bytes, but the index entries or the conversion policy changed since the
    // store captured the path, so the restored draft keeps its recovery error.
    suite.run("a staged launched path or a changed conversion policy before a recovery choice keeps its recovery error", [] {
        for (const QByteArray &change : {QByteArray("staged"), QByteArray("autocrlf")}) {
            ParentRepository repo(launcherBinary, {conflict("tracked.txt", "launch bytes\n"),
                                                   conflict("other.txt", "other launch bytes\n")});
            ScopedEnvironment environment(repo.environment);
            const auto session = twoEditedStores(repo);
            ApplicationController controller(gitOnPath());
            openController(controller, repo);
            auto parent = startParent(repo, ParentRepository::Tool, {"tracked.txt"});
            waitForWaiter(controller, "tracked.txt");
            CHECK(readFile(controller.mergetoolIntakes().front() + "/merged") == "launch bytes\n");
            if (change == "staged") repo.git({"add", "--", "tracked.txt"});
            else repo.git({"config", "core.autocrlf", "true"});
            const auto indexChanged = repo.index();
            CHECK(controller.loader()->chooseRecoverySession(session));
            pump([&] { return !controller.loader()->opening(); }, controller.loader()->detail());
            CHECK(controller.loader()->startupState() == "ready" && controller.loader()->recoveryChoices().isEmpty());
            waitForWaiter(controller, "tracked.txt");
            const auto id = select(controller, "tracked.txt");
            holdFor(800, [&] {
                return controller.files()->hasRecoveryFailure(id.toStdString())
                    && parentBlocked(*parent, controller, "tracked.txt") && repo.index() == indexChanged;
            }, QString::fromLatin1(change) + QStringLiteral(": the recovery error cleared or the parent was released | ")
                + controller.commandMessage());
            CHECK(!controller.editResult(id, revisionOf(controller, "tracked.txt"), 0, 0, QStringLiteral("edited\n")));
            CHECK(!controller.markResolved(id, revisionOf(controller, "tracked.txt"), true));
            CHECK(controller.leaveMergetool());
            waitParent(*parent, 1, controller.commandMessage());
            CHECK(readFile(repo.path("tracked.txt")) == "launch bytes\n");
            CHECK(repo.index() == indexChanged);
            CHECK(!opened(controller, "tracked.txt").facts().resolved);
        }
    });

    // TRUCE-0102: the restore after the choice validates the index and policy
    // itself, so the case above keeps its error even without the revalidation.
    // Here both change after the restore and while the revalidation that the
    // choice starts waits on its first index inspection, so only the
    // revalidation can see them.
    suite.run("a path staged or a conversion policy changed while a recovery choice revalidates the launched path keeps its recovery error", [] {
        for (const QByteArray &change : {QByteArray("staged"), QByteArray("autocrlf")}) {
            ParentRepository repo(launcherBinary, {conflict("tracked.txt", "launch bytes\n"),
                                                   conflict("other.txt", "other launch bytes\n")});
            ScopedEnvironment environment(repo.environment);
            const auto session = twoEditedStores(repo);
            // Open until close, so the load runs Git through it unchanged.
            GitGate gate("tracked.txt");
            ApplicationController controller(gate.executable());
            // Runs before the controller goes, so its tasks never wait on a held call.
            const auto releaseGate = qScopeGuard([&gate] { gate.open(); gate.releaseAll(); });
            openController(controller, repo);
            auto parent = startParent(repo, ParentRepository::Tool, {"tracked.txt"});
            waitForWaiter(controller, "tracked.txt");
            gate.close(QStringLiteral("ls-files"));
            CHECK(controller.loader()->chooseRecoverySession(session));
            // Calls held while the load runs are the restore's. The load
            // completes on this thread, so a call held after it is the
            // revalidation's.
            QString revalidation;
            pump([&] {
                for (const auto &call : gate.held()) {
                    if (!controller.loader()->opening()) {
                        revalidation = call;
                        return true;
                    }
                    gate.release(call);
                }
                return false;
            }, QString::fromLatin1(change) + QStringLiteral(": no revalidation after the choice | ") + controller.loader()->detail());
            CHECK(gate.held() == QStringList{revalidation});
            CHECK(gate.arguments(revalidation).contains("ls-files --stage -z -- tracked.txt"));
            gate.open();
            CHECK(controller.loader()->startupState() == "ready" && controller.loader()->recoveryChoices().isEmpty());
            const auto id = select(controller, "tracked.txt");
            CHECK(controller.files()->hasRecoveryFailure(id.toStdString()));
            if (change == "staged") repo.git({"add", "--", "tracked.txt"});
            else repo.git({"config", "core.autocrlf", "true"});
            const auto indexChanged = repo.index();
            gate.release(revalidation);
            holdFor(800, [&] {
                return controller.files()->hasRecoveryFailure(id.toStdString())
                    && parentBlocked(*parent, controller, "tracked.txt") && repo.index() == indexChanged;
            }, QString::fromLatin1(change) + QStringLiteral(": the recovery error cleared or the parent was released | ")
                + controller.commandMessage());
            CHECK(!controller.editResult(id, revisionOf(controller, "tracked.txt"), 0, 0, QStringLiteral("edited\n")));
            CHECK(!controller.markResolved(id, revisionOf(controller, "tracked.txt"), true));
            CHECK(controller.leaveMergetool());
            waitParent(*parent, 1, controller.commandMessage());
            CHECK(readFile(repo.path("tracked.txt")) == "launch bytes\n");
            CHECK(repo.index() == indexChanged);
            CHECK(!opened(controller, "tracked.txt").facts().resolved);
        }
    });

    suite.run("bytes the parent stages after the launcher are not acknowledged as the reviewed result", [] {
        ParentRepository repo(launcherBinary, {conflict("tracked.txt", "launch bytes\n")});
        ScopedEnvironment environment(repo.environment);
        ApplicationController controller(gitOnPath());
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
        ApplicationController controller(gitOnPath());
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
        auto controller = std::make_unique<ApplicationController>(gitOnPath());
        openController(*controller, repo);
        auto parent = startParent(repo, ParentRepository::Tool, {"tracked.txt"});
        waitForWaiter(*controller, "tracked.txt");
        editDraft(*controller, "tracked.txt", QStringLiteral("draft across sessions\n"));
        draft = draftOf(*controller, "tracked.txt");
        CHECK(controller->saveDraft(select(*controller, "tracked.txt"), revisionOf(*controller, "tracked.txt")));
        controller.reset();
        waitParent(*parent, 1, QStringLiteral("closed session"));
        // A dropped session, unlike an explicit leave, makes the launcher report the missing decision.
        CHECK(parent->readAllStandardError().contains("closed before an explicit decision"));
        CHECK(readFile(repo.path("tracked.txt")) == "launch bytes\n");
        CHECK(repo.index() == indexBefore);
        ApplicationController reopened(gitOnPath());
        openController(reopened, repo);
        CHECK(opened(reopened, "tracked.txt").checkpoint());
        CHECK(opened(reopened, "tracked.txt").checkpoint()->draft.text == draft);
        CHECK(!opened(reopened, "tracked.txt").facts().resolved);
    });

    suite.run("leaveSession fails the launched tool without staging, restores MERGED and a new session restores the drafts", [] {
        ParentRepository repo(launcherBinary, {conflict("tracked.txt", "launch bytes\n"),
                                               conflict("other.txt", "other launch bytes\n")});
        ScopedEnvironment environment(repo.environment);
        const auto indexBefore = repo.index();
        auto controller = std::make_unique<ApplicationController>(gitOnPath());
        openController(*controller, repo);
        auto parent = startParent(repo, ParentRepository::Tool, {"tracked.txt"});
        waitForWaiter(*controller, "tracked.txt");
        const auto intake = controller->mergetoolIntakes().front();
        const auto drafts = saveDrafts(*controller, {"tracked.txt", "other.txt"});
        writeFile(repo.path("tracked.txt"), "written by another editor\n");
        SessionEnd end;
        watchSessionEnd(*controller, end);
        CHECK(controller->leaveSession());
        CHECK(end.count == 1 && !end.clean);
        CHECK(end.outcome.contains(QStringLiteral("1 launched path returned to Git unresolved.")));
        // The session stays alive here, so only the explicit leave frame can release the launcher.
        waitParent(*parent, 1, controller->commandMessage());
        const auto errors = parent->readAllStandardError();
        CHECK(errors.contains("merge of tracked.txt failed"));
        CHECK(!errors.contains("closed before an explicit decision"));
        CHECK(!controller->mergetoolHasWaiter("tracked.txt"));
        CHECK(readFile(repo.path("tracked.txt")) == "launch bytes\n");
        CHECK(readFile(repo.path("other.txt")) == "other launch bytes\n");
        CHECK(repo.index() == indexBefore);
        CHECK(repo.gitTemporaries("tracked.txt").isEmpty());
        CHECK(readFile(intake + "/merged") == "launch bytes\n");
        CHECK(!controller->leaveSession() && end.count == 1);
        controller.reset();
        checkRecovered(repo, drafts);
    });

    suite.run("leaveSession during a multi-path run sends the waiting launcher a leave and Git stages nothing more", [] {
        ParentRepository repo(launcherBinary, {conflict("alpha.txt", "alpha launch\n"),
                                               conflict("beta.txt", "beta launch\n"),
                                               conflict("gamma.txt", "gamma launch\n")});
        ScopedEnvironment environment(repo.environment);
        auto controller = std::make_unique<ApplicationController>(gitOnPath());
        openController(*controller, repo);
        auto parent = startParent(repo, ParentRepository::GatedTool, {});
        // Resolve alpha.txt first, so the leave must skip a launcher that already has its decision.
        waitForWaiter(*controller, "alpha.txt");
        editDraft(*controller, "alpha.txt", QStringLiteral("resolved before leaving\n"));
        const auto alpha = QByteArray::fromStdString(draftOf(*controller, "alpha.txt"));
        CHECK(controller->markResolved(select(*controller, "alpha.txt"), revisionOf(*controller, "alpha.txt"), true));
        pump([&] { return repo.exits() == "alpha.txt 0\n"; }, controller->commandMessage());
        repo.release();
        pump([&] {
            return opened(*controller, "alpha.txt").facts().resolved && controller->mergetoolWaitingPath() == "beta.txt";
        }, controller->commandMessage() + " | waiting=" + QString::fromLatin1(controller->mergetoolWaitingPath()));
        CHECK(repo.stagedObject("alpha.txt") == repo.objectOf(alpha));
        const auto indexBefore = repo.index();
        const auto drafts = saveDrafts(*controller, {"beta.txt", "gamma.txt"});
        SessionEnd end;
        watchSessionEnd(*controller, end);
        CHECK(controller->leaveSession());
        CHECK(end.count == 1 && !end.clean);
        CHECK(end.outcome.contains(QStringLiteral("Left with 2 of 3 files outstanding.")));
        CHECK(end.outcome.contains(QStringLiteral("1 launched path returned to Git unresolved.")));
        // The gated parent records the launcher's status, then holds before Git restores or stages.
        pump([&] { return repo.exits() == "alpha.txt 0\nbeta.txt 1\n"; }, controller->commandMessage());
        CHECK(readFile(repo.path("beta.txt")) == "beta launch\n");
        CHECK(repo.index() == indexBefore);
        repo.release();
        // Git asks whether to go on to gamma.txt. Answering no ends the run.
        QByteArray output;
        pump([&] {
            output += parent->readAllStandardOutput();
            return output.contains("Continue merging other unresolved paths");
        }, QStringLiteral("Git did not ask to continue after the failed path"));
        parent->write("n\n");
        waitParent(*parent, 1, controller->commandMessage());
        const auto errors = parent->readAllStandardError();
        CHECK(errors.contains("merge of beta.txt failed"));
        CHECK(!errors.contains("closed before an explicit decision"));
        CHECK(repo.index() == indexBefore);
        CHECK(repo.unmerged("beta.txt") && repo.unmerged("gamma.txt"));
        CHECK(readFile(repo.path("beta.txt")) == "beta launch\n");
        CHECK(readFile(repo.path("gamma.txt")) == "gamma launch\n");
        CHECK(repo.gitTemporaries("beta.txt").isEmpty() && repo.gitTemporaries("gamma.txt").isEmpty());
        // gamma.txt was never launched.
        CHECK(controller->mergetoolIntakes().size() == 2);
        controller.reset();
        checkRecovered(repo, drafts);
    });

    suite.run("a terminal interrupt stops Git and the launcher without staging and keeps the saved draft", [] {
        ParentRepository repo(launcherBinary, {conflict("tracked.txt", "launch bytes\n")});
        ScopedEnvironment environment(repo.environment);
        ApplicationController controller(gitOnPath());
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
        ApplicationController controller(gitOnPath());
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
        ApplicationController controller(gitOnPath());
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
        ApplicationController controller(gitOnPath());
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
        // The refusal comes from background Git inspections, which can take longer than the hold under load.
        pump([&] { return controller.commandMessage().contains(QStringLiteral("filesystem monitor setting: core.fsmonitor")); },
             QStringLiteral("Mark resolved was not refused for core.fsmonitor | message after the hold: ")
                 + controller.commandMessage());
        CHECK(controller.commandMessage().contains(QStringLiteral("filesystem monitor setting: core.fsmonitor")));
        CHECK(parentBlocked(*parent, controller, "tracked.txt") && !QFile::exists(sentinel)
              && !opened(controller, "tracked.txt").facts().resolved);
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
