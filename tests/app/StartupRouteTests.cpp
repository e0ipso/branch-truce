#include "app/ApplicationController.h"
#include "TestHarness.h"
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTimer>
#include <array>
#include <sys/stat.h>

using namespace branch_truce;

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
        git({"add", "--", "tracked.txt", "excluded.txt"});
        git({"commit", "--quiet", "-m", "side"});
        side = git({"rev-parse", "HEAD"}).trimmed();
        git({"checkout", "--quiet", "main"});
        for (const auto name : {"tracked.txt", "excluded.txt"}) writeFile(path(name), "main\n");
        git({"add", "--", "tracked.txt", "excluded.txt"});
        git({"commit", "--quiet", "-m", "main"});
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

class ScopedEnvironment {
public:
    explicit ScopedEnvironment(const QProcessEnvironment &environment)
    {
        for (const auto name : {"HOME", "PATH", "LC_ALL", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
                                "XDG_DATA_HOME", "GIT_CONFIG_NOSYSTEM", "GIT_TERMINAL_PROMPT"}) {
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
    timeout.start(10000);
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

SnapshotFileIdentity identity(const QString &path)
{
    struct stat observed {};
    CHECK(::lstat(QFile::encodeName(path).constData(), &observed) == 0);
    return {static_cast<std::uint64_t>(observed.st_dev), static_cast<std::uint64_t>(observed.st_ino),
        static_cast<std::uint32_t>(observed.st_mode), static_cast<std::int64_t>(observed.st_size),
        observed.st_mtim.tv_sec, observed.st_mtim.tv_nsec, observed.st_ctim.tv_sec, observed.st_ctim.tv_nsec};
}

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
        suite.run("merge and rebase captures retain verified sources provenance and the manual launch result", [] {
            for (const auto operation : {"merge", "rebase-merge", "rebase-apply"}) {
                Repository repo;
                repo.conflict(operation);
                const auto launchIdentity = identity(repo.path("tracked.txt"));
                ScopedEnvironment environment(repo.environment);
                ApplicationController controller;
                openController(controller, repo);
                const auto &selected = openedFile(controller);
                CHECK(selected.support() == Support::Supported && selected.snapshots());
                const auto expected = *selected.snapshots();
                CHECK(expected.path == "tracked.txt" && expected.resultMode == "100644");
                CHECK(expected.launchResult.text.utf8 == repo.manual.toStdString());
                CHECK(expected.launchResult.text.bytes == repo.manual.toStdString());
                CHECK(selected.current().draft.text == expected.launchResult.text.utf8);
                CHECK(expected.launchResult.identity == launchIdentity);
                CHECK(expected.launchResult.text.format == SnapshotFormat{SnapshotFormat::Encoding::Utf8,
                    false, SnapshotFormat::Newline::Lf, true});
                const bool rebase = QString::fromLatin1(operation).startsWith("rebase");
                const auto &provenance = expected.provenance;
                CHECK(!provenance.sessionId.empty());
                CHECK(provenance.worktreeRoot == repo.root.toStdString());
                CHECK(provenance.gitDirectory == QFileInfo(repo.path(".git")).canonicalFilePath().toStdString());
                CHECK(provenance.operation == (rebase ? SnapshotProvenance::Operation::Rebase
                                                      : SnapshotProvenance::Operation::Merge));
                CHECK(provenance.stage2CommitId == repo.main.toStdString());
                CHECK(provenance.stage3CommitId == repo.side.toStdString());
                CHECK(provenance.baseCommitId == repo.git({"rev-parse", QString::fromLatin1(repo.side) + '^'}).trimmed().toStdString());
                if (rebase) {
                    CHECK(provenance.headName == "refs/heads/side");
                    CHECK(provenance.originalHead == repo.side.toStdString());
                    CHECK(provenance.onto == repo.main.toStdString());
                }
                CHECK(expected.earlier && expected.stage2 && expected.stage3);
                CHECK(expected.earlier->commitId == provenance.baseCommitId);
                CHECK(expected.stage2->commitId == provenance.stage2CommitId);
                CHECK(expected.stage3->commitId == provenance.stage3CommitId);
                CHECK(expected.earlier->label == "Common earlier version");
                CHECK(expected.stage2->label.starts_with(rebase ? "Current rebased state:" : "Current checkout:"));
                CHECK(expected.stage3->label.starts_with(rebase ? "Commit being replayed:" : "Merged commit:"));
                const std::array<const SourceSnapshot *, 3> sources{
                    &*expected.earlier, &*expected.stage2, &*expected.stage3};
                for (std::size_t i = 0; i < sources.size(); ++i) {
                    const auto &source = *sources[i];
                    const auto object = repo.git({"rev-parse", ':' + QString::number(static_cast<unsigned int>(i + 1))
                        + ":tracked.txt"}).trimmed();
                    CHECK(source.objectId == object.toStdString() && source.mode == "100644");
                    CHECK(source.rawBytes == repo.git({"cat-file", "blob", QString::fromLatin1(object)}).toStdString());
                    CHECK(source.worktree.bytes == source.rawBytes && source.worktree.utf8 == source.rawBytes);
                    CHECK(source.worktree.format == SnapshotFormat{SnapshotFormat::Encoding::Utf8,
                        false, SnapshotFormat::Newline::Lf, true});
                }
                const auto &other = openedFile(controller, "excluded.txt");
                CHECK(other.snapshots() && other.snapshots()->provenance == provenance);
                auto *model = controller.files();
                auto edited = selected.current().draft;
                edited.text = "later manual edit\n";
                CHECK(model->replaceDraft(selected.id(), selected.current().id, edited) == Transition::Applied);
                CHECK(*selected.snapshots() == expected);
                CHECK(model->undo(selected.id(), selected.current().id) == Transition::Applied);
                CHECK(selected.current().draft.text == repo.manual.toStdString());
                CHECK(model->redo(selected.id(), selected.current().id) == Transition::Applied);
                writeFile(repo.path("tracked.txt"), "external replacement\n");
                repo.git({"add", "--", "tracked.txt"});
                writeFile(repo.path(".gitattributes"), "tracked.txt -text\n");
                CHECK(*selected.snapshots() == expected);
            }
        });
        suite.run("converted source and manual result captures retain independent bytes and format metadata", [] {
            struct FormatCase {
                QByteArray attributes;
                QByteArray earlier;
                QByteArray main;
                QByteArray side;
                QByteArray launch;
                SnapshotFormat::Encoding encoding;
                bool sourceBom;
                bool launchBom;
            };
            const FormatCase cases[]{
                {"text eol=crlf", "base\r\n", "main\r\n", "side\r\n", "\xef\xbb\xbfmanual \xc3\xa9\r\nkept",
                    SnapshotFormat::Encoding::Utf8, false, true},
                {"text eol=crlf working-tree-encoding=UTF-16",
                    QByteArray::fromHex("fffe62006100730065000d000a00"),
                    QByteArray::fromHex("fffe6d00610069006e000d000a00"),
                    QByteArray::fromHex("fffe73006900640065000d000a00"),
                    QByteArray::fromHex("fffe6d0061006e00750061006c002000e9000d000a006b00650070007400"),
                    SnapshotFormat::Encoding::Utf16LittleEndian, true, true},
                {"text eol=crlf working-tree-encoding=UTF-16BE",
                    QByteArray::fromHex("0062006100730065000d000a"),
                    QByteArray::fromHex("006d00610069006e000d000a"),
                    QByteArray::fromHex("0073006900640065000d000a"),
                    QByteArray::fromHex("006d0061006e00750061006c002000e9000d000a006b006500700074"),
                    SnapshotFormat::Encoding::Utf16BigEndian, false, false}
            };
            for (const auto &format : cases) {
                Repository repo;
                repo.git({"config", "core.autocrlf", "false"});
                repo.conflict("merge");
                writeFile(repo.path(".gitattributes"), "tracked.txt " + format.attributes + '\n');
                writeFile(repo.path("tracked.txt"), format.launch);
                const auto launchIdentity = identity(repo.path("tracked.txt"));
                ScopedEnvironment environment(repo.environment);
                ApplicationController controller;
                openController(controller, repo);
                const auto &selected = openedFile(controller);
                CHECK(selected.support() == Support::Supported && selected.snapshots());
                const auto captured = *selected.snapshots();
                CHECK(captured.earlier && captured.stage2 && captured.stage3);
                const std::array<const SourceSnapshot *, 3> sources{
                    &*captured.earlier, &*captured.stage2, &*captured.stage3};
                const std::array<QByteArray, 3> checkout{format.earlier, format.main, format.side};
                const std::array<std::string, 3> canonical{"base\n", "main\n", "side\n"};
                for (std::size_t i = 0; i < sources.size(); ++i) {
                    CHECK(sources[i]->rawBytes == canonical[i]);
                    CHECK(sources[i]->worktree.utf8 == canonical[i]);
                    CHECK(sources[i]->worktree.bytes == checkout[i].toStdString());
                    CHECK(sources[i]->worktree.bytes != sources[i]->rawBytes);
                    CHECK(sources[i]->worktree.format == SnapshotFormat{format.encoding,
                        format.sourceBom, SnapshotFormat::Newline::CrLf, true});
                }
                CHECK(selected.current().draft.text == "manual \xc3\xa9\nkept");
                CHECK(captured.launchResult.text.utf8 == selected.current().draft.text);
                CHECK(captured.launchResult.text.bytes == format.launch.toStdString());
                CHECK(captured.launchResult.text.format == SnapshotFormat{format.encoding,
                    format.launchBom, SnapshotFormat::Newline::CrLf, false});
                CHECK(captured.launchResult.identity == launchIdentity);
                CHECK(captured.conversion.attributes.at("text") == "set");
                CHECK(captured.conversion.attributes.at("eol") == "crlf");
                CHECK(captured.conversion.configuration.at("core.autocrlf") == "false");
                if (format.encoding == SnapshotFormat::Encoding::Utf8) {
                    CHECK(!captured.conversion.attributes.contains("working-tree-encoding"));
                } else {
                    CHECK(captured.conversion.attributes.at("working-tree-encoding")
                        == (format.encoding == SnapshotFormat::Encoding::Utf16LittleEndian ? "UTF-16" : "UTF-16BE"));
                }
                auto edited = selected.current().draft;
                edited.text += " later";
                CHECK(controller.files()->replaceDraft(selected.id(), selected.current().id, edited) == Transition::Applied);
                CHECK(*selected.snapshots() == captured);
            }
        });
        suite.run("add-add has an unavailable earlier source while an empty base remains captured", [] {
            for (const bool missing : {false, true}) {
                Repository repo;
                if (missing) repo.git({"rm", "--", "tracked.txt"});
                else {
                    writeFile(repo.path("tracked.txt"), {});
                    repo.git({"add", "--", "tracked.txt"});
                }
                repo.git({"commit", "--quiet", "-m", "prepare earlier source"});
                repo.conflict("merge");
                ScopedEnvironment environment(repo.environment);
                ApplicationController controller;
                openController(controller, repo);
                const auto &selected = openedFile(controller);
                CHECK(selected.support() == Support::Supported && selected.snapshots());
                const auto expected = *selected.snapshots();
                CHECK(expected.earlier.has_value() == !missing);
                CHECK(expected.stage2 && expected.stage3);
                CHECK(expected.stage2->rawBytes == "main\n" && expected.stage3->rawBytes == "side\n");
                CHECK(selected.current().draft.text == repo.manual.toStdString());
                if (!missing) {
                    CHECK(expected.earlier->rawBytes.empty());
                    CHECK(expected.earlier->worktree.bytes.empty() && expected.earlier->worktree.utf8.empty());
                    CHECK(!expected.earlier->objectId.empty() && expected.earlier->commitId.has_value());
                    CHECK(!expected.earlier->worktree.format.finalNewline);
                }
                auto edited = selected.current().draft;
                edited.text = "edited after capture\n";
                auto *model = controller.files();
                CHECK(model->replaceDraft(selected.id(), selected.current().id, edited) == Transition::Applied);
                CHECK(model->undo(selected.id(), selected.current().id) == Transition::Applied);
                CHECK(model->redo(selected.id(), selected.current().id) == Transition::Applied);
                CHECK(*selected.snapshots() == expected);
            }
        });
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
