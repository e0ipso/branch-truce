#include "app/ApplicationController.h"
#include "fixtures/GitOnPath.h"
#include "app/MergetoolLauncher.h"
#include "app/MergetoolProtocol.h"
#include "app/RecoveryRestore.h"
#include "fixtures/mergetool/GitGate.h"
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
#include <QScopeGuard>
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
    CHECK(controller.loader()->openRepository(repo.root, preferredFileId));
    // Opening takes about 0.5 s on an idle host. At load averages of 25 to 36 on 8 cores it
    // averaged 2.5 s and some opens took over 10 s.
    pump([&] { return !controller.loader()->opening(); }, QStringLiteral("the repository opening did not finish"));
    CHECK(!controller.loader()->opening());
}

void openController(ApplicationController &controller, const Repository &repo, const QString &preferredFileId = {})
{
    startController(controller, repo, preferredFileId);
    if (controller.loader()->startupState() != "ready")
        throw std::runtime_error("controller startup: " + controller.loader()->detail().toStdString());
    CHECK(controller.mergetool()->listening());
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

QString recoveryError(ApplicationController &controller, const QByteArray &path)
{
    auto *files = controller.files();
    for (int row = 0; row < files->totalFiles(); ++row) {
        const auto index = files->index(row);
        if (files->data(index, SessionModel::FileIdRole).toString() == QString::fromLatin1(path.toHex()))
            return files->data(index, SessionModel::RecoveryErrorRole).toString();
    }
    throw std::runtime_error("no file row for " + path.toStdString());
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

// Replaces a file by a same-byte copy with a new inode and timestamps, as
// installed Git does with MERGED before it runs the tool.
void copyInPlace(const QString &target)
{
    CHECK(QFile::rename(target, target + ".moved") && QFile::copy(target + ".moved", target));
    CHECK(QFile::remove(target + ".moved"));
}

// A window captures tracked.txt and records an edit; then Git's copy replaces
// tracked.txt, so the next window restores a copied launch path.
void storeBeforeCopiedLaunch(const Repository &repo)
{
    {
        ApplicationController before(gitOnPath());
        openController(before, repo);
        CHECK(before.files()->editResult(QByteArray("tracked.txt").toHex().toStdString(),
            opened(before, "tracked.txt").current().id, {{0, 0}, "recovered edit\n"}) == Transition::Applied);
    }
    copyInPlace(repo.path("tracked.txt"));
}

// Starts a launcher on tracked.txt after the window loaded and waits for its attach.
void attachLauncher(QProcess &launcher, const Repository &repo, const QTemporaryDir &sides,
    ApplicationController &controller)
{
    launcher.setProcessEnvironment(directLauncherEnvironment(repo, sides));
    launcher.setWorkingDirectory(repo.root);
    launcher.start(launcherBinary, {"--mergetool"});
    CHECK(launcher.waitForStarted(5000));
    pump([&] { return controller.mergetool()->waitingPath() == "tracked.txt"; }, controller.commandMessage());
}

// Runs the event loop for a fixed time and checks that `holds` stays true throughout.
void holdFor(int milliseconds, const std::function<bool()> &holds, const QString &detail)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
        if (!holds()) throw std::runtime_error(detail.toStdString());
    }
}

