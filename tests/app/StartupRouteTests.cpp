#include "TestHarness.h"
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

namespace {
void writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size());
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly));
    const auto bytes = file.readAll();
    CHECK(file.error() == QFileDevice::NoError);
    return bytes;
}

QByteArray quoteShell(QByteArray value)
{
    value.replace("'", "'\\''");
    return '\'' + value + '\'';
}

class Repository {
public:
    explicit Repository(bool excludedAttributes = false)
    {
        CHECK(directory.isValid() && executables.isValid());
        root = QFileInfo(directory.path()).canonicalFilePath();
        environment.insert("PATH", "/usr/bin:/bin");
        // App caches and user configuration must stay outside the launched worktree.
        environment.insert("HOME", executables.path());
        environment.insert("XDG_CONFIG_HOME", executables.path() + "/config");
        environment.insert("XDG_CACHE_HOME", executables.path() + "/cache");
        environment.insert("XDG_DATA_HOME", executables.path() + "/data");
        environment.insert("LC_ALL", "C");
        environment.insert("GIT_CONFIG_NOSYSTEM", "1");
        environment.insert("GIT_TERMINAL_PROMPT", "0");
        environment.insert("QT_QPA_PLATFORM", "offscreen");
        environment.insert("QT_QUICK_BACKEND", "software");
        git({"init", "--quiet", "--initial-branch=main"});
        git({"config", "user.name", "Branch Truce Tests"});
        git({"config", "user.email", "tests@example.invalid"});
        writeFile(path("tracked.txt"), "base\n");
        writeFile(path("excluded.txt"), "base\n");
        if (excludedAttributes) {
            writeFile(path(".gitattributes"), "excluded.txt -text eol=crlf\n");
            git({"add", "--", ".gitattributes"});
        }
        git({"add", "--", "tracked.txt", "excluded.txt"});
        git({"commit", "--quiet", "-m", "base"});
    }
    QString path(const QString &relative) const { return root + '/' + relative; }
    QByteArray git(const QStringList &arguments, int expected = 0) const
    {
        QProcess process;
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(root);
        process.start("/usr/bin/git", QStringList{"--no-pager", "--literal-pathspecs"} + arguments);
        CHECK(process.waitForStarted(5000) && process.waitForFinished(5000));
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != expected)
            throw std::runtime_error("fixture git: " + process.readAllStandardError().toStdString());
        return process.readAllStandardOutput();
    }
    void conflict(const QString &operation)
    {
        git({"checkout", "--quiet", "-b", "side"});
        for (const auto name : {"tracked.txt", "excluded.txt"}) writeFile(path(name), "side\n");
        git({"commit", "--quiet", "-am", "side"});
        side = git({"rev-parse", "HEAD"}).trimmed();
        git({"checkout", "--quiet", "main"});
        for (const auto name : {"tracked.txt", "excluded.txt"}) writeFile(path(name), "main\n");
        git({"commit", "--quiet", "-am", "main"});
        main = git({"rev-parse", "HEAD"}).trimmed();
        if (operation.startsWith("rebase")) {
            git({"checkout", "--quiet", "side"});
            git({"rebase", operation == "rebase-apply" ? "--apply" : "--merge", "main"}, 1);
        } else if (operation == "cherry-pick") {
            git({"cherry-pick", "side"}, 1);
        } else {
            git({"merge", "--no-edit", "side"}, 1);
        }
        writeFile(path("tracked.txt"), manual);
    }
    QMap<QString, QByteArray> snapshot() const
    {
        QMap<QString, QByteArray> result;
        // Includes the index, operation metadata, refs, objects, and untracked files.
        QDirIterator files(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const auto selected = files.next();
            result.insert(QDir(root).relativeFilePath(selected), readFile(selected));
        }
        return result;
    }
    void offlineGuard()
    {
        git({"config", "remote.origin.url", "https://offline.invalid/never-contact"});
        git({"config", "branch.main.remote", "origin"});
        const auto log = quoteShell(QFile::encodeName(executables.path() + "/git-argv"));
        const auto marker = quoteShell(QFile::encodeName(executables.path() + "/network-attempt"));
        const auto script = executables.path() + "/git";
        writeFile(script, "#!/bin/sh\nfor argument in \"$@\"; do\n"
            "  case \"$argument\" in fetch|pull|push|clone|ls-remote|remote|submodule) : > " + marker + "; exit 97 ;; esac\n"
            "done\nprintf '%s\\n' \"$@\" >> " + log + "\nexec /usr/bin/git \"$@\"\n");
        CHECK(QFile::setPermissions(script, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        environment.insert("PATH", executables.path() + ":/usr/bin:/bin");
    }
    QJsonObject launch(const QString &application, const QStringList &arguments, const QString &cwd = {}) const
    {
        const auto before = snapshot();
        QProcess process;
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(cwd.isEmpty() ? root : cwd);
        process.start(application, QStringList{"--smoke-test"} + arguments);
        CHECK(process.waitForStarted(5000));
        if (!process.waitForFinished(20000)) {
            process.kill();
            process.waitForFinished(5000);
            throw std::runtime_error("startup timed out: " + process.readAllStandardError().toStdString());
        }
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
            throw std::runtime_error("startup: " + process.readAllStandardError().toStdString());
        const auto output = process.readAllStandardOutput();
        QByteArray record;
        int records = 0;
        for (const auto &line : output.split('\n')) {
            if (line.startsWith("BRANCH_TRUCE_SESSION=")) { record = line.mid(21); ++records; }
        }
        CHECK(records == 1);
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(record, &error);
        CHECK(error.error == QJsonParseError::NoError && document.isObject());
        CHECK(snapshot() == before);
        return document.object();
    }
    QTemporaryDir directory;
    QTemporaryDir executables;
    QString root;
    QProcessEnvironment environment;
    QByteArray main;
    QByteArray side;
    const QByteArray manual = "manual UTF-8 \xc3\xa9 \xf0\x9f\x98\x80\n";
};

QJsonObject file(const QJsonObject &state, const QString &name)
{
    const auto files = state.value("files").toArray();
    for (const auto &value : files) if (value.toObject().value("path").toString() == name) return value.toObject();
    throw std::runtime_error("missing startup file: " + name.toStdString());
}

void ready(const Repository &repo, const QJsonObject &state, const QString &operation)
{
    CHECK(state.value("state") == "ready" && state.value("operation") == operation);
    CHECK(state.value("repository").toString() == repo.root && state.value("files").toArray().size() == 2);
    CHECK(state.value("resolved").toInt() == 0 && !state.value("workflow").toString().isEmpty());
    CHECK(file(state, "tracked.txt").value("support") == "supported");
    CHECK(file(state, "tracked.txt").value("text").toString().toUtf8() == repo.manual);
    CHECK(file(state, "tracked.txt").value("detail").toString().isEmpty());
    CHECK(state.value("stage2").toString().contains(QString::fromLatin1(repo.main)));
    CHECK(state.value("stage3").toString().contains(QString::fromLatin1(repo.side)));
}
} // namespace

