#include "app/ApplicationController.h"
#include "TestHarness.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

#include <functional>
#include <optional>

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

void rejectDuplicate(const Repository &repo, ApplicationController &controller, QProcess &parent)
{
    QTemporaryDir sides;
    CHECK(sides.isValid());
    writeFile(sides.filePath("base"), "base\n");
    writeFile(sides.filePath("local"), "local\n");
    writeFile(sides.filePath("remote"), "remote\n");
    auto environment = repo.environment;
    environment.insert("MERGED", "tracked.txt");
    environment.insert("BASE", sides.filePath("base"));
    environment.insert("LOCAL", sides.filePath("local"));
    environment.insert("REMOTE", sides.filePath("remote"));
    QProcess duplicate;
    duplicate.setProcessEnvironment(environment);
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
    return suite.report();
}