// The restored draft stays read-only, Mark resolved is refused and nothing is
// written over the current bytes. Leave fails the launcher, which puts back
// the MERGED bytes it saw when it attached.
void checkCopiedLaunchRefused(ApplicationController &controller, QProcess &launcher, const QString &target,
    const QByteArray &current, const QByteArray &attached)
{
    const auto fileId = QString::fromLatin1(QByteArray("tracked.txt").toHex());
    select(controller, "tracked.txt");
    const auto revision = revisionOf(opened(controller, "tracked.txt"));
    CHECK(controller.files()->hasRecoveryFailure(fileId.toStdString()));
    CHECK(!controller.editResult(fileId, revision, 0, 0, QStringLiteral("edited after the attach\n")));
    CHECK(!controller.markResolved(fileId, revision, true));
    CHECK(launcher.state() == QProcess::Running);
    CHECK(readFile(target) == current);
    CHECK(controller.leaveMergetool());
    waitFinished(launcher, 1, controller);
    CHECK(readFile(target) == attached);
    CHECK(controller.files()->hasRecoveryFailure(fileId.toStdString()));
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
    CHECK(controller.mergetool()->waitingPath() == "tracked.txt");
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
// Nothing sits at "<path> (deleted)", so every spawn test also covers a binary
// replaced after the launcher started.
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
        // As in a package upgrade, the running image keeps its inode and its path
        // now holds another program. Linux then names the image "<path> (deleted)",
        // and the launcher has to spawn the stub at the original path.
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
        return runMergetoolLauncher(2, launcherArguments, gitOnPath());
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
        ApplicationController controller(gitOnPath());
        openController(controller, repo);
        auto parent = startMergetool(repo, {"tracked.txt"});
        pump([&] { return controller.mergetool()->waitingPath() == "tracked.txt"; }, controller.commandMessage());
        CHECK(controller.mergetool()->intakes().size() == 1);
        const auto firstIntake = controller.mergetool()->intakes().front();
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
        CHECK(controller.mergetool()->intakes().size() == 1);
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
        pump([&] { return controller.mergetool()->waitingPath() == "excluded.txt"; }, controller.commandMessage());
        CHECK(controller.mergetool()->intakes().size() == 2);
        CHECK(readFile(firstIntake + "/merged") == firstMerged);
        CHECK(controller.mergetool()->intakes().at(1) != firstIntake);
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
        ApplicationController controller(gitOnPath());
        openController(controller, repo);
        auto parent = startMergetool(repo, {"tracked.txt"});
        pump([&] { return controller.mergetool()->waitingPath() == "tracked.txt"; }, controller.commandMessage());
        const auto intake = controller.mergetool()->intakes().front();
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
        ApplicationController controller(gitOnPath());
        openController(controller, repo);
        auto parent = startMergetool(repo, {});
        pump([&] {
            const auto path = controller.mergetool()->waitingPath();
            return path == "tracked.txt" || path == "excluded.txt";
        }, controller.commandMessage());
        const auto first = controller.mergetool()->waitingPath();
        const auto second = first == "tracked.txt" ? QByteArray("excluded.txt") : QByteArray("tracked.txt");
        const auto firstText = first == "tracked.txt" ? repo.tracked : repo.excluded;
        const auto secondText = second == "tracked.txt" ? repo.tracked : repo.excluded;
        const auto other = select(controller, second);
        CHECK(!controller.markResolved(other, revisionOf(opened(controller, second)), true));
        CHECK(controller.commandMessage().contains(QStringLiteral("Waiting for Git to launch this path")));
        CHECK(unmerged(repo, first) && unmerged(repo, second));
        CHECK(parent->state() == QProcess::Running);
        CHECK(controller.mergetool()->waitingPath() == first);
        const auto firstId = select(controller, first);
        CHECK(controller.saveDraft(firstId, revisionOf(opened(controller, first))));
        CHECK(readFile(repo.path(QString::fromLatin1(first))) == firstText);
        CHECK(controller.markResolved(firstId, revisionOf(opened(controller, first)), true));
        pump([&] {
            return opened(controller, first).facts().resolved && controller.mergetool()->waitingPath() == second
                && parent->state() == QProcess::Running;
        }, controller.commandMessage() + " | waiting=" + QString::fromLatin1(controller.mergetool()->waitingPath()));
        CHECK(controller.mergetool()->intakes().size() == 2);
        CHECK(readFile(controller.mergetool()->intakes().front() + "/merged") == firstText);
        CHECK(readFile(controller.mergetool()->intakes().at(1) + "/merged") == secondText);
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
        ApplicationController controller(gitOnPath());
        openController(controller, repo);
        // QML reads both properties again only when their notify signal fires.
        const auto *meta = controller.metaObject();
        for (const auto *name : {"mergetoolWaiting", "parentStagingPath"}) {
            CHECK(meta->property(meta->indexOfProperty(name)).notifySignal()
                  == QMetaMethod::fromSignal(&ApplicationController::mergetoolChanged));
        }
        struct Observed {
            bool waiting = false;
            QString staging;
        };
        QList<Observed> seen;
        QObject::connect(&controller, &ApplicationController::mergetoolChanged, &controller, [&] {
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
        pump([&] { return controller.mergetool()->waitingPath() == "tracked.txt"; }, controller.commandMessage());
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
    // The reload replaces the publisher's files, which used to drop the parent's
    // staging assignment while the waiter stayed assigned. Mark resolved then
    // staged the path in the app and the launcher never got a decision.
    suite.run("TRUCE-0100 a recovery choice made while a launcher waits hands the launched path back to it", [] {
        Repository repo("launch bytes\n", "excluded stays conflicted\n");
        ScopedEnvironment environment(repo.environment);
        QString chosen;
        {
            ApplicationController one(gitOnPath());
            openController(one, repo);
            ApplicationController two(gitOnPath());
            startController(two, repo);
            for (auto *controller : {&one, &two}) {
                const auto revision = opened(*controller, "tracked.txt").current().id;
                CHECK(controller->files()->editResult(QByteArray("tracked.txt").toHex().toStdString(), revision,
                    {{0, 0}, controller == &one ? "chosen\n" : "other\n"}) == Transition::Applied);
            }
            chosen = QFileInfo(one.files()->recoveryDirectory()).fileName();
        }
        ApplicationController controller(gitOnPath());
        openController(controller, repo);
        CHECK(controller.loader()->recoveryChoices().size() == 2);
        // A direct launcher has no Git parent, so MERGED keeps the identity the stores captured.
        QTemporaryDir sides;
        QProcess launcher;
        launcher.setProcessEnvironment(directLauncherEnvironment(repo, sides));
        launcher.setWorkingDirectory(repo.root);
        launcher.start(launcherBinary, {"--mergetool"});
        CHECK(launcher.waitForStarted(5000));
        pump([&] { return controller.mergetool()->waitingPath() == "tracked.txt"; }, controller.commandMessage());
        CHECK(controller.loader()->chooseRecoverySession(chosen));
        pump([&] { return !controller.loader()->opening(); }, controller.loader()->detail());
        CHECK(controller.loader()->startupState() == "ready" && controller.loader()->recoveryChoices().isEmpty());
        CHECK(controller.mergetool()->waitingPath() == "tracked.txt");
        const auto id = select(controller, "tracked.txt");
        CHECK(controller.mergetoolWaiting());
        const auto expected = opened(controller, "tracked.txt").current().draft.text;
        CHECK(expected.starts_with("chosen\n"));
        CHECK(controller.markResolved(id, revisionOf(opened(controller, "tracked.txt")), true));
        waitFinished(launcher, 0, controller);
        pump([&] { return controller.parentStagingPath() == QStringLiteral("tracked.txt"); },
             controller.commandMessage());
        const auto offer = controller.parentStagingOffer(id);
        CHECK(offer && offer->path == "tracked.txt");
        CHECK(readFile(repo.path("tracked.txt")) == QByteArray::fromStdString(expected));
        // Only the parent stages the path, and the app waits for that acknowledgment.
        CHECK(unmerged(repo, "tracked.txt") && unmerged(repo, "excluded.txt"));
        CHECK(!opened(controller, "tracked.txt").facts().resolved);
    });
    // TRUCE-0102: a store captured tracked.txt before Git replaced it by a copy
    // with the same bytes, and the window restored it before the launcher
    // attached. The attach used to recheck only the file, so staging the path or
    // changing core.autocrlf in between still cleared the recovery error.
    suite.run("TRUCE-0102 a late launcher clears a copied launch path's recovery error only while its index and conversion policy match", [] {
        for (const auto &change : {QStringLiteral("none"), QStringLiteral("staged"), QStringLiteral("autocrlf")}) {
            Repository repo("launch bytes\n", "excluded stays conflicted\n");
            ScopedEnvironment environment(repo.environment);
            const auto fileId = QString::fromLatin1(QByteArray("tracked.txt").toHex());
            {
                ApplicationController before(gitOnPath());
                openController(before, repo);
                CHECK(before.files()->editResult(fileId.toStdString(), opened(before, "tracked.txt").current().id,
                    {{0, 0}, "recovered edit\n"}) == Transition::Applied);
            }
            const auto target = repo.path("tracked.txt");
            CHECK(QFile::rename(target, target + ".moved") && QFile::copy(target + ".moved", target));
            CHECK(QFile::remove(target + ".moved"));
            ApplicationController controller(gitOnPath());
            openController(controller, repo);
            CHECK(recoveryError(controller, "tracked.txt") == copiedLaunchFailure());
            if (change == QStringLiteral("staged")) repo.git({"add", "--", "tracked.txt"});
            if (change == QStringLiteral("autocrlf")) repo.git({"config", "core.autocrlf", "true"});
            QTemporaryDir sides;
            QProcess launcher;
            launcher.setProcessEnvironment(directLauncherEnvironment(repo, sides));
            launcher.setWorkingDirectory(repo.root);
            launcher.start(launcherBinary, {"--mergetool"});
            CHECK(launcher.waitForStarted(5000));
            pump([&] { return controller.mergetool()->waitingPath() == "tracked.txt"; }, controller.commandMessage());
            pump([&] { return recoveryError(controller, "tracked.txt") != copiedLaunchFailure(); },
                 change + QStringLiteral(": the attach did not settle the copied launch"));
            select(controller, "tracked.txt");
            const auto revision = revisionOf(opened(controller, "tracked.txt"));
            if (change == QStringLiteral("none")) {
                CHECK(!controller.files()->hasRecoveryFailure(fileId.toStdString()));
                CHECK(controller.editResult(fileId, revision, 0, 0, QStringLiteral("reviewed after the attach\n")));
                const auto expected = opened(controller, "tracked.txt").current().draft.text;
                CHECK(controller.markResolved(fileId, revisionOf(opened(controller, "tracked.txt")), true));
                waitFinished(launcher, 0, controller);
                CHECK(readFile(target) == QByteArray::fromStdString(expected));
                CHECK(unmerged(repo, "tracked.txt"));
                continue;
            }
            CHECK(controller.files()->hasRecoveryFailure(fileId.toStdString()));
            CHECK(!controller.editResult(fileId, revision, 0, 0, QStringLiteral("edited after the attach\n")));
            CHECK(!controller.markResolved(fileId, revision, true));
            CHECK(readFile(target) == "launch bytes\n");
            CHECK(controller.leaveMergetool());
            waitFinished(launcher, 1, controller);
            CHECK(readFile(target) == "launch bytes\n");
            CHECK(controller.files()->hasRecoveryFailure(fileId.toStdString()));
        }
    });
    // TRUCE-0101: a launcher that finds no window starts one on the path Git
    // launched. That window restores through the same restoreRecovery call and
    // must reach the same adoption when the waiting launcher attaches.
    suite.run("TRUCE-0101 a launcher-started window restores a copied launch path and Mark resolved releases the launcher", [] {
        Repository repo("launch bytes\n", "excluded stays conflicted\n");
        ScopedEnvironment environment(repo.environment);
        storeBeforeCopiedLaunch(repo);
        const auto target = repo.path("tracked.txt");
        ApplicationController controller(gitOnPath());
        SpawnedLauncher launcher(repo);
        launcher.start();
        pump([&] { return launcher.spawned() || launcher.process.state() == QProcess::NotRunning; },
             QStringLiteral("the launcher did not start a session"));
        CHECK(launcher.spawned());
        const auto launched = launcher.checkSpawnArguments();
        CHECK(launcher.process.state() == QProcess::Running);
        openController(controller, repo, launched);
        CHECK(controller.selectedFileId() == launched);
        CHECK(opened(controller, "tracked.txt").current().draft.text.starts_with("recovered edit\n"));
        pump([&] { return controller.mergetool()->waitingPath() == "tracked.txt"; }, controller.commandMessage());
        pump([&] { return !controller.files()->hasRecoveryFailure(launched.toStdString()); },
             QStringLiteral("the launcher-started window kept the copied launch error: ")
                 + recoveryError(controller, "tracked.txt"));
        CHECK(controller.editResult(launched, revisionOf(opened(controller, "tracked.txt")), 0, 0,
            QStringLiteral("reviewed in the started window\n")));
        const auto expected = opened(controller, "tracked.txt").current().draft.text;
        CHECK(expected.starts_with("reviewed in the started window\nrecovered edit\n"));
        CHECK(controller.markResolved(launched, revisionOf(opened(controller, "tracked.txt")), true));
        launcher.finish(0);
        CHECK(readFile(target) == QByteArray::fromStdString(expected));
        CHECK(unmerged(repo, "tracked.txt") && unmerged(repo, "excluded.txt"));
    });
    // TRUCE-0101: the load validated Git's copy of tracked.txt. Before the
    // launcher attached, the file changed again: a new same-byte copy, which
    // still matches the launcher's MERGED digest, or other bytes, which do not.
    // Neither file is the one validation saw, so the adoption refuses.
    suite.run("TRUCE-0101 a copied launch path that changed after recovery validation keeps its recovery error on attach", [] {
        for (const auto &change : {QStringLiteral("copied"), QStringLiteral("edited")}) {
            Repository repo("launch bytes\n", "excluded stays conflicted\n");
            ScopedEnvironment environment(repo.environment);
            storeBeforeCopiedLaunch(repo);
            const auto target = repo.path("tracked.txt");
            ApplicationController controller(gitOnPath());
            openController(controller, repo);
            CHECK(recoveryError(controller, "tracked.txt") == copiedLaunchFailure());
            if (change == QStringLiteral("copied")) copyInPlace(target);
            else writeFile(target, "written by another editor\n");
            const auto expected = readFile(target);
            QTemporaryDir sides;
            QProcess launcher;
            attachLauncher(launcher, repo, sides, controller);
            if (change == QStringLiteral("copied")) {
                pump([&] { return recoveryError(controller, "tracked.txt") != copiedLaunchFailure(); },
                     change + QStringLiteral(": the attach did not settle the copied launch"));
                CHECK(recoveryError(controller, "tracked.txt").contains(QStringLiteral("changed after recovery validation")));
            } else {
                // The launcher's MERGED digest is not the launch bytes, so nothing is checked.
                holdFor(800, [&] { return recoveryError(controller, "tracked.txt") == copiedLaunchFailure(); },
                        change + QStringLiteral(": the recovery error changed"));
            }
            checkCopiedLaunchRefused(controller, launcher, target, expected, expected);
        }
    });
    // TRUCE-0102: an outside write lands while Git inspects the index for the
    // late attach's revalidation, after its first file check passed. The
    // revalidation's final file check, or the adoption after it, sees the
    // write, so the error stays. "none" holds Git the same way and changes
    // nothing, so the wrapper itself does not fail the check.
    suite.run("TRUCE-0102 an outside write while the late attach revalidates keeps the recovery error", [] {
        for (const auto &change : {QStringLiteral("none"), QStringLiteral("edited"), QStringLiteral("copied")}) {
            Repository repo("launch bytes\n", "excluded stays conflicted\n");
            ScopedEnvironment environment(repo.environment);
            storeBeforeCopiedLaunch(repo);
            const auto target = repo.path("tracked.txt");
            // Open until close, so the load runs Git through it unchanged.
            mergetool_fixture::GitGate gate("tracked.txt");
            ApplicationController controller(gate.executable());
            // Runs before the controller goes, so its tasks never wait on a held call.
            const auto releaseGate = qScopeGuard([&gate] { gate.open(); gate.releaseAll(); });
            openController(controller, repo);
            CHECK(recoveryError(controller, "tracked.txt") == copiedLaunchFailure());
            gate.close(QStringLiteral("ls-files"));
            QTemporaryDir sides;
            QProcess launcher;
            attachLauncher(launcher, repo, sides, controller);
            pump([&] { return !gate.held().isEmpty(); }, change + QStringLiteral(": the revalidation did not inspect the index"));
            const auto held = gate.held();
            CHECK(held.size() == 1);
            CHECK(gate.arguments(held.front()).contains("ls-files --stage -z -- tracked.txt"));
            gate.open();
            CHECK(recoveryError(controller, "tracked.txt") == copiedLaunchFailure());
            if (change == QStringLiteral("edited")) writeFile(target, "written by another editor\n");
            if (change == QStringLiteral("copied")) copyInPlace(target);
            const auto outside = readFile(target);
            gate.release(held.front());
            pump([&] { return recoveryError(controller, "tracked.txt") != copiedLaunchFailure(); },
                 change + QStringLiteral(": the revalidation did not settle the copied launch"));
            if (change == QStringLiteral("none")) {
                CHECK(!controller.files()->hasRecoveryFailure(QByteArray("tracked.txt").toHex().toStdString()));
                select(controller, "tracked.txt");
                CHECK(controller.leaveMergetool());
                waitFinished(launcher, 1, controller);
                CHECK(readFile(target) == "launch bytes\n");
                continue;
            }
            CHECK(recoveryError(controller, "tracked.txt").contains(QStringLiteral("index or conversion policy")));
            checkCopiedLaunchRefused(controller, launcher, target, outside, "launch bytes\n");
        }
    });
    suite.run("TRUCE-0065 a session that listens after the GUI start is attached within the deadline", [] {
        Repository repo("attached after the spawn\n", "excluded stays conflicted\n");
        ScopedEnvironment environment(repo.environment);
        ApplicationController controller(gitOnPath());
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
        pump([&] { return controller.mergetool()->waitingPath() == "tracked.txt"; }, controller.commandMessage());
        CHECK(launcher.clock.elapsed() < 75 * 200);
        CHECK(controller.mergetoolWaiting());
        CHECK(controller.mergetool()->intakes().size() == 1);
        CHECK(readFile(controller.mergetool()->intakes().front() + "/merged") == repo.tracked);
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
        ApplicationController controller(gitOnPath());
        openController(controller, repo);
        // A direct launcher has no Git parent whose backup move would hide a leftover tail.
        QTemporaryDir sides;
        QProcess launcher;
        launcher.setProcessEnvironment(directLauncherEnvironment(repo, sides));
        launcher.setWorkingDirectory(repo.root);
        launcher.start(launcherBinary, {"--mergetool"});
        CHECK(launcher.waitForStarted(5000));
        pump([&] { return controller.mergetool()->waitingPath() == "tracked.txt"; }, controller.commandMessage());
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
        ApplicationController controller(gitOnPath());
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
        pump([&] { return controller.mergetool()->waitingPath() == "tracked.txt"; },
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
        ApplicationController controller(gitOnPath());
        startController(controller, repo);
        CHECK(controller.loader()->startupState() == QStringLiteral("ready-without-mergetool"));
        CHECK(controller.loader()->detail().startsWith(QStringLiteral("Mergetool attach is unavailable: The mergetool socket directory ")
                                             + runtime.path() + QStringLiteral("/branch-truce is not a directory.")));
        CHECK(!controller.mergetool()->listening());
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
        ApplicationController controller(gitOnPath());
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
        CHECK(controller.mergetool()->listening() && controller.mergetool()->intakes().isEmpty());
        CHECK(controller.mergetool()->waitingPath().isEmpty() && !controller.mergetool()->hasWaiter("tracked.txt"));
        CHECK(readFile(repo.path("tracked.txt")) == repo.tracked);
        CHECK(unmerged(repo, "tracked.txt"));
    });
    return suite.report();
}