class StartupRouteTests {
public:
    int run(const QString &application)
    {
        test_support::Suite suite("StartupRouteTests");
        suite.run("merge startup accepts an explicit root a subdirectory and the current directory", [&] {
            Repository repo;
            repo.conflict("merge");
            CHECK(QDir(repo.root).mkdir("nested"));
            for (const auto &arguments : QList<QStringList>{{repo.root}, {repo.path("nested")}, {}}) {
                const auto state = repo.launch(application, arguments, repo.path("nested"));
                ready(repo, state, "Merge");
                CHECK(state.value("stage2").toString().startsWith("Current checkout:"));
                CHECK(state.value("stage3").toString().startsWith("Merged commit:"));
                CHECK(file(state, "excluded.txt").value("support") == "supported");
            }
        });
        suite.run("both rebase startup routes preserve the result and truthful source identities", [&] {
            for (const auto operation : {"rebase-merge", "rebase-apply"}) {
                Repository repo;
                repo.conflict(operation);
                const auto state = repo.launch(application, {repo.root});
                ready(repo, state, "Rebase");
                CHECK(state.value("stage2").toString().startsWith("Current rebased state:"));
                CHECK(state.value("stage3").toString().startsWith("Commit being replayed:"));
            }
        });
        suite.run("unsupported cherry-pick keeps the inventory outside app without side labels", [&] {
            Repository repo;
            repo.conflict("cherry-pick");
            const auto state = repo.launch(application, {repo.root});
            CHECK(state.value("state") == "unsupported" && state.value("operation") == "Unsupported operation");
            CHECK(!state.value("detail").toString().isEmpty() && state.value("files").toArray().size() == 2);
            CHECK(state.value("stage2").toString().isEmpty() && state.value("stage3").toString().isEmpty());
            CHECK(state.value("resolved").toInt() == 0);
            for (const auto &value : state.value("files").toArray()) {
                CHECK(value.toObject().value("support") == "outside-app");
                CHECK(!value.toObject().value("detail").toString().isEmpty());
            }
        });
        suite.run("offline startup uses only local Git inspection with an unreachable remote", [&] {
            Repository repo;
            repo.conflict("merge");
            repo.offlineGuard();
            ready(repo, repo.launch(application, {repo.root}), "Merge");
            CHECK(!QFile::exists(repo.executables.path() + "/network-attempt"));
            const auto trace = readFile(repo.executables.path() + "/git-argv");
            CHECK(trace.contains("rev-parse\n") && trace.contains("cat-file\n"));
        });
        suite.run("excluded attributes retain the classifier reason beside an accepted manual UTF-8 result", [&] {
            Repository repo(true);
            repo.conflict("merge");
            const auto state = repo.launch(application, {repo.root});
            ready(repo, state, "Merge");
            const auto excluded = file(state, "excluded.txt");
            CHECK(excluded.value("support") == "outside-app" && excluded.value("text").toString().isEmpty());
            CHECK(excluded.value("detail").toString()
                  == "Outside app: stage 1 cannot be edited: Outside app: unsupported or contradictory text/eol/ident attributes");
            CHECK(state.value("detail").toString().contains("outside app remain outstanding"));
        });
        suite.run("a repository without an operation reports no session and no source identities", [&] {
            Repository repo;
            const auto state = repo.launch(application, {repo.root});
            CHECK(state.value("state") == "no-session" && state.value("files").toArray().isEmpty());
            CHECK(!state.value("detail").toString().isEmpty() && state.value("operation").toString().isEmpty());
            CHECK(state.value("stage2").toString().isEmpty() && state.value("stage3").toString().isEmpty());
        });
        suite.run("invalid selections expose an error without an empty supported session", [&] {
            Repository repo;
            for (const auto &selection : {repo.executables.path(), repo.path("missing")}) {
                const auto state = repo.launch(application, {selection});
                CHECK(state.value("state") == "error" && state.value("files").toArray().isEmpty());
                CHECK(!state.value("detail").toString().isEmpty());
                CHECK(state.value("stage2").toString().isEmpty() && state.value("stage3").toString().isEmpty());
            }
        });
        return suite.report();
    }
};

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (application.arguments().size() != 2) return 2;
    return StartupRouteTests{}.run(application.arguments()[1]);
}
