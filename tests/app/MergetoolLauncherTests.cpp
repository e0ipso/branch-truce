#include "app/ApplicationController.h"
#include "app/MergetoolProtocol.h"
#include "TestHarness.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

#include <csignal>
#include <cstdio>
#include <functional>
#include <optional>
#include <sys/resource.h>

#include <memory>

using namespace branch_truce;

namespace {
QString launcherBinary;

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
    explicit Repository(QByteArray trackedText, QByteArray excludedText)
        : tracked(std::move(trackedText)), excluded(std::move(excludedText))
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
        environment.insert("BRANCH_TRUCE_MERGETOOL_NO_SPAWN", "1");
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
        writeFile(path("tracked.txt"), tracked);
        writeFile(path("excluded.txt"), excluded);
        notesObject = git({"rev-parse", ":notes.txt"}).trimmed();
        const auto command = QStringLiteral("MERGED=\"$MERGED\" BASE=\"$BASE\" LOCAL=\"$LOCAL\" REMOTE=\"$REMOTE\" '")
            + QString(launcherBinary).replace(QLatin1Char('\''), QStringLiteral("'\\''"))
            + QStringLiteral("' --mergetool");
        git({"config", "mergetool.truce-launcher.cmd", command});
        git({"config", "mergetool.truce-launcher.trustExitCode", "true"});
        git({"config", "mergetool.keepBackup", "false"});
        git({"config", "mergetool.keepTemporaries", "false"});
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
    QTemporaryDir directory;
    QTemporaryDir executables;
    QString root;
    QProcessEnvironment environment;
    QByteArray notesObject;
    QByteArray tracked;
    QByteArray excluded;
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

void pump(const std::function<bool()> &ready, const QString &detail)
{
    if (ready()) return;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QTimer pulse;
    pulse.setInterval(30);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&pulse, &QTimer::timeout, &loop, [&] {
        if (ready()) loop.quit();
    });
    timeout.start(25000);
    pulse.start();
    loop.exec();
    if (!ready()) throw std::runtime_error(detail.toStdString());
}

void openController(ApplicationController &controller, const Repository &repo)
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
    CHECK(!controller.parentSessionActive());
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

bool unmerged(const Repository &repo, const QByteArray &path)
{
    return repo.git({"diff", "--name-only", "--diff-filter=U", "-z"}).split('\0').contains(path);
}

std::unique_ptr<QProcess> startMergetool(const Repository &repo, const QStringList &paths)
{
    auto process = std::make_unique<QProcess>();
    process->setProcessEnvironment(repo.environment);
    process->setWorkingDirectory(repo.root);
    process->start("/usr/bin/git", QStringList{"--no-pager", "--literal-pathspecs", "mergetool", "--no-prompt",
                                               "--tool=truce-launcher", "--"} + paths);
    CHECK(process->waitForStarted(5000));
    return process;
}

void waitFinished(QProcess &process, int expected, ApplicationController &controller)
{
    pump([&] { return process.state() == QProcess::NotRunning; },
         controller.commandMessage() + " | mergetool still running");
    CHECK(process.exitStatus() == QProcess::NormalExit);
    if (process.exitCode() != expected)
        throw std::runtime_error("mergetool exit " + std::to_string(process.exitCode()) + "\n"
            + process.readAllStandardError().toStdString() + process.readAllStandardOutput().toStdString());
}

QString revisionOf(const FileSession &file)
{
    return QString::number(static_cast<qulonglong>(file.current().id));
}

// Environment for a launcher started directly on tracked.txt, without a Git parent.
QProcessEnvironment directLauncherEnvironment(const Repository &repo, const QTemporaryDir &sides)
{
    CHECK(sides.isValid());
    writeFile(sides.filePath("base"), "base\n");
    writeFile(sides.filePath("local"), "local\n");
    writeFile(sides.filePath("remote"), "remote\n");
    auto environment = repo.environment;
    environment.insert("MERGED", "tracked.txt");
    environment.insert("BASE", sides.filePath("base"));
    environment.insert("LOCAL", sides.filePath("local"));
    environment.insert("REMOTE", sides.filePath("remote"));
    return environment;
}

