#include "app/ApplicationController.h"
#include "app/MergetoolLauncher.h"
#include "app/MergetoolProtocol.h"
#include "TestHarness.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include <memory>

using namespace branch_truce;

namespace peer_credentials {
// Set only in the --foreign-peer-launcher child of this test binary.
bool foreign = false;
}

// Linked into this test executable only. The launcher's SO_PEERCRED query then
// reports another uid, as a socket answered by a different user would.
extern "C" int __real_getsockopt(int descriptor, int level, int name, void *value, socklen_t *size);
extern "C" int __wrap_getsockopt(int descriptor, int level, int name, void *value, socklen_t *size)
{
    const int result = __real_getsockopt(descriptor, level, name, value, size);
    if (result == 0 && peer_credentials::foreign && level == SOL_SOCKET && name == SO_PEERCRED
        && value != nullptr && size != nullptr && *size == sizeof(ucred))
        static_cast<ucred *>(value)->uid = ::geteuid() + 1;
    return result;
}

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

QString privateDirectory(const QString &path)
{
    CHECK(QDir().mkpath(path)
          && QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    return path;
}

class Repository {
public:
    explicit Repository(QByteArray trackedText, QByteArray excludedText)
        : tracked(std::move(trackedText)), excluded(std::move(excludedText))
    {
        CHECK(directory.isValid() && executables.isValid() && runtime.isValid());
        root = QFileInfo(directory.path()).canonicalFilePath();
        environment.insert("PATH", "/usr/bin:/bin");
        environment.insert("HOME", executables.path());
        environment.insert("XDG_CONFIG_HOME", executables.path() + "/config");
        environment.insert("XDG_CACHE_HOME", executables.path() + "/cache");
        environment.insert("XDG_DATA_HOME", executables.path() + "/data");
        // The session socket lives here, so the in-process session and every launcher meet.
        environment.insert("XDG_RUNTIME_DIR", privateDirectory(runtime.path()));
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
    // Under a long TMPDIR a QTemporaryDir would push the socket path past its
    // length limit, and every session would fall back to the shared
    // /tmp/branch-truce-<uid>. A short base under /tmp keeps it private.
    QTemporaryDir runtime{QStringLiteral("/tmp/bt-run-XXXXXX")};
    QString root;
    QProcessEnvironment environment;
    QByteArray notesObject;
    QByteArray tracked;
    QByteArray excluded;
};

// Variables the fixture leaves out are unset in this process while it lives.
class ScopedEnvironment {
public:
    explicit ScopedEnvironment(const QProcessEnvironment &environment)
    {
        for (const auto name : {"HOME", "PATH", "LC_ALL", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME",
                                "XDG_RUNTIME_DIR", "TMPDIR", "GIT_CONFIG_NOSYSTEM", "GIT_CONFIG_GLOBAL",
                                "GIT_TERMINAL_PROMPT"}) {
            previous.insert(name, qEnvironmentVariableIsSet(name)
                ? std::optional<QByteArray>(qgetenv(name)) : std::nullopt);
            const auto key = QString::fromLatin1(name);
            if (environment.contains(key)) qputenv(name, environment.value(key).toUtf8());
            else qunsetenv(name);
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

// The session in this process listens in the fixture's private runtime directory, not a fallback.
QString checkSocketDirectory(const Repository &repo)
{
    const auto runtime = repo.environment.value(QStringLiteral("XDG_RUNTIME_DIR"));
    CHECK(!runtime.isEmpty() && qEnvironmentVariable("XDG_RUNTIME_DIR") == runtime);
    const auto socket = mergetoolSocketDirectory();
    if (socket != runtime + QStringLiteral("/branch-truce"))
        throw std::runtime_error("socket directory outside the private runtime directory: " + socket.toStdString());
    return socket;
}

void startController(ApplicationController &controller, const Repository &repo, const QString &preferredFileId = {})
{
    checkSocketDirectory(repo);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&controller, &ApplicationController::launchChanged, &loop, [&] {
        if (!controller.opening()) loop.quit();
    });
    CHECK(controller.openRepository(repo.root, preferredFileId));
    timeout.start(15000);
    if (controller.opening()) loop.exec();
    CHECK(!controller.opening());
}

void openController(ApplicationController &controller, const Repository &repo, const QString &preferredFileId = {})
{
    startController(controller, repo, preferredFileId);
    if (controller.startupState() != "ready")
        throw std::runtime_error("controller startup: " + controller.detail().toStdString());
    CHECK(controller.mergetoolListening());
    CHECK(QFileInfo(checkSocketDirectory(repo)).isDir());
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
        // The stub records its arguments, one per line, and where its standard streams point.
        // Command substitution runs in a child, so /proc/$$ still shows the stub's own streams.
        writeFile(stub, "#!/bin/sh\nstreams=$(for fd in 0 1 2; do readlink /proc/$$/fd/$fd; done)\n"
                        "printf '%s\\n' \"$streams\" > '" + marker.toUtf8()
                        + ".streams'\nprintf '%s\\n' \"$@\" > '" + marker.toUtf8() + ".part' && mv '"
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
    // The GUI start names the launched path before the worktree, and keeps the
    // launcher's terminal out of the GUI's standard streams.
    QString checkSpawnArguments() const
    {
        const auto launched = QByteArray("tracked.txt").toHex();
        const auto arguments = readFile(marker);
        const auto streams = readFile(marker + QStringLiteral(".streams"));
        if (arguments.split('\n') != QList<QByteArray>{"--mergetool-path", launched, repo.root.toUtf8(), ""}
            || streams != "/dev/null\n/dev/null\n/dev/null\n")
            throw std::runtime_error("spawned GUI arguments:\n" + arguments.toStdString()
                                     + "streams:\n" + streams.toStdString());
        return QString::fromLatin1(launched);
    }
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
    // Child mode: run the real launcher code from this binary with a foreign peer uid.
    if (argc == 2 && std::strcmp(argv[1], "--foreign-peer-launcher") == 0) {
        peer_credentials::foreign = true;
        char mergetool[] = "--mergetool";
        char *launcherArguments[] = {argv[0], mergetool, nullptr};
        return runMergetoolLauncher(2, launcherArguments);
    }
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
    suite.run("selecting the launched path after Git attached notifies the waiting state and the staging path", [] {
        Repository repo("selected after the attach\n", "excluded stays conflicted\n");
        ScopedEnvironment environment(repo.environment);
        ApplicationController controller;
        openController(controller, repo);
        // QML reads both properties again only when their notify signal fires.
        const auto *meta = controller.metaObject();
        for (const auto *name : {"mergetoolWaiting", "parentStagingPath"}) {
            CHECK(meta->property(meta->indexOfProperty(name)).notifySignal()
                  == QMetaMethod::fromSignal(&ApplicationController::commandChanged));
        }
        struct Observed {
            bool waiting = false;
            QString staging;
        };
        QList<Observed> seen;
        QObject::connect(&controller, &ApplicationController::commandChanged, &controller, [&] {
            seen.append({controller.mergetoolWaiting(), controller.parentStagingPath()});
        });
        const auto other = select(controller, "excluded.txt");
        // A direct launcher has no Git parent, so the written offer stays unstaged.
        QTemporaryDir sides;
        QProcess launcher;
        launcher.setProcessEnvironment(directLauncherEnvironment(repo, sides));
        launcher.setWorkingDirectory(repo.root);
        launcher.start(launcherBinary, {"--mergetool"});
        CHECK(launcher.waitForStarted(5000));
        pump([&] { return controller.mergetoolWaitingPath() == "tracked.txt"; }, controller.commandMessage());
        CHECK(!controller.mergetoolWaiting() && controller.parentStagingPath().isEmpty());
        seen.clear();
        const auto id = select(controller, "tracked.txt");
        CHECK(controller.mergetoolWaiting());
        CHECK(!seen.isEmpty() && seen.last().waiting);
        seen.clear();
        select(controller, "excluded.txt");
        CHECK(!seen.isEmpty() && !seen.last().waiting);
        select(controller, "tracked.txt");
        CHECK(controller.markResolved(id, revisionOf(opened(controller, "tracked.txt")), true));
        waitFinished(launcher, 0, controller);
        pump([&] { return controller.parentStagingPath() == QStringLiteral("tracked.txt"); },
             controller.commandMessage());
        CHECK(!controller.mergetoolWaiting());
        seen.clear();
        CHECK(controller.selectFile(other));
        CHECK(!seen.isEmpty() && seen.last().staging.isEmpty());
        seen.clear();
        select(controller, "tracked.txt");
        CHECK(!seen.isEmpty() && seen.last().staging == QStringLiteral("tracked.txt"));
        CHECK(unmerged(repo, "tracked.txt") && unmerged(repo, "excluded.txt"));
    });
    suite.run("TRUCE-0065 a session that listens after the GUI start is attached within the deadline", [] {
        Repository repo("attached after the spawn\n", "excluded stays conflicted\n");
        ScopedEnvironment environment(repo.environment);
        ApplicationController controller;
        SpawnedLauncher launcher(repo);
        launcher.start();
        pump([&] { return launcher.spawned() || launcher.process.state() == QProcess::NotRunning; },
             QStringLiteral("the launcher did not start a session"));
        CHECK(launcher.spawned());
        const auto launched = launcher.checkSpawnArguments();
        // Retries that fail at once would have given up long before this.
        QElapsedTimer pause;
        pause.start();
        while (pause.elapsed() < 1500) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        CHECK(launcher.process.state() == QProcess::Running);
        openController(controller, repo, launched);
        // The session opens on the launched path, not on the first file in the list.
        CHECK(controller.files()->data(controller.files()->index(0), SessionModel::FileIdRole).toString() != launched);
        CHECK(controller.selectedFileId() == launched);
        pump([&] { return controller.mergetoolWaitingPath() == "tracked.txt"; }, controller.commandMessage());
        CHECK(launcher.clock.elapsed() < 75 * 200);
        CHECK(controller.mergetoolWaiting());
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
        CHECK(launcher.spawned());
        launcher.checkSpawnArguments();
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
    suite.run("TRUCE-0072 the socket directory ignores TMPDIR and falls back from an unusable XDG_RUNTIME_DIR", [] {
        const auto fallback = QStringLiteral("/tmp/branch-truce-") + QString::number(::geteuid());
        git::RepositoryContext repository;
        repository.operation = git::RepositoryOperation::Merge;
        repository.supported = true;
        repository.worktreeRoot = QStringLiteral("/srv/worktree");
        repository.stage2.commitId = QByteArray(40, 'a');
        repository.stage3.commitId = QByteArray(40, 'b');
        const auto socketFor = [&](const std::optional<QString> &runtime, const QString &temporary) {
            QProcessEnvironment environment;
            environment.insert("TMPDIR", temporary);
            if (runtime) environment.insert("XDG_RUNTIME_DIR", *runtime);
            ScopedEnvironment scoped(environment);
            CHECK(QDir::tempPath() == temporary);
            return std::pair{mergetoolSocketDirectory(), mergetoolServerName(repository)};
        };
        const auto longTemporary = QStringLiteral("/tmp/") + QString(90, QLatin1Char('t'));
        const auto [shortDirectory, shortSocket] = socketFor(QStringLiteral("/run/user/7/"), longTemporary);
        CHECK(shortDirectory == QStringLiteral("/run/user/7/branch-truce"));
        CHECK(socketFor(QStringLiteral("/run/user/7"), QStringLiteral("/other")).second == shortSocket);
        CHECK(shortSocket.startsWith(shortDirectory + QStringLiteral("/bt-mt-")));
        CHECK(shortSocket.size() == shortDirectory.size() + 1 + 6 + 32);
        // The longest runtime directory whose socket path stays within Qt's 106 bytes.
        const auto boundary = QStringLiteral("/r") + QString(52, QLatin1Char('x'));
        const auto [fitDirectory, fitSocket] = socketFor(boundary, longTemporary);
        CHECK(fitDirectory == boundary + QStringLiteral("/branch-truce"));
        CHECK(QFile::encodeName(fitSocket).size() == 106);
        CHECK(socketFor(boundary + QLatin1Char('x'), longTemporary).first == fallback);
        CHECK(socketFor(std::nullopt, longTemporary).first == fallback);
        CHECK(socketFor(QString(), longTemporary).first == fallback);
        CHECK(socketFor(QStringLiteral("run/user/7"), longTemporary).first == fallback);
        CHECK(QFile::encodeName(socketFor(std::nullopt, longTemporary).second).size() <= 106);
    });
    suite.run("TRUCE-0072 a GUI and launcher with different long TMPDIR values attach for the same worktree", [] {
        Repository repo("attached across TMPDIR values\n", "excluded stays conflicted\n");
        // A runtime directory at the boundary length gives a 106-byte socket path.
        QTemporaryDir runtimeRoot(QStringLiteral("/tmp/bt-XXXXXX"));
        CHECK(runtimeRoot.isValid());
        const auto runtimePrefix = runtimeRoot.path() + QStringLiteral("/r");
        const auto runtime = privateDirectory(runtimePrefix + QString(54 - runtimePrefix.size(), QLatin1Char('x')));
        CHECK(runtime.size() == 54);
        QTemporaryDir temporaries;
        CHECK(temporaries.isValid());
        const auto guiTemporary = temporaries.path() + QStringLiteral("/gui-") + QString(60, QLatin1Char('g'));
        const auto launcherTemporary = temporaries.path() + QStringLiteral("/launcher-") + QString(60, QLatin1Char('l'));
        CHECK(QDir().mkpath(guiTemporary) && QDir().mkpath(launcherTemporary));
        repo.environment.insert("XDG_RUNTIME_DIR", runtime);
        auto guiEnvironment = repo.environment;
        guiEnvironment.insert("TMPDIR", guiTemporary);
        ScopedEnvironment environment(guiEnvironment);
        CHECK(QDir::tempPath() == guiTemporary);
        ApplicationController controller;
        openController(controller, repo);
        const auto sockets = QDir(runtime + QStringLiteral("/branch-truce"))
            .entryInfoList({QStringLiteral("bt-mt-*")}, QDir::System);
        CHECK(sockets.size() == 1 && QFile::encodeName(sockets.front().filePath()).size() == 106);
        CHECK(QDir(guiTemporary).entryList({QStringLiteral("bt-mt-*")}, QDir::System).isEmpty());
        QTemporaryDir sides;
        auto launcherEnvironment = directLauncherEnvironment(repo, sides);
        launcherEnvironment.insert("TMPDIR", launcherTemporary);
        QProcess launcher;
        launcher.setProcessEnvironment(launcherEnvironment);
        launcher.setWorkingDirectory(repo.root);
        launcher.start(launcherBinary, {"--mergetool"});
        CHECK(launcher.waitForStarted(5000));
        pump([&] { return controller.mergetoolWaitingPath() == "tracked.txt"; },
             controller.commandMessage() + " | " + QString::fromLocal8Bit(launcher.readAllStandardError()));
        select(controller, "tracked.txt");
        CHECK(controller.leaveMergetool());
        waitFinished(launcher, 1, controller);
        CHECK(readFile(repo.path("tracked.txt")) == repo.tracked);
        CHECK(unmerged(repo, "tracked.txt") && unmerged(repo, "excluded.txt"));
    });
    suite.run("TRUCE-0072 a socket that cannot listen is reported and the launcher exits non-zero", [] {
        Repository repo("nobody can attach\n", "excluded stays conflicted\n");
        // Short enough for the socket path, so the file below blocks the directory the session uses.
        QTemporaryDir runtime(QStringLiteral("/tmp/bt-run-XXXXXX"));
        CHECK(runtime.isValid());
        // A regular file where the socket directory belongs makes listening impossible.
        writeFile(runtime.filePath(QStringLiteral("branch-truce")), "not a directory\n");
        repo.environment.insert("XDG_RUNTIME_DIR", runtime.path());
        ScopedEnvironment environment(repo.environment);
        ApplicationController controller;
        startController(controller, repo);
        CHECK(controller.startupState() == QStringLiteral("ready-without-mergetool"));
        CHECK(controller.detail().startsWith(QStringLiteral("Mergetool attach is unavailable: The mergetool socket directory ")
                                             + runtime.path() + QStringLiteral("/branch-truce is not a directory.")));
        CHECK(!controller.mergetoolListening());
        CHECK(controller.files()->totalFiles() == 2);
        QTemporaryDir sides;
        QProcess launcher;
        launcher.setProcessEnvironment(directLauncherEnvironment(repo, sides));
        launcher.setWorkingDirectory(repo.root);
        launcher.start(launcherBinary, {"--mergetool"});
        CHECK(launcher.waitForStarted(5000));
        waitFinished(launcher, 1, controller);
        // Qt may warn about the C locale first.
        const QByteArray refusal("mergetool: No Branch Truce session is listening for this repository.\n");
        const auto errors = launcher.readAllStandardError();
        CHECK(errors == refusal || errors.endsWith('\n' + refusal));
        CHECK(readFile(repo.path("tracked.txt")) == repo.tracked);
        CHECK(unmerged(repo, "tracked.txt"));
    });
    suite.run("TRUCE-0072 a session socket answered by another uid is refused before the launcher sends anything", [] {
        Repository repo("never attached\n", "excluded stays conflicted\n");
        ScopedEnvironment environment(repo.environment);
        ApplicationController controller;
        openController(controller, repo);
        QTemporaryDir sides;
        QProcess launcher;
        launcher.setProcessEnvironment(directLauncherEnvironment(repo, sides));
        launcher.setWorkingDirectory(repo.root);
        launcher.start(QCoreApplication::applicationFilePath(), {"--foreign-peer-launcher"});
        CHECK(launcher.waitForStarted(5000));
        waitFinished(launcher, 1, controller);
        // Qt may warn about the C locale first.
        const QByteArray refusal("mergetool: The session socket does not belong to this user.\n");
        const auto errors = launcher.readAllStandardError();
        if (errors != refusal && !errors.endsWith('\n' + refusal))
            throw std::runtime_error("launcher stderr: " + errors.toStdString());
        // Let the session handle whatever reached its socket before the launcher closed it.
        QElapsedTimer settle;
        settle.start();
        while (settle.elapsed() < 300) QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
        CHECK(controller.mergetoolListening() && controller.mergetoolIntakes().isEmpty());
        CHECK(controller.mergetoolWaitingPath().isEmpty() && !controller.mergetoolHasWaiter("tracked.txt"));
        CHECK(readFile(repo.path("tracked.txt")) == repo.tracked);
        CHECK(unmerged(repo, "tracked.txt"));
    });
    return suite.report();
}
