#include "app/ApplicationController.h"
#include "TestHarness.h"
#include "fixtures/RecoveryFaults.h"
#include <QCoreApplication>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QMap>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>

using namespace branch_truce;

namespace {
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

class Repository {
public:
    Repository()
    {
        CHECK(directory.isValid() && executables.isValid());
        root = QFileInfo(directory.path()).canonicalFilePath();
        environment.insert("PATH", "/usr/bin:/bin");
        environment.insert("HOME", executables.path());
        environment.insert("XDG_CONFIG_HOME", executables.path() + "/config");
        environment.insert("XDG_CACHE_HOME", executables.path() + "/cache");
        environment.insert("XDG_DATA_HOME", executables.path() + "/data");
        environment.insert("LC_ALL", "C");
        environment.insert("GIT_CONFIG_NOSYSTEM", "1");
        environment.insert("GIT_CONFIG_GLOBAL", "/dev/null");
        environment.insert("GIT_TERMINAL_PROMPT", "0");
        git({"init", "--quiet", "--initial-branch=main"});
        git({"config", "user.name", "Branch Truce Tests"});
        git({"config", "user.email", "tests@example.invalid"});
        git({"config", "core.autocrlf", "false"});
        writeFile(path("tracked.txt"), "base\n");
        writeFile(path("excluded.txt"), "base\n");
        writeFile(path("notes.txt"), "notes stay staged\n");
        git({"add", "--", "tracked.txt", "excluded.txt", "notes.txt"});
        git({"commit", "--quiet", "-m", "base"});
        git({"checkout", "--quiet", "-b", "side"});
        writeFile(path("tracked.txt"), "side\n");
        writeFile(path("excluded.txt"), "side\n");
        git({"add", "--", "tracked.txt", "excluded.txt"});
        git({"commit", "--quiet", "-m", "side"});
        git({"checkout", "--quiet", "main"});
        writeFile(path("tracked.txt"), "main\n");
        writeFile(path("excluded.txt"), "main\n");
        git({"add", "--", "tracked.txt", "excluded.txt"});
        git({"commit", "--quiet", "-m", "main"});
        git({"merge", "--no-edit", "side"}, 1);
        writeFile(path("tracked.txt"), manual);
        notesObject = git({"rev-parse", ":notes.txt"}).trimmed();
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
            throw std::runtime_error("fixture git: " + process.readAllStandardError().toStdString());
        return process.readAllStandardOutput();
    }
    QMap<QString, QByteArray> snapshot() const
    {
        QMap<QString, QByteArray> result;
        QDirIterator files(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const auto selected = files.next();
            result.insert(QDir(root).relativeFilePath(selected), readFile(selected));
        }
        return result;
    }
    QTemporaryDir directory;
    QTemporaryDir executables;
    QString root;
    QProcessEnvironment environment;
    QByteArray notesObject;
    const QByteArray manual = "resolved without a prior save\n";
};

class ScopedEnvironment {
public:
    explicit ScopedEnvironment(const QProcessEnvironment &environment)
    {
        for (const auto name : {"HOME", "PATH", "LC_ALL", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
                                "XDG_DATA_HOME", "GIT_CONFIG_NOSYSTEM", "GIT_CONFIG_GLOBAL", "GIT_TERMINAL_PROMPT"}) {
            previous.insert(name, qEnvironmentVariableIsSet(name)
                ? std::optional<QByteArray>(qgetenv(name)) : std::nullopt);
            qputenv(name, environment.value(QString::fromLatin1(name)).toUtf8());
        }
    }
    ~ScopedEnvironment()
    {
        for (auto entry = previous.cbegin(); entry != previous.cend(); ++entry) {
            if (entry.value()) qputenv(entry.key().constData(), *entry.value());
            else qunsetenv(entry.key().constData());
        }
    }
private:
    QMap<QByteArray, std::optional<QByteArray>> previous;
};

void openController(ApplicationController &controller, const Repository &repo)
{
    const auto before = repo.snapshot();
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
    CHECK(repo.snapshot() == before);
}

const FileSession &openedFile(ApplicationController &controller, const char *path = "tracked.txt")
{
    const auto *selected = controller.files()->file(QByteArray(path).toHex().toStdString());
    CHECK(selected != nullptr);
    return *selected;
}

QString selectTracked(ApplicationController &controller)
{
    const auto id = QString::fromLatin1(QByteArray("tracked.txt").toHex());
    CHECK(controller.selectFile(id));
    return id;
}

QString revisionOf(const FileSession &file)
{
    return QString::number(static_cast<qulonglong>(file.current().id));
}

void waitFor(ApplicationController &controller, const std::function<bool()> &ready)
{
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 25000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    if (!ready())
        throw std::runtime_error((controller.commandMessage() + " | "
            + controller.selectedFile().value("status").toString() + " | busy="
            + QString::number(controller.resolutionBusy())).toStdString());
}

QSet<QByteArray> unmerged(const Repository &repo)
{
    QSet<QByteArray> result;
    for (const auto &path : repo.git({"diff", "--name-only", "--diff-filter=U", "-z"}).split('\0'))
        if (!path.isEmpty()) result.insert(path);
    return result;
}

void expectUnchanged(const Repository &repo, const QMap<QString, QByteArray> &before, bool ignoreObjects = false)
{
    const auto after = repo.snapshot();
    QStringList changed;
    const auto skip = [&](const QString &path) { return ignoreObjects && path.startsWith(QStringLiteral(".git/objects/")); };
    for (auto it = after.cbegin(); it != after.cend(); ++it)
        if (!skip(it.key()) && (!before.contains(it.key()) || before.value(it.key()) != it.value())) changed << it.key();
    for (auto it = before.cbegin(); it != before.cend(); ++it)
        if (!skip(it.key()) && !after.contains(it.key())) changed << ("missing " + it.key());
    if (!changed.isEmpty()) throw std::runtime_error(changed.join(", ").toStdString());
    CHECK(changed.isEmpty());
}

struct Session {
    Repository repo;
    ScopedEnvironment environment;
    ApplicationController controller;
    QString id;
    explicit Session() : environment(repo.environment)
    {
        openController(controller, repo);
        id = selectTracked(controller);
        CHECK(openedFile(controller).facts().unsaved);
        CHECK(openedFile(controller).facts().undecided > 0);
        CHECK(readFile(repo.path("tracked.txt")) == repo.manual);
        CHECK(unmerged(repo).contains("tracked.txt") && unmerged(repo).contains("excluded.txt"));
    }
    QString revision() { return revisionOf(openedFile(controller)); }
};
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("branch-truce"));
    QCoreApplication::setApplicationName(QStringLiteral("branch-truce-resolution-tests"));
    test_support::Suite suite("resolution");
    suite.run("confirmation, cancellation and a stale revision leave the repository unpublished", [] {
        Session session;
        const auto before = session.repo.snapshot();
        const auto text = openedFile(session.controller).current().draft.text;
        CHECK(!session.controller.markResolved(session.id, session.revision(), false));
        CHECK(session.controller.resolutionConfirmation());
        CHECK(session.controller.commandMessage().contains(QStringLiteral("intentional")));
        CHECK(!openedFile(session.controller).reviewed());
        expectUnchanged(session.repo, before);
        session.controller.cancelResolution();
        CHECK(!session.controller.resolutionConfirmation());
        CHECK(openedFile(session.controller).current().draft.text == text);
        const auto stale = session.revision();
        auto edited = openedFile(session.controller).current().draft;
        edited.text += "later edit\n";
        CHECK(session.controller.files()->replaceDraft(session.id.toStdString(), openedFile(session.controller).current().id, edited)
            == Transition::Applied);
        CHECK(!session.controller.markResolved(session.id, stale, true));
        CHECK(session.controller.commandMessage().contains(QStringLiteral("changed")));
        CHECK(readFile(session.repo.path("tracked.txt")) == session.repo.manual);
        expectUnchanged(session.repo, before);
        CHECK(!openedFile(session.controller).facts().resolved);
    });
    suite.run("a failed review persist publishes nothing", [] {
        Session session;
        const auto before = session.repo.snapshot();
        recovery_faults::Failure fault(recovery_faults::Point::JournalSync);
        CHECK(!session.controller.markResolved(session.id, session.revision(), true));
        CHECK(recovery_faults::failures == 1);
        CHECK(session.controller.files()->hasPendingRecovery());
        CHECK(!openedFile(session.controller).reviewed());
        expectUnchanged(session.repo, before);
        CHECK(unmerged(session.repo).contains("tracked.txt"));
    });
    suite.run("a rejected repository write keeps the draft and the conflict", [] {
        Session session;
        CHECK(QFile::setPermissions(session.repo.path("tracked.txt"), QFileDevice::ReadOwner));
        const auto before = session.repo.snapshot();
        CHECK(session.controller.markResolved(session.id, session.revision(), true));
        waitFor(session.controller, [&] {
            return !session.controller.resolutionBusy() && openedFile(session.controller).writeOutcome()
                && openedFile(session.controller).writeOutcome()->outcome != Outcome::Succeeded;
        });
        const auto outcome = openedFile(session.controller).writeOutcome()->outcome;
        CHECK(outcome == Outcome::Failed || outcome == Outcome::Uncertain);
        CHECK(openedFile(session.controller).reviewed() && openedFile(session.controller).checkpoint());
        CHECK(!openedFile(session.controller).facts().resolved && !openedFile(session.controller).staged());
        CHECK(readFile(session.repo.path("tracked.txt")) == session.repo.manual);
        expectUnchanged(session.repo, before, true);
        CHECK(unmerged(session.repo).contains("tracked.txt") && unmerged(session.repo).contains("excluded.txt"));
        CHECK(session.repo.git({"rev-parse", ":notes.txt"}).trimmed() == session.repo.notesObject);
    });
    suite.run("an unsupported staging hook leaves the written file unresolved", [] {
        Session session;
        const auto hook = session.repo.path(".git/hooks/post-index-change");
        writeFile(hook, "#!/bin/sh\nexit 1\n");
        CHECK(QFile::setPermissions(hook, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const auto before = session.repo.snapshot();
        CHECK(session.controller.markResolved(session.id, session.revision(), true));
        waitFor(session.controller, [&] {
            const auto &file = openedFile(session.controller);
            return !session.controller.resolutionBusy() && file.writeOutcome()
                && file.writeOutcome()->outcome == Outcome::Succeeded && file.stageOutcome();
        });
        CHECK(openedFile(session.controller).stageOutcome()->outcome == Outcome::Failed
            || openedFile(session.controller).stageOutcome()->outcome == Outcome::Uncertain);
        CHECK(!openedFile(session.controller).facts().resolved);
        CHECK(openedFile(session.controller).facts().status == FileStatus::StageFailed
            || openedFile(session.controller).facts().status == FileStatus::OutcomeUncertain);
        CHECK(readFile(session.repo.path("tracked.txt")) == session.repo.manual);
        expectUnchanged(session.repo, before, true);
        CHECK(unmerged(session.repo).contains("tracked.txt"));
        CHECK(session.controller.commandMessage().contains(QStringLiteral("unresolved"))
            || session.controller.selectedFile().value("status").toString().contains(QStringLiteral("unresolved"))
            || session.controller.selectedFile().value("status").toString().contains(QStringLiteral("uncertain")));
    });
    suite.run("Mark resolved publishes one unsaved path and leaves the other conflict", [] {
        Session session;
        CHECK(!openedFile(session.controller).checkpoint());
        CHECK(session.controller.markResolved(session.id, session.revision(), true));
        waitFor(session.controller, [&] { return openedFile(session.controller).facts().resolved; });
        CHECK(openedFile(session.controller).checkpoint());
        CHECK(openedFile(session.controller).facts().status == FileStatus::Resolved);
        CHECK(session.controller.selectedFile().value("status").toString() == "Written and resolved");
        CHECK(session.repo.git({"show", ":tracked.txt"}) == session.repo.manual);
        CHECK(readFile(session.repo.path("tracked.txt")) == session.repo.manual);
        const auto remaining = unmerged(session.repo);
        CHECK(!remaining.contains("tracked.txt") && remaining.contains("excluded.txt") && !remaining.contains("notes.txt"));
        CHECK(session.repo.git({"rev-parse", ":notes.txt"}).trimmed() == session.repo.notesObject);
        CHECK(!session.controller.files()->canFinish());
        CHECK(!session.controller.parentStagingOffer(session.id));
    });
    suite.run("parent-owned staging waits for Git and resolves only after that index entry matches", [] {
        Session session;
        CHECK(session.controller.assignParentStaging("tracked.txt"));
        CHECK(session.controller.markResolved(session.id, session.revision(), true));
        waitFor(session.controller, [&] {
            return !session.controller.resolutionBusy() && session.controller.parentStagingOffer(session.id).has_value();
        });
        CHECK(openedFile(session.controller).writeOutcome()
            && openedFile(session.controller).writeOutcome()->outcome == Outcome::Succeeded);
        CHECK(!openedFile(session.controller).stageOutcome());
        CHECK(!openedFile(session.controller).facts().resolved);
        CHECK(readFile(session.repo.path("tracked.txt")) == session.repo.manual);
        CHECK(unmerged(session.repo).contains("tracked.txt") && unmerged(session.repo).contains("excluded.txt"));
        const auto offered = session.controller.parentStagingOffer(session.id);
        CHECK(offered && offered->path == "tracked.txt" && !offered->canonicalObjectId.isEmpty());
        CHECK(session.controller.parentStagingPath().endsWith(QStringLiteral("tracked.txt")));
        CHECK(session.controller.verifyParentAcknowledgment(session.id, session.revision()));
        waitFor(session.controller, [&] { return !session.controller.resolutionBusy(); });
        CHECK(!openedFile(session.controller).facts().resolved);
        CHECK(session.controller.parentStagingOffer(session.id));
        CHECK(unmerged(session.repo).contains("tracked.txt"));
        session.repo.git({"add", "--", "tracked.txt"});
        CHECK(!unmerged(session.repo).contains("tracked.txt"));
        CHECK(session.controller.verifyParentAcknowledgment(session.id, session.revision()));
        waitFor(session.controller, [&] { return openedFile(session.controller).facts().resolved; });
        CHECK(!session.controller.parentStagingOffer(session.id));
        CHECK(unmerged(session.repo).contains("excluded.txt"));
        CHECK(session.repo.git({"rev-parse", ":notes.txt"}).trimmed() == session.repo.notesObject);
        CHECK(session.repo.git({"show", ":tracked.txt"}) == session.repo.manual);
        CHECK(!session.controller.files()->canFinish());
    });
    return suite.report();
}