void rejectDuplicate(const Repository &repo, ApplicationController &controller, QProcess &parent)
{
    QTemporaryDir sides;
    QProcess duplicate;
    duplicate.setProcessEnvironment(directLauncherEnvironment(repo, sides));
    duplicate.setWorkingDirectory(repo.root);
    duplicate.start(launcherBinary, {"--mergetool"});
    CHECK(duplicate.waitForStarted(5000));
    pump([&] { return duplicate.state() == QProcess::NotRunning; }, "duplicate launcher still running");
    CHECK(duplicate.exitCode() != 0);
    CHECK(parent.state() == QProcess::Running);
    CHECK(controller.mergetoolWaitingPath() == "tracked.txt");
    CHECK(readFile(repo.path("tracked.txt")) == repo.tracked);
    CHECK(unmerged(repo, "tracked.txt"));
}

void makeExecutable(const QString &path)
{
    CHECK(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
}

// The launcher starts the GUI from its own executable path. It runs from a
// private copy whose path is then replaced by a stub that only records the
// spawn, so no GUI starts and nothing listens unless the test opens a session.
// A git wrapper holds repository discovery, and so the spawn, until the stub
// is in place.
class SpawnedLauncher {
public:
    explicit SpawnedLauncher(const Repository &repository) : repo(repository)
    {
        CHECK(stage.isValid() && QDir(stage.path()).mkdir(QStringLiteral("bin")));
        // GitAdapter trusts only executables whose file and directories nobody else can write.
        makeExecutable(stage.filePath(QStringLiteral("bin")));
        binary = stage.filePath(QStringLiteral("branch-truce"));
        marker = stage.filePath(QStringLiteral("spawned"));
        gate = stage.filePath(QStringLiteral("gate"));
        stub = stage.filePath(QStringLiteral("stub"));
        CHECK(QFile::copy(launcherBinary, binary));
        makeExecutable(binary);
        const auto git = stage.filePath(QStringLiteral("bin/git"));
        writeFile(git, "#!/bin/sh\nwhile [ ! -e '" + gate.toUtf8() + "' ]; do sleep 0.02; done\n"
                       "exec /usr/bin/git \"$@\"\n");
        makeExecutable(git);
        writeFile(stub, "#!/bin/sh\nprintf '%s' \"$1\" > '" + marker.toUtf8() + ".part' && mv '"
                        + marker.toUtf8() + ".part' '" + marker.toUtf8() + "'\n");
        makeExecutable(stub);
        // Linux names a replaced running image "<path> (deleted)" in /proc/self/exe,
        // and Qt 6.10 returns that name from applicationFilePath().
        const auto deleted = binary + QStringLiteral(" (deleted)");
        CHECK(QFile::copy(stub, deleted));
        makeExecutable(deleted);
        auto environment = directLauncherEnvironment(repo, sides);
        environment.remove(QStringLiteral("BRANCH_TRUCE_MERGETOOL_NO_SPAWN"));
        environment.insert(QStringLiteral("PATH"), stage.filePath(QStringLiteral("bin")) + QStringLiteral(":/usr/bin:/bin"));
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(repo.root);
    }
    void start()
    {
        clock.start();
        process.start(binary, {QStringLiteral("--mergetool")});
        CHECK(process.waitForStarted(5000));
        // The running image keeps its inode. The path it would spawn is now the stub.
        CHECK(::rename(QFile::encodeName(stub).constData(), QFile::encodeName(binary).constData()) == 0);
        writeFile(gate, {});
    }
    bool spawned() const { return QFile::exists(marker); }
    void finish(int expected)
    {
        pump([&] { return process.state() == QProcess::NotRunning; }, QStringLiteral("launcher still running"));
        elapsed = clock.elapsed();
        CHECK(process.exitStatus() == QProcess::NormalExit);
        errors = process.readAllStandardError();
        if (process.exitCode() != expected)
            throw std::runtime_error("launcher exit " + std::to_string(process.exitCode()) + ": " + errors.toStdString());
    }

    const Repository &repo;
    QTemporaryDir stage;
    QTemporaryDir sides;
    QString binary;
    QString marker;
    QString gate;
    QString stub;
    QProcess process;
    QElapsedTimer clock;
    qint64 elapsed = 0;
    QByteArray errors;
};

// Every file write fails with EFBIG while this is alive; truncation still works.
class NoFileGrowth {
public:
    NoFileGrowth()
    {
        previousHandler = std::signal(SIGXFSZ, SIG_IGN);
        CHECK(::getrlimit(RLIMIT_FSIZE, &previous) == 0);
        auto limited = previous;
        limited.rlim_cur = 0;
        CHECK(::setrlimit(RLIMIT_FSIZE, &limited) == 0);
    }
    ~NoFileGrowth()
    {
        ::setrlimit(RLIMIT_FSIZE, &previous);
        std::signal(SIGXFSZ, previousHandler);
    }
private:
    struct rlimit previous {};
    void (*previousHandler)(int) = nullptr;
};
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (argc != 2) {
        QTextStream(stderr) << "usage: mergetool-launcher-tests <branch-truce>\n";
        return 2;
    }
    launcherBinary = QString::fromLocal8Bit(argv[1]);
    test_support::Suite suite("MergetoolLauncherTests");
    suite.run("installed Git stages only after an explicit resolution and a repeated launch keeps the first inputs", [] {
        Repository repo("resolved from the app\n", "excluded stays conflicted\n");
        ScopedEnvironment environment(repo.environment);
        ApplicationController controller;
        openController(controller, repo);
        auto parent = startMergetool(repo, {"tracked.txt"});
        pump([&] { return controller.mergetoolWaitingPath() == "tracked.txt"; }, controller.commandMessage());
        CHECK(controller.mergetoolIntakes().size() == 1);
        const auto firstIntake = controller.mergetoolIntakes().front();
        const auto firstMerged = readFile(firstIntake + "/merged");
        CHECK(firstMerged == repo.tracked);
        CHECK(QFile::exists(firstIntake + "/base") && QFile::exists(firstIntake + "/local") && QFile::exists(firstIntake + "/remote"));
        const auto id = select(controller, "tracked.txt");
        CHECK(controller.saveDraft(id, revisionOf(opened(controller, "tracked.txt"))));
        CHECK(controller.commandMessage().contains(QStringLiteral("repository file unchanged")));
        CHECK(readFile(repo.path("tracked.txt")) == repo.tracked);
        CHECK(unmerged(repo, "tracked.txt") && unmerged(repo, "excluded.txt"));
        CHECK(parent->state() == QProcess::Running);
        rejectDuplicate(repo, controller, *parent);
        CHECK(controller.mergetoolIntakes().size() == 1);
        CHECK(readFile(firstIntake + "/merged") == firstMerged);
        CHECK(controller.markResolved(id, revisionOf(opened(controller, "tracked.txt")), true));
        CHECK(parent->state() == QProcess::Running);
        CHECK(unmerged(repo, "tracked.txt"));
        waitFinished(*parent, 0, controller);
        CHECK(!unmerged(repo, "tracked.txt"));
        pump([&] { return opened(controller, "tracked.txt").facts().resolved; }, controller.commandMessage());
        CHECK(repo.git({"show", ":tracked.txt"}) == repo.tracked);
        CHECK(readFile(repo.path("tracked.txt")) == repo.tracked);
        CHECK(unmerged(repo, "excluded.txt"));
        CHECK(repo.git({"rev-parse", ":notes.txt"}).trimmed() == repo.notesObject);
        CHECK(QFile::exists(firstIntake + "/merged"));

        auto again = startMergetool(repo, {"excluded.txt"});
        pump([&] { return controller.mergetoolWaitingPath() == "excluded.txt"; }, controller.commandMessage());
        CHECK(controller.mergetoolIntakes().size() == 2);
        CHECK(readFile(firstIntake + "/merged") == firstMerged);
        CHECK(controller.mergetoolIntakes().at(1) != firstIntake);
        const auto other = select(controller, "excluded.txt");
        CHECK(controller.saveDraft(other, revisionOf(opened(controller, "excluded.txt"))));
        CHECK(readFile(repo.path("excluded.txt")) == repo.excluded);
        CHECK(controller.leaveMergetool());
        waitFinished(*again, 1, controller);
        CHECK(readFile(repo.path("excluded.txt")) == repo.excluded);
        CHECK(unmerged(repo, "excluded.txt"));
        CHECK(opened(controller, "excluded.txt").checkpoint());
        CHECK(!opened(controller, "excluded.txt").facts().resolved);
        CHECK(readFile(firstIntake + "/merged") == firstMerged);
        CHECK(repo.git({"rev-parse", ":notes.txt"}).trimmed() == repo.notesObject);
    });
    suite.run("cancel restores the parent-visible file and keeps the app checkpoint", [] {
        Repository repo("cancel keeps this draft\n", "excluded stays conflicted\n");
        ScopedEnvironment environment(repo.environment);
        ApplicationController controller;
        openController(controller, repo);
        auto parent = startMergetool(repo, {"tracked.txt"});
        pump([&] { return controller.mergetoolWaitingPath() == "tracked.txt"; }, controller.commandMessage());
        const auto intake = controller.mergetoolIntakes().front();
        const auto id = select(controller, "tracked.txt");
        CHECK(controller.saveDraft(id, revisionOf(opened(controller, "tracked.txt"))));
        CHECK(controller.leaveMergetool());
        waitFinished(*parent, 1, controller);
        CHECK(readFile(repo.path("tracked.txt")) == repo.tracked);
        CHECK(unmerged(repo, "tracked.txt") && unmerged(repo, "excluded.txt"));
        CHECK(opened(controller, "tracked.txt").checkpoint());
        CHECK(!opened(controller, "tracked.txt").facts().resolved);
        CHECK(readFile(intake + "/merged") == repo.tracked);
        CHECK(QFile::exists(intake + "/local"));
    });
    suite.run("multi-file Git mergetool blocks the second path until the first launcher is released", [] {
        Repository repo("first path resolved\n", "second path left unresolved\n");
        ScopedEnvironment environment(repo.environment);
        ApplicationController controller;
        openController(controller, repo);
        auto parent = startMergetool(repo, {});
        pump([&] {
            const auto path = controller.mergetoolWaitingPath();
            return path == "tracked.txt" || path == "excluded.txt";
        }, controller.commandMessage());
        const auto first = controller.mergetoolWaitingPath();
        const auto second = first == "tracked.txt" ? QByteArray("excluded.txt") : QByteArray("tracked.txt");
        const auto firstText = first == "tracked.txt" ? repo.tracked : repo.excluded;
        const auto secondText = second == "tracked.txt" ? repo.tracked : repo.excluded;
        const auto other = select(controller, second);
        CHECK(!controller.markResolved(other, revisionOf(opened(controller, second)), true));
        CHECK(controller.commandMessage().contains(QStringLiteral("Waiting for Git to launch this path")));
        CHECK(unmerged(repo, first) && unmerged(repo, second));
        CHECK(parent->state() == QProcess::Running);
        CHECK(controller.mergetoolWaitingPath() == first);
        const auto firstId = select(controller, first);
        CHECK(controller.saveDraft(firstId, revisionOf(opened(controller, first))));
        CHECK(readFile(repo.path(QString::fromLatin1(first))) == firstText);
        CHECK(controller.markResolved(firstId, revisionOf(opened(controller, first)), true));
        pump([&] {
            return opened(controller, first).facts().resolved && controller.mergetoolWaitingPath() == second
                && parent->state() == QProcess::Running;
        }, controller.commandMessage() + " | waiting=" + QString::fromLatin1(controller.mergetoolWaitingPath()));
        CHECK(controller.mergetoolIntakes().size() == 2);
        CHECK(readFile(controller.mergetoolIntakes().front() + "/merged") == firstText);
        CHECK(readFile(controller.mergetoolIntakes().at(1) + "/merged") == secondText);
        CHECK(repo.git({"show", QString::fromLatin1(":" + first)}) == firstText);
        CHECK(select(controller, second) == QString::fromLatin1(second.toHex()));
        CHECK(controller.leaveMergetool());
        waitFinished(*parent, 1, controller);
        CHECK(!unmerged(repo, first));
        CHECK(unmerged(repo, second));
        CHECK(readFile(repo.path(QString::fromLatin1(second))) == secondText);
        CHECK(opened(controller, first).checkpoint());
        CHECK(!opened(controller, second).facts().resolved);
        CHECK(repo.git({"rev-parse", ":notes.txt"}).trimmed() == repo.notesObject);
    });
    suite.run("TRUCE-0065 a session that listens after the GUI start is attached within the deadline", [] {
        Repository repo("attached after the spawn\n", "excluded stays conflicted\n");
        ScopedEnvironment environment(repo.environment);
        ApplicationController controller;
        SpawnedLauncher launcher(repo);
        launcher.start();
        pump([&] { return launcher.spawned() || launcher.process.state() == QProcess::NotRunning; },
             QStringLiteral("the launcher did not start a session"));
        CHECK(launcher.spawned() && readFile(launcher.marker) == repo.root.toUtf8());
        // Retries that fail at once would have given up long before this.
        QElapsedTimer pause;
        pause.start();
        while (pause.elapsed() < 1500) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        CHECK(launcher.process.state() == QProcess::Running);
        openController(controller, repo);
        pump([&] { return controller.mergetoolWaitingPath() == "tracked.txt"; }, controller.commandMessage());
        CHECK(launcher.clock.elapsed() < 75 * 200);
        CHECK(controller.mergetoolIntakes().size() == 1);
        CHECK(readFile(controller.mergetoolIntakes().front() + "/merged") == repo.tracked);
        select(controller, "tracked.txt");
        CHECK(controller.leaveMergetool());
        launcher.finish(1);
        CHECK(!launcher.errors.contains("No Branch Truce session is listening"));
        CHECK(readFile(repo.path("tracked.txt")) == repo.tracked);
        CHECK(unmerged(repo, "tracked.txt") && unmerged(repo, "excluded.txt"));
    });
    suite.run("TRUCE-0065 a started session that never listens fails after the wall-clock deadline", [] {
        Repository repo("nobody listens\n", "excluded stays conflicted\n");
        SpawnedLauncher launcher(repo);
        launcher.start();
        launcher.finish(1);
        CHECK(launcher.spawned() && readFile(launcher.marker) == repo.root.toUtf8());
        CHECK(launcher.errors.contains("mergetool: No Branch Truce session is listening for this repository.\n"));
        CHECK(launcher.elapsed >= 75 * 200);
        CHECK(readFile(repo.path("tracked.txt")) == repo.tracked);
        CHECK(unmerged(repo, "tracked.txt"));
    });
    suite.run("TRUCE-0066 a leave after a longer write restores exactly the launch bytes", [] {
        Repository repo("short launch\n", "excluded stays conflicted\n");
        ScopedEnvironment environment(repo.environment);
        ApplicationController controller;
        openController(controller, repo);
        // A direct launcher has no Git parent whose backup move would hide a leftover tail.
        QTemporaryDir sides;
        QProcess launcher;
        launcher.setProcessEnvironment(directLauncherEnvironment(repo, sides));
        launcher.setWorkingDirectory(repo.root);
        launcher.start(launcherBinary, {"--mergetool"});
        CHECK(launcher.waitForStarted(5000));
        pump([&] { return controller.mergetoolWaitingPath() == "tracked.txt"; }, controller.commandMessage());
        const auto longer = QByteArray("a GUI write longer than the launch bytes\n").repeated(32);
        writeFile(repo.path("tracked.txt"), longer);
        select(controller, "tracked.txt");
        CHECK(controller.leaveMergetool());
        waitFinished(launcher, 1, controller);
        CHECK(readFile(repo.path("tracked.txt")) == repo.tracked);
        CHECK(QFileInfo(repo.path("tracked.txt")).size() == repo.tracked.size());
        CHECK(unmerged(repo, "tracked.txt") && unmerged(repo, "excluded.txt"));
    });
    suite.run("TRUCE-0066 a restore writes exactly the launch bytes and a failed write keeps the longer file", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        const auto path = QFileInfo(directory.path()).canonicalFilePath() + QStringLiteral("/merged.txt");
        const auto native = QFile::encodeName(path);
        const QByteArray launch("<<<<<<< ours\nlaunch\n=======\nother\n>>>>>>> theirs\n");
        const auto longer = launch + QByteArray("a longer GUI write\n").repeated(64);
        QString error;
        writeFile(path, longer);
        CHECK(restoreMergetoolFile(native, launch, &error) && error.isEmpty());
        CHECK(readFile(path) == launch);
        writeFile(path, QByteArray(launch.size(), 'e'));
        CHECK(restoreMergetoolFile(native, launch, &error) && readFile(path) == launch);
        writeFile(path, "short\n");
        CHECK(restoreMergetoolFile(native, longer, &error) && readFile(path) == longer);
        writeFile(path, longer);
        bool restored = true;
        {
            NoFileGrowth limit;
            restored = restoreMergetoolFile(native, launch, &error);
        }
        CHECK(!restored);
        CHECK(error == QStringLiteral("The original file could not be restored."));
        CHECK(readFile(path) == longer);
    });
    return suite.report();
}
