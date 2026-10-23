#include "GitAdapter.h"
#include "TestHarness.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTimer>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

using namespace branch_truce::git;

namespace {
constexpr auto gitExecutable = "/usr/bin/git";

QList<QByteArray> oddPaths()
{
    return {":leading.txt", ":(glob)*", ":!tracked.txt", "[ab].txt", "--all", "-",
            " space name\twith\nnewline ", "quote\"and\\backslash", QByteArray::fromHex("636166c3a92e747874"),
            QByteArray("odd-") + char(0xff) + ".txt", QByteArray("odd-") + char(0xfe) + ".txt",
            QByteArray("nested/odd-") + char(0xff) + ".txt"};
}

void writeFile(const QString &path, const QByteArray &bytes, bool executable = false)
{
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly));
    CHECK(file.write(bytes) == bytes.size());
    file.close();
    if (executable) CHECK(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
}

class Repository {
public:
    Repository()
    {
        CHECK(root.isValid() && executables.isValid());
        git({"init", "--quiet", "--initial-branch=main"});
        writeFile(path("tracked.txt"), "before\n");
        git({"add", "--", "tracked.txt"});
    }
    ~Repository()
    {
        // QTemporaryDir's QString traversal cannot remove non-UTF-8 names.
        for (const auto &file : rawFiles) ::unlink(file.constData());
    }
    void writePath(const QByteArray &relative, const QByteArray &bytes)
    {
        const auto file = QFile::encodeName(root.path()) + '/' + relative;
        const int fd = ::open(file.constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        CHECK(fd >= 0);
        rawFiles.append(file);
        const auto count = ::write(fd, bytes.constData(), static_cast<std::size_t>(bytes.size()));
        const int closed = ::close(fd);
        CHECK(count == bytes.size() && closed == 0);
    }
    QString path(const QString &relative) const { return root.path() + '/' + relative; }
    QByteArray git(const QStringList &arguments, const QByteArray &input = {}, int expectedExit = 0) const
    {
        QProcess process;
        QProcessEnvironment env;
        env.insert("PATH", "/usr/bin:/bin");
        env.insert("LC_ALL", "C");
        // Fixture setup ignores the user's Git configuration and cannot prompt.
        env.insert("HOME", root.path());
        env.insert("XDG_CONFIG_HOME", root.path());
        env.insert("GIT_CONFIG_NOSYSTEM", "1");
        env.insert("GIT_TERMINAL_PROMPT", "0");
        process.setProcessEnvironment(env);
        process.setWorkingDirectory(root.path());
        auto argv = QStringList{"--no-pager", "--literal-pathspecs"};
        argv.append(arguments);
        process.start(gitExecutable, argv);
        CHECK(process.waitForStarted(5000));
        CHECK(process.write(input) == input.size());
        process.closeWriteChannel();
        CHECK(process.waitForFinished(5000));
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != expectedExit) {
            throw std::runtime_error("fixture git: " + process.readAllStandardError().toStdString());
        }
        return process.readAllStandardOutput();
    }
    Request request(Operation operation, const QByteArray &file = {}) const
    {
        return {operation, root.path(), file, {}, 71, 93};
    }
    QString executable(const QByteArray &script) const
    {
        const auto selected = executables.path() + "/git-fixture";
        writeFile(selected, script, true);
        return selected;
    }
    // Scripted process outcomes exercise adapter boundaries. Literal path/index
    // correctness is covered separately against real Git in this temporary repo.
    QString fakeGit(const QByteArray &operation,
                    const QByteArray &inspection = "printf '100644 bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb 0\\ttracked.txt\\0'\n") const
    {
        QByteArray script = R"SH(#!/bin/sh
printf '%s\0' "$@" >> .adapter-argv
while [ "$#" -gt 0 ]; do
    case "$1" in
        --no-pager|--literal-pathspecs) shift ;;
        -c|-C) shift 2 ;;
        *) break ;;
    esac
done
case "$1" in
    --version) printf 'git version fixture\n' ;;
    config) exit 0 ;;
    rev-parse)
        case "$*" in
            *--git-path*objects*) printf '%s/.git/objects\n' "$PWD" ;;
            *--git-path*hooks*) printf '%s/.git/hooks\n' "$PWD" ;;
            *--show-toplevel*) printf '%s\n' "$PWD" ;;
            *) printf '%s/.git\n' "$PWD" ;;
        esac ;;
    check-attr) printf 'tracked.txt\0filter\0unspecified\0' ;;
    ls-files)
        case "$*" in *--cached*) printf 'tracked.txt\0'; exit 0 ;; esac
        if [ -e .after-add ]; then
)SH";
        script += inspection;
        script += R"SH(
        else
            printf '100644 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 0\ttracked.txt\0'
        fi ;;
    add|status|cat-file)
)SH";
        script += operation;
        script += "\n;;\n*) exit 97 ;;\nesac\n";
        return executable(script);
    }
    QTemporaryDir root{QStringLiteral("/tmp/truce-git-repository-XXXXXX")};
    QTemporaryDir executables{QStringLiteral("/tmp/truce-git-executable-XXXXXX")};
    QList<QByteArray> rawFiles;
};

class Completion {
public:
    explicit Completion(GitAdapter &adapter) : adapter_(adapter)
    {
        connection_ = QObject::connect(&adapter, &GitAdapter::finished, &loop_, [this](const Result &result) {
            results.push_back(result);
            loop_.quit();
        });
    }
    Result wait(int deadlineMs = 5000)
    {
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &loop_, &QEventLoop::quit);
        deadline.start(deadlineMs);
        if (results.empty()) loop_.exec();
        CHECK(results.size() == 1);
        CHECK(!adapter_.busy());
        QCoreApplication::processEvents();
        CHECK(results.size() == 1);
        return results.front();
    }
    Result run(const Request &request, int deadlineMs = 5000)
    {
        CHECK(adapter_.start(request));
        CHECK(adapter_.busy() && results.empty()); // Completion must be asynchronous.
        CHECK(!adapter_.start(request)); // A busy refusal must not emit another result.
        const auto result = wait(deadlineMs);
        CHECK(result.request.operation == request.operation && result.request.directory == request.directory);
        CHECK(result.request.path == request.path && result.request.objectId == request.objectId);
        CHECK(result.request.sessionGeneration == 71 && result.request.inputRevision == 93);
        return result;
    }
    std::vector<Result> results;
private:
    GitAdapter &adapter_;
    QEventLoop loop_;
    QMetaObject::Connection connection_;
};

Result run(GitAdapter &adapter, const Request &request)
{
    Completion completion(adapter);
    return completion.run(request);
}

class EnvironmentValue {
public:
    EnvironmentValue(QByteArray name, const QByteArray &value)
        : name_(std::move(name)), previous_(qgetenv(name_.constData())), set_(qEnvironmentVariableIsSet(name_.constData()))
    { CHECK(qputenv(name_.constData(), value)); }
    ~EnvironmentValue()
    {
        if (set_) qputenv(name_.constData(), previous_);
        else qunsetenv(name_.constData());
    }
private:
    QByteArray name_;
    QByteArray previous_;
    bool set_;
};

void registerMergetool(Repository &repo, const QString &scenario)
{
    const auto base = repo.git({"hash-object", "-w", "--stdin"}, "base\n").trimmed();
    const auto ours = repo.git({"hash-object", "-w", "--stdin"}, "ours\n").trimmed();
    const auto theirs = repo.git({"hash-object", "-w", "--stdin"}, "theirs\n").trimmed();
    const auto nul = QByteArray(1, '\0');
    repo.git({"update-index", "-z", "--index-info"},
        "0 " + QByteArray(40, '0') + "\ttracked.txt" + nul
        + "100644 " + base + " 1\ttracked.txt" + nul
        + "100644 " + ours + " 2\ttracked.txt" + nul
        + "100644 " + theirs + " 3\ttracked.txt" + nul);
    writeFile(repo.path("tracked.txt"), "<<<<<<< ours\nours\n=======\ntheirs\n>>>>>>> theirs\n");
    auto executable = QCoreApplication::applicationFilePath();
    executable.replace('\'', "'\\''");
    const auto command = '\'' + executable + "' --mergetool-fixture " + scenario + " \"$MERGED\"";
    repo.git({"config", "mergetool.truce-coverage.cmd", command});
    repo.git({"config", "mergetool.truce-coverage.trustExitCode", "true"});
    repo.git({"config", "mergetool.keepBackup", "false"});
}

// This is a fixture launcher for the real Git parent protocol. It exercises
// the adapter's release preflight; it does not implement the GUI launcher.
int mergetoolFixture(const QString &scenario, const QString &merged)
{
    const auto configure = [](const QString &key, const QString &value) {
        QProcess process;
        process.start(gitExecutable, {"config", key, value});
        CHECK(process.waitForStarted(5000) && process.waitForFinished(5000));
        CHECK(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0);
    };
    // Install hazards after the parent materializes its temporary sources.
    // The app cannot prevent helpers a separately invoked parent ran earlier.
    if (scenario == "filter") {
        writeFile(".gitattributes", "/tracked.txt filter=handoff\n");
        configure("filter.handoff.clean", "touch .handoff-helper-ran; cat");
    } else if (scenario == "hook" || scenario == "custom-hook") {
        const auto hooks = scenario == "hook" ? QString(".git/hooks") : QString("custom hooks");
        CHECK(QDir().mkpath(hooks));
        configure("core.hooksPath", hooks);
        writeFile(hooks + "/post-index-change", "#!/bin/sh\ntouch .handoff-helper-ran\n", true);
    } else if (scenario == "monitor") {
        configure("core.fsmonitor", "touch .handoff-helper-ran");
    } else if (scenario == "promisor") {
        configure("remote.origin.promisor", "true");
    }
    GitAdapter adapter(gitExecutable);
    const auto preflight = run(adapter, {Operation::CheckParentStaging, QDir::currentPath(),
                                       QFile::encodeName(merged), {}, 71, 93});
    writeFile(".handoff-preflight", preflight.diagnostic.toUtf8());
    CHECK(preflight.mutation == Mutation::NotAttempted);
    if (preflight.outcome != Outcome::Success) {
        writeFile(".handoff-rejected", "blocked\n");
        return 1;
    }
    writeFile(".handoff-released", "preflight passed\n");
    writeFile(merged, scenario == "crlf" ? "resolved\r\n" : "resolved\n");
    return 0;
}
}

class GitAdapterTests {
public:
    int runTests()
    {
        test_support::Suite suite("GitAdapterTests");
        suite.run("named reads use real Git and preserve request identity", [] {
            Repository repo;
            GitAdapter adapter(gitExecutable);
            for (const auto operation : {Operation::DiscoverRepository, Operation::ListUnmerged, Operation::Status,
                                         Operation::ReadConfiguration, Operation::InspectIndex, Operation::ReadAttributes}) {
                const bool needsPath = operation == Operation::InspectIndex || operation == Operation::ReadAttributes;
                const auto result = run(adapter, repo.request(operation, needsPath ? "tracked.txt" : ""));
                CHECK(result.outcome == Outcome::Success && result.exitCode == 0);
                CHECK(result.mutation == Mutation::NotAttempted && !result.outputIncomplete);
                if (operation == Operation::DiscoverRepository) {
                    CHECK(result.standardOutput.contains(repo.root.path().toUtf8()));
                    CHECK(result.repository.worktreeRoot == repo.root.path());
                    CHECK(result.repository.gitDirectory == repo.path(".git"));
                    CHECK(result.repository.operation == RepositoryOperation::None && !result.repository.supported);
                    CHECK(!result.repository.diagnostic.isEmpty() && result.indexEntries.isEmpty());
                }
                if (operation == Operation::ListUnmerged) CHECK(result.standardOutput.isEmpty());
                if (operation == Operation::InspectIndex) CHECK(result.standardOutput.endsWith(QByteArray("\ttracked.txt\0", 13)));
            }
            const auto object = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
            for (const auto operation : {Operation::ReadObject, Operation::MaterializeSource}) {
                auto request = repo.request(operation, operation == Operation::MaterializeSource ? "tracked.txt" : "");
                request.objectId = object;
                const auto result = run(adapter, request);
                CHECK(result.outcome == Outcome::Success && result.standardOutput == "before\n");
                CHECK(result.mutation == Mutation::NotAttempted);
            }
        });
        suite.run("unmerged index entries stay NUL-delimited", [] {
            Repository repo;
            CHECK(QDir(repo.root.path()).mkdir("nested"));
            const auto base = repo.git({"hash-object", "-w", "--stdin"}, "base\n").trimmed();
            const auto ours = repo.git({"hash-object", "-w", "--stdin"}, "ours\n").trimmed();
            const auto theirs = repo.git({"hash-object", "-w", "--stdin"}, "theirs\n").trimmed();
            auto names = oddPaths();
            names.prepend("tracked.txt");
            QByteArray entries = "0 0000000000000000000000000000000000000000\ttracked.txt" + QByteArray(1, '\0');
            for (const auto &name : names) {
                entries += "100644 " + base + " 1\t" + name + QByteArray(1, '\0');
                entries += "100644 " + ours + " 2\t" + name + QByteArray(1, '\0');
                entries += "100644 " + theirs + " 3\t" + name + QByteArray(1, '\0');
            }
            repo.git({"update-index", "-z", "--index-info"}, entries);
            GitAdapter adapter(gitExecutable);
            const auto result = run(adapter, repo.request(Operation::ListUnmerged));
            CHECK(result.outcome == Outcome::Success);
            CHECK(result.standardOutput == repo.git({"ls-files", "--unmerged", "-z"}));
            CHECK(result.standardOutput.count('\0') == names.size() * 3);
            CHECK(result.indexEntries.size() == names.size() * 3);
            // Synthetic unmerged stages without an operation must stay visible,
            // but cannot acquire merge/rebase source labels from their position.
            const auto discovery = run(adapter, repo.request(Operation::DiscoverRepository));
            CHECK(discovery.outcome == Outcome::Success && !discovery.repository.supported);
            CHECK(discovery.repository.operation == RepositoryOperation::Unsupported);
            CHECK(discovery.indexEntries.size() == result.indexEntries.size());
            CHECK(discovery.repository.stage2.commitId.isEmpty() && discovery.repository.stage3.commitId.isEmpty());
            CHECK(discovery.repository.diagnostic.contains("no verified merge or rebase"));
            for (const auto &name : names) {
                unsigned int stages = 0;
                for (const auto &entry : result.indexEntries) {
                    if (entry.path != name) continue;
                    CHECK(entry.mode == "100644" && entry.stage >= 1 && entry.stage <= 3);
                    CHECK(entry.objectId == (entry.stage == 1 ? base : entry.stage == 2 ? ours : theirs));
                    stages |= 1u << entry.stage;
                    // Select the enumerated byte path after asking for its label.
                    const auto identity = entry.path;
                    CHECK(!displayPath(entry.path).isEmpty() && entry.path == identity);
                    auto request = repo.request(Operation::MaterializeSource, entry.path);
                    request.objectId = entry.objectId;
                    const auto source = run(adapter, request);
                    CHECK(source.outcome == Outcome::Success);
                    CHECK(source.standardOutput == (entry.stage == 1 ? "base\n" : entry.stage == 2 ? "ours\n" : "theirs\n"));
                }
                CHECK(stages == 14);
                repo.writePath(name, "resolved\n");
                const auto staged = run(adapter, repo.request(Operation::StageFile, name));
                CHECK(staged.outcome == Outcome::Success && staged.mutation == Mutation::ObservedChanged);
                CHECK(staged.indexBefore.count('\0') == 3 && staged.indexAfter.count('\0') == 1);
                CHECK(staged.indexAfter.endsWith('\t' + name + QByteArray(1, '\0')));
            }
            CHECK(repo.git({"ls-files", "--unmerged", "-z"}).isEmpty());
        });
        suite.run("enumerated and staged paths retain special and non-UTF-8 bytes", [] {
            Repository repo;
            CHECK(QDir(repo.root.path()).mkdir("nested"));
            GitAdapter adapter(gitExecutable);
            const auto names = oddPaths();
            writeFile(repo.path("a.txt"), "must remain untracked\n");
            writeFile(repo.path("b.txt"), "must remain untracked\n");
            QByteArray rules = "* truce-path\n";
            for (const auto &name : names) {
                if (QString::fromUtf8(name).toUtf8() != name) rules += name + " text eol=crlf\n";
            }
            writeFile(repo.path(".gitattributes"), rules);
            const auto untouched = repo.git({"ls-files", "--stage", "-z", "--", "tracked.txt"});
            for (const auto &name : names) {
                repo.writePath(name, "literal content\n");
                const auto decoy = QString::fromUtf8(name).toUtf8();
                if (decoy != name) repo.writePath(decoy, "display decoy must remain untracked\n");
                const auto identity = name;
                CHECK(!displayPath(name).isEmpty() && name == identity);
                const auto result = run(adapter, repo.request(Operation::StageFile, name));
                CHECK(result.outcome == Outcome::Success && result.exitCode == 0);
                CHECK(result.mutation == Mutation::ObservedChanged && result.indexBefore.isEmpty());
                const auto inspected = run(adapter, repo.request(Operation::InspectIndex, name));
                CHECK(inspected.outcome == Outcome::Success && inspected.standardOutput == result.indexAfter);
                CHECK(inspected.indexEntries.size() == 1 && inspected.indexEntries.front().path == name);
                const auto &entry = inspected.indexEntries.front();
                CHECK(entry.stage == 0 && entry.mode == "100644");
                CHECK(result.indexAfter.endsWith('\t' + name + QByteArray(1, '\0')));
                CHECK(repo.git({"cat-file", "blob", QString::fromLatin1(entry.objectId)}) == "literal content\n");
                const auto attributes = run(adapter, repo.request(Operation::ReadAttributes, name));
                CHECK(attributes.outcome == Outcome::Success);
                CHECK(attributes.standardOutput.contains(name + QByteArray("\0truce-path\0set\0", 16)));
                auto request = repo.request(Operation::MaterializeSource, name);
                request.objectId = entry.objectId;
                const auto source = run(adapter, request);
                CHECK(source.outcome == Outcome::Success);
                CHECK(source.standardOutput == (decoy == name ? "literal content\n" : "literal content\r\n"));
                const auto status = run(adapter, repo.request(Operation::Status));
                CHECK(status.outcome == Outcome::Success && status.standardOutput.contains(name + QByteArray(1, '\0')));
                const auto all = repo.git({"ls-files", "--stage", "-z"});
                CHECK(all.contains(result.indexAfter));
                if (decoy != name) CHECK(!all.contains('\t' + decoy + QByteArray(1, '\0')));
            }
            CHECK(displayPath(QByteArray("odd-") + char(0xff) + ".txt") == "odd-\\xff.txt");
            CHECK(displayPath("odd-\\xff.txt") == "odd-\\\\xff.txt");
            CHECK(displayPath("a\tb\nc") == "a\\tb\\nc");
            CHECK(displayPath(QByteArray::fromHex("636166c3a92e747874")) == QString::fromUtf8("caf\xc3\xa9.txt"));
            CHECK(repo.git({"ls-files", "--stage", "-z", "--", "a.txt", "b.txt"}).isEmpty());
            CHECK(repo.git({"ls-files", "--stage", "-z", "--", "tracked.txt"}) == untouched);
            const auto unchanged = run(adapter, repo.request(Operation::StageFile, ":leading.txt"));
            CHECK(unchanged.outcome == Outcome::Success && unchanged.mutation == Mutation::ObservedUnchanged);
        });
        suite.run("invalid operands cannot reach Git or mutate the fixture", [] {
            Repository repo;
            GitAdapter adapter(gitExecutable);
            const auto before = repo.git({"ls-files", "--stage", "-z"});
            const QList<QByteArray> paths{"", "/absolute", "../escape", "dir/../file", "./tracked.txt", "dir//file", "dir/",
                                          QByteArray("a\0b", 3)};
            for (const auto &path : paths) {
                const auto result = run(adapter, repo.request(Operation::StageFile, path));
                CHECK(result.outcome == Outcome::Rejected && result.mutation == Mutation::NotAttempted);
                CHECK(result.exitCode == -1 && !result.diagnostic.isEmpty());
            }
            auto bad = repo.request(Operation::ReadObject);
            for (const auto &object : {QByteArray("HEAD"), QByteArray(40, 'g'), QByteArray("--help")}) {
                bad.objectId = object;
                CHECK(run(adapter, bad).outcome == Outcome::Rejected);
            }
            bad = repo.request(Operation::Status, "unexpected");
            CHECK(run(adapter, bad).outcome == Outcome::Rejected);
            bad = repo.request(Operation::Status);
            bad.objectId = QByteArray(40, 'a');
            CHECK(run(adapter, bad).outcome == Outcome::Rejected);
            bad = repo.request(static_cast<Operation>(999));
            CHECK(run(adapter, bad).outcome == Outcome::Rejected);
            bad = repo.request(Operation::Status);
            bad.directory = ".";
            CHECK(run(adapter, bad).outcome == Outcome::Rejected);
            CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
        });
        suite.run("symlink traversal and nonregular staging are rejected", [] {
            Repository repo;
            CHECK(QDir(repo.root.path()).mkdir("dir"));
            writeFile(repo.path("dir/file"), "data\n");
            CHECK(QFile::link(repo.path("tracked.txt"), repo.path("link")));
            CHECK(QFile::link(repo.path("dir"), repo.path("linked-dir")));
            GitAdapter adapter(gitExecutable);
            for (const auto &path : {"link", "linked-dir/file", "dir", "missing"}) {
                const auto result = run(adapter, repo.request(Operation::StageFile, path));
                CHECK(result.outcome == Outcome::Rejected && result.mutation == Mutation::NotAttempted);
            }
            const QByteArray raw = QByteArray("link-") + char(0xff);
            const auto link = QFile::encodeName(repo.root.path()) + '/' + raw;
            CHECK(::symlink(QFile::encodeName(repo.path("tracked.txt")).constData(), link.constData()) == 0);
            repo.rawFiles.append(link);
            repo.writePath(QString::fromUtf8(raw).toUtf8(), "display decoy\n");
            const auto before = repo.git({"ls-files", "--stage", "-z"});
            const auto result = run(adapter, repo.request(Operation::StageFile, raw));
            CHECK(result.outcome == Outcome::Rejected && result.mutation == Mutation::NotAttempted);
            CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
        });
        suite.run("executable trust is checked before launch and again after selection", [] {
            Repository repo;
            const auto selected = repo.executable("#!/bin/sh\nprintf 'must not run' > .unsafe-executable\n");
            GitAdapter changed(selected);
            writeFile(selected, "#!/bin/sh\nprintf 'changed executable must not run' > .unsafe-executable\n", true);
            CHECK(run(changed, repo.request(Operation::Status)).outcome == Outcome::Rejected);
            GitAdapter relative("git");
            CHECK(run(relative, repo.request(Operation::Status)).outcome == Outcome::Rejected);
            const auto inside = repo.path("git");
            writeFile(inside, "#!/bin/sh\nprintf unsafe > .unsafe-executable\n", true);
            GitAdapter inRepository(inside);
            CHECK(run(inRepository, repo.request(Operation::Status)).outcome == Outcome::Rejected);
            CHECK(QFile::setPermissions(selected, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                       | QFileDevice::ExeOwner | QFileDevice::WriteGroup));
            GitAdapter writable(selected);
            CHECK(run(writable, repo.request(Operation::Status)).outcome == Outcome::Rejected);
            CHECK(!QFile::exists(repo.path(".unsafe-executable")));
        });
        suite.run("ambient Git and executable search overrides do not escape the fixture", [] {
            Repository repo;
            Repository other;
            const auto otherBefore = other.git({"ls-files", "--stage", "-z"});
            EnvironmentValue gitDir("GIT_DIR", other.path(".git").toUtf8());
            EnvironmentValue workTree("GIT_WORK_TREE", other.root.path().toUtf8());
            EnvironmentValue index("GIT_INDEX_FILE", other.path("redirected-index").toUtf8());
            EnvironmentValue configCount("GIT_CONFIG_COUNT", "1");
            EnvironmentValue configKey("GIT_CONFIG_KEY_0", "core.fsmonitor");
            EnvironmentValue configValue("GIT_CONFIG_VALUE_0", "must-not-run");
            EnvironmentValue path("PATH", repo.root.path().toUtf8());
            GitAdapter adapter(gitExecutable);
            writeFile(repo.path("tracked.txt"), "new\n");
            const auto result = run(adapter, repo.request(Operation::StageFile, "tracked.txt"));
            CHECK(result.outcome == Outcome::Success && result.mutation == Mutation::ObservedChanged);
            CHECK(repo.git({"show", ":tracked.txt"}) == "new\n");
            CHECK(other.git({"ls-files", "--stage", "-z"}) == otherBefore);
            CHECK(!QFile::exists(other.path("redirected-index")));
        });
        suite.run("helper and offline policy precedes affected reads and staging", [] {
            for (const auto &driver : {"Fixture", "set", "unset", "unspecified"}) {
                for (const auto &kind : {"clean", "smudge", "process"}) {
                    Repository repo;
                    const auto key = QByteArray("filter.") + driver + '.' + kind;
                    writeFile(repo.path(".gitattributes"), "tracked.txt filter=" + QByteArray(driver) + '\n');
                    const auto before = repo.git({"ls-files", "--stage", "-z"});
                    const auto blob = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
                    repo.git({"config", QString::fromLatin1(key), "touch .helper-ran; cat"});
                    GitAdapter adapter(gitExecutable);
                    // Policy inspection and raw identity reads never invoke filters.
                    CHECK(run(adapter, repo.request(Operation::ReadAttributes, "tracked.txt")).outcome == Outcome::Success);
                    auto raw = repo.request(Operation::ReadObject);
                    raw.objectId = blob;
                    CHECK(run(adapter, raw).standardOutput == "before\n");
                    for (const auto operation : {Operation::Status, Operation::MaterializeSource,
                                                 Operation::CheckParentStaging, Operation::StageFile}) {
                        auto request = repo.request(operation, operation == Operation::Status ? "" : "tracked.txt");
                        if (operation == Operation::MaterializeSource) request.objectId = blob;
                        const auto result = run(adapter, request);
                        CHECK(result.outcome == Outcome::Rejected && result.mutation == Mutation::NotAttempted);
                        CHECK(result.diagnostic.contains("Outside app") && result.diagnostic.contains("filter"));
                    }
                    CHECK(!QFile::exists(repo.path(".helper-ran")));
                    repo.git({"config", "--unset", QString::fromLatin1(key)});
                    CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
                }
            }
            {
                Repository repo;
                GitAdapter adapter(gitExecutable);
                const auto blob = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
                repo.git({"config", "filter.unused.clean", "touch .helper-ran; cat"});
                repo.git({"config", "filter.unused.smudge", "touch .helper-ran; cat"});
                repo.git({"config", "filter.unspecified.smudge", "touch .helper-ran; cat"});
                writeFile(repo.path(".gitattributes"), "other.txt filter=unused\ntracked.txt text eol=crlf\n");
                auto source = repo.request(Operation::MaterializeSource, "tracked.txt");
                source.objectId = blob;
                CHECK(run(adapter, source).standardOutput == "before\r\n");
                CHECK(run(adapter, repo.request(Operation::Status)).outcome == Outcome::Success);
                CHECK(run(adapter, repo.request(Operation::CheckParentStaging, "tracked.txt")).outcome == Outcome::Success);
                writeFile(repo.path("tracked.txt"), "after\r\n");
                CHECK(run(adapter, repo.request(Operation::StageFile, "tracked.txt")).outcome == Outcome::Success);
                CHECK(repo.git({"show", ":tracked.txt"}) == "after\n");
                // The same adapter must recheck attributes/configuration on every request.
                writeFile(repo.path(".gitattributes"), "/tracked.txt filter=unused\n");
                CHECK(run(adapter, source).outcome == Outcome::Rejected);
                CHECK(run(adapter, repo.request(Operation::StageFile, "tracked.txt")).outcome == Outcome::Rejected);
                CHECK(QDir(repo.root.path()).mkdir("subdirectory"));
                for (const auto operation : {Operation::Status, Operation::MaterializeSource, Operation::StageFile}) {
                    auto request = repo.request(operation, operation == Operation::Status ? "" : "tracked.txt");
                    request.directory = repo.path("subdirectory");
                    if (operation == Operation::MaterializeSource) request.objectId = blob;
                    CHECK(run(adapter, request).outcome == Outcome::Rejected);
                }
                writeFile(repo.path(".gitattributes"), "/other.txt filter=unused\n");
                for (const auto operation : {Operation::Status, Operation::MaterializeSource, Operation::StageFile}) {
                    auto request = repo.request(operation, operation == Operation::Status ? "" : "tracked.txt");
                    request.directory = repo.path("subdirectory");
                    if (operation == Operation::MaterializeSource) request.objectId = blob;
                    CHECK(run(adapter, request).outcome == Outcome::Success);
                }
                CHECK(!QFile::exists(repo.path(".helper-ran")));
            }
            for (const auto &hooksPath : {".git/hooks", "custom hooks"}) {
                Repository repo;
                CHECK(QDir(repo.root.path()).mkpath(hooksPath));
                repo.git({"config", "core.hooksPath", hooksPath});
                const auto blob = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
                const auto before = repo.git({"ls-files", "--stage", "-z"});
                GitAdapter adapter(gitExecutable);
                writeFile(repo.path(QString(hooksPath) + "/pre-commit"), "#!/bin/sh\ntouch .hook-ran\n", true);
                CHECK(run(adapter, repo.request(Operation::StageFile, "tracked.txt")).outcome == Outcome::Success);
                const auto hook = repo.path(QString(hooksPath) + "/post-index-change");
                writeFile(hook, "#!/bin/sh\ntouch .hook-ran\n", true);
                for (const auto operation : {Operation::CheckParentStaging, Operation::StageFile}) {
                    const auto result = run(adapter, repo.request(operation, "tracked.txt"));
                    CHECK(result.outcome == Outcome::Rejected && result.mutation == Mutation::NotAttempted);
                    CHECK(result.diagnostic.contains("post-index-change"));
                }
                auto source = repo.request(Operation::MaterializeSource, "tracked.txt");
                source.objectId = blob;
                CHECK(run(adapter, source).outcome == Outcome::Success);
                CHECK(run(adapter, repo.request(Operation::Status)).outcome == Outcome::Success);
                CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
                CHECK(!QFile::exists(repo.path(".hook-ran")));
                CHECK(QFile::setPermissions(hook, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
                CHECK(run(adapter, repo.request(Operation::CheckParentStaging, "tracked.txt")).outcome == Outcome::Success);
                repo.git({"config", "core.hooksPath", "/dev/null"});
                CHECK(run(adapter, repo.request(Operation::StageFile, "tracked.txt")).outcome == Outcome::Success);
            }
            {
                Repository repo;
                GitAdapter adapter(gitExecutable);
                const auto before = repo.git({"ls-files", "--stage", "-z"});
                repo.git({"config", "core.fsmonitor", "touch .monitor-ran"});
                CHECK(run(adapter, repo.request(Operation::Status)).outcome == Outcome::Success);
                for (const auto operation : {Operation::CheckParentStaging, Operation::StageFile}) {
                    const auto result = run(adapter, repo.request(operation, "tracked.txt"));
                    CHECK(result.outcome == Outcome::Rejected && result.mutation == Mutation::NotAttempted);
                    CHECK(result.diagnostic.contains("core.fsmonitor"));
                }
                CHECK(!QFile::exists(repo.path(".monitor-ran")));
                repo.git({"config", "core.fsmonitor", "false"});
                CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
                CHECK(run(adapter, repo.request(Operation::StageFile, "tracked.txt")).outcome == Outcome::Success);
            }
            for (int scenario = 0; scenario < 4; ++scenario) {
                Repository repo;
                Repository alternate;
                const auto blob = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
                const auto before = repo.git({"ls-files", "--stage", "-z"});
                if (scenario == 0) repo.git({"config", "extensions.partialClone", "origin"});
                if (scenario == 1) repo.git({"config", "remote.origin.promisor", "true"});
                if (scenario == 2) writeFile(repo.path(".git/objects/pack/pack-fixture.promisor"), "");
                if (scenario == 3) {
                    writeFile(alternate.path(".git/objects/pack/pack-fixture.promisor"), "");
                    writeFile(repo.path(".git/objects/info/alternates"), alternate.path(".git/objects").toUtf8() + '\n');
                }
                repo.git({"config", "remote.origin.url", repo.root.path()});
                repo.git({"config", "remote.origin.uploadpack", "touch .fetch-ran"});
                GitAdapter adapter(gitExecutable);
                for (const auto operation : {Operation::DiscoverRepository, Operation::ListUnmerged, Operation::InspectIndex,
                                             Operation::ReadObject, Operation::ReadAttributes, Operation::Status,
                                             Operation::MaterializeSource, Operation::CheckParentStaging, Operation::StageFile}) {
                    const bool noPath = operation == Operation::DiscoverRepository || operation == Operation::ListUnmerged
                        || operation == Operation::ReadObject || operation == Operation::Status;
                    auto request = repo.request(operation, noPath ? "" : "tracked.txt");
                    if (operation == Operation::ReadObject || operation == Operation::MaterializeSource) request.objectId = blob;
                    const auto result = run(adapter, request);
                    CHECK(result.outcome == Outcome::Rejected && result.mutation == Mutation::NotAttempted);
                    CHECK(result.diagnostic.contains("Outside app"));
                }
                // A missing object must be rejected before Git can try lazy retrieval.
                auto missing = repo.request(Operation::ReadObject);
                missing.objectId = QByteArray(40, '1');
                CHECK(run(adapter, missing).outcome == Outcome::Rejected);
                CHECK(!QFile::exists(repo.path(".fetch-ran")));
                CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
                if (scenario == 0) repo.git({"config", "--unset", "extensions.partialClone"});
                if (scenario == 1) repo.git({"config", "--add", "remote.origin.promisor", "false"});
                if (scenario == 2) CHECK(QFile::remove(repo.path(".git/objects/pack/pack-fixture.promisor")));
                if (scenario == 3) CHECK(QFile::remove(alternate.path(".git/objects/pack/pack-fixture.promisor")));
                auto local = repo.request(Operation::ReadObject);
                local.objectId = blob;
                CHECK(run(adapter, local).outcome == Outcome::Success);
                CHECK(run(adapter, missing).outcome == Outcome::ExitFailure);
                CHECK(!QFile::exists(repo.path(".fetch-ran")));
            }
        });
        suite.run("a registered mergetool parent stages only after a successful release preflight", [] {
            for (const auto &scenario : {"plain", "crlf"}) {
                Repository repo;
                if (QByteArray(scenario) == "crlf") writeFile(repo.path(".gitattributes"), "/tracked.txt text eol=crlf\n");
                registerMergetool(repo, scenario);
                CHECK(repo.git({"ls-files", "--unmerged", "-z"}).count('\0') == 3);
                repo.git({"mergetool", "--no-prompt", "--tool=truce-coverage", "--", "tracked.txt"});
                CHECK(QFile::exists(repo.path(".handoff-released")) && !QFile::exists(repo.path(".handoff-rejected")));
                CHECK(repo.git({"ls-files", "--unmerged", "-z"}).isEmpty());
                CHECK(repo.git({"show", ":tracked.txt"}) == "resolved\n");
                GitAdapter adapter(gitExecutable);
                const auto acknowledged = run(adapter, repo.request(Operation::InspectIndex, "tracked.txt"));
                CHECK(acknowledged.outcome == Outcome::Success && acknowledged.indexEntries.size() == 1);
                CHECK(acknowledged.indexEntries.first().stage == 0);
                CHECK(acknowledged.indexEntries.first().objectId == repo.git({"hash-object", "--stdin"}, "resolved\n").trimmed());
            }
        });
        suite.run("a registered mergetool parent retains conflicts when helper or offline preflight rejects release", [] {
            for (const auto &scenario : {"filter", "hook", "custom-hook", "monitor", "promisor"}) {
                Repository repo;
                registerMergetool(repo, scenario);
                const auto before = repo.git({"ls-files", "--stage", "-z"});
                repo.git({"mergetool", "--no-prompt", "--tool=truce-coverage", "--", "tracked.txt"}, {}, 1);
                CHECK(QFile::exists(repo.path(".handoff-rejected")) && !QFile::exists(repo.path(".handoff-released")));
                CHECK(!QFile::exists(repo.path(".handoff-helper-ran")));
                QFile diagnostic(repo.path(".handoff-preflight"));
                CHECK(diagnostic.open(QIODevice::ReadOnly) && diagnostic.readAll().contains("Outside app"));
                // Disable the monitor before the fixture itself reads the index.
                if (QByteArray(scenario) == "monitor") repo.git({"config", "--unset", "core.fsmonitor"});
                CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
                CHECK(repo.git({"ls-files", "--unmerged", "-z"}).count('\0') == 3);
                QFile merged(repo.path("tracked.txt"));
                CHECK(merged.open(QIODevice::ReadOnly) && merged.readAll().startsWith("<<<<<<< ours\n"));
            }
        });
        suite.run("failed preflight and failed launch retain diagnostics", [] {
            Repository repo;
            const auto preflight = repo.executable("#!/bin/sh\nprintf 'preflight stdout'\nprintf 'preflight stderr' >&2\nexit 17\n");
            GitAdapter failing(preflight);
            const auto result = run(failing, repo.request(Operation::Status));
            CHECK(result.outcome == Outcome::ExitFailure && result.exitCode == -1);
            CHECK(result.standardOutput == "preflight stdout" && result.standardError == "preflight stderr");
            CHECK(result.diagnostic.contains("17") && result.mutation == Mutation::NotAttempted);
            const auto missing = repo.executable("#!/definitely/missing/interpreter\n");
            GitAdapter launch(missing);
            const auto absent = run(launch, repo.request(Operation::Status));
            CHECK(absent.outcome == Outcome::LaunchFailure && absent.exitCode == -1);
            CHECK(!absent.diagnostic.isEmpty() && absent.mutation == Mutation::NotAttempted);
        });
        suite.run("a crashed operation retains its output without a normal exit code", [] {
            Repository repo;
            const auto fake = repo.fakeGit("printf 'crash stdout'\nprintf 'crash stderr' >&2\nkill -KILL $$\n");
            GitAdapter adapter(fake);
            const auto result = run(adapter, repo.request(Operation::Status));
            CHECK(result.outcome == Outcome::ProcessFailure && result.exitCode == -1);
            CHECK(result.standardOutput == "crash stdout" && result.standardError == "crash stderr");
            CHECK(result.mutation == Mutation::NotAttempted && result.diagnostic.contains("crashed"));
        });
        suite.run("destruction before queued launch publishes no result or mutation", [] {
            Repository repo;
            const auto before = repo.git({"ls-files", "--stage", "-z"});
            QObject receiver;
            int completions = 0;
            {
                GitAdapter adapter(gitExecutable);
                QObject::connect(&adapter, &GitAdapter::finished, &receiver, [&](const Result &) { ++completions; });
                CHECK(adapter.start(repo.request(Operation::StageFile, "tracked.txt")));
            }
            QCoreApplication::processEvents();
            CHECK(completions == 0);
            CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
        });
        suite.run("failed mutation retains operation output, stderr and exit through reinspection", [] {
            Repository repo;
            const auto fake = repo.fakeGit(": > .after-add\nprintf 'operation stdout'\nprintf 'operation stderr' >&2\nexit 23\n");
            GitAdapter adapter(fake);
            const auto result = run(adapter, repo.request(Operation::StageFile, "tracked.txt"));
            CHECK(result.outcome == Outcome::ExitFailure && result.exitCode == 23);
            CHECK(result.standardOutput == "operation stdout" && result.standardError == "operation stderr");
            CHECK(result.diagnostic.contains("23") && result.mutation == Mutation::ObservedChanged);
            CHECK(result.indexBefore.contains(QByteArray(40, 'a')) && result.indexAfter.contains(QByteArray(40, 'b')));
        });
        suite.run("failed reinspection retains the original failed operation diagnostics", [] {
            Repository repo;
            const auto fake = repo.fakeGit(": > .after-add\nprintf 'original stdout'\nprintf 'original stderr' >&2\nexit 23\n",
                                           "printf 'inspection stdout'\nprintf 'inspection stderr' >&2\nexit 29\n");
            GitAdapter adapter(fake);
            const auto result = run(adapter, repo.request(Operation::StageFile, "tracked.txt"));
            CHECK(result.outcome == Outcome::ExitFailure && result.exitCode == 23);
            CHECK(result.mutation == Mutation::Uncertain && result.standardOutput == "original stdout");
            CHECK(result.standardError.contains("original stderr") && result.standardError.contains("inspection stderr"));
            CHECK(result.indexAfter == "inspection stdout");
            CHECK(result.diagnostic.contains("23") && result.diagnostic.contains("reinspection failed"));
        });
        suite.run("successful child exit cannot claim staging when index inspection fails", [] {
            Repository repo;
            const auto fake = repo.fakeGit(": > .after-add\nprintf 'successful add stdout'\n",
                                           "printf 'bad index'\nexit 29\n");
            GitAdapter adapter(fake);
            const auto result = run(adapter, repo.request(Operation::StageFile, "tracked.txt"));
            CHECK(result.outcome == Outcome::InspectionFailure && result.exitCode == 0);
            CHECK(result.mutation == Mutation::Uncertain && result.standardOutput == "successful add stdout");
        });
        suite.run("successful staging requires exactly one regular stage-zero index entry", [] {
            const QList<QByteArray> inspections{"", "100644 bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb 2\ttracked.txt",
                                                "120000 bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb 0\ttracked.txt",
                                                "100644 bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb 0\tother.txt"};
            for (const auto &entry : inspections) {
                Repository repo;
                // All supplied bytes are fixed test data with no shell operands.
                const auto inspection = entry.isEmpty() ? QByteArray(":\n") : "printf '" + entry + "\\0'\n";
                const auto fake = repo.fakeGit(": > .after-add\n", inspection);
                GitAdapter adapter(fake);
                const auto result = run(adapter, repo.request(Operation::StageFile, "tracked.txt"));
                CHECK(result.outcome == Outcome::InspectionFailure && result.exitCode == 0);
                CHECK(result.mutation == (entry.isEmpty() || entry.contains(" 2\t") ? Mutation::ObservedChanged : Mutation::Uncertain));
            }
        });
        suite.run("cancellation before queued launch completes once without mutation", [] {
            Repository repo;
            GitAdapter adapter(gitExecutable);
            Completion completion(adapter);
            CHECK(adapter.start(repo.request(Operation::StageFile, "tracked.txt")));
            CHECK(completion.results.empty() && adapter.busy());
            adapter.cancel();
            adapter.cancel();
            const auto result = completion.wait();
            CHECK(result.outcome == Outcome::Cancelled && result.mutation == Mutation::NotAttempted);
            CHECK(result.exitCode == -1);
            adapter.cancel();
            CHECK(!adapter.busy() && completion.results.size() == 1);
        });
        suite.run("cancellation after mutation reinspects while preserving operation output", [] {
            // Cancel either the running mutation or its post-exit reinspection.
            for (const int exitCode : {-1, 0, 128}) {
                Repository repo;
                const bool finished = exitCode >= 0;
                QByteArray operation = ": > .after-add\nprintf 'before cancellation'\n";
                if (finished) {
                    if (exitCode != 0) operation += "printf 'fatal: index.lock exists' >&2\n";
                    operation += "exit " + QByteArray::number(exitCode) + '\n';
                } else {
                    operation += ": > .operation-started\nsleep 60\n";
                }
                QByteArray inspection;
                if (finished) {
                    inspection = ": > .inspection-started\nwhile [ ! -e .release-inspection ]; do sleep 0.01; done\n";
                }
                inspection += "printf '100644 bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb 0\\ttracked.txt\\0'\n";
                const auto fake = repo.fakeGit(operation, inspection);
                GitAdapter adapter(fake);
                Completion completion(adapter);
                QTimer cancel;
                QObject::connect(&cancel, &QTimer::timeout, &cancel, [&] {
                    const auto marker = finished ? ".inspection-started" : ".operation-started";
                    if (QFile::exists(repo.path(marker))) {
                        cancel.stop();
                        adapter.cancel();
                        adapter.cancel();
                        if (finished) writeFile(repo.path(".release-inspection"), "release\n");
                    }
                });
                cancel.start(5);
                const auto result = completion.run(repo.request(Operation::StageFile, "tracked.txt"));
                const auto expected = !finished ? Outcome::Cancelled
                    : exitCode == 0 ? Outcome::Success : Outcome::ExitFailure;
                CHECK(result.outcome == expected && result.cancellationRequested);
                CHECK(result.mutation == Mutation::ObservedChanged);
                CHECK(result.standardOutput == "before cancellation" && !result.indexAfter.isEmpty());
                if (finished) {
                    CHECK(result.exitCode == exitCode);
                    CHECK(result.diagnostic == (exitCode == 0 ? QString{} : QStringLiteral("Git exited with code 128")));
                    CHECK(result.standardError == (exitCode == 0 ? QByteArray{} : QByteArray("fatal: index.lock exists")));
                }
                adapter.cancel();
                const auto next = run(adapter, repo.request(Operation::InspectIndex, "tracked.txt"));
                CHECK(next.outcome == Outcome::Success && !next.cancellationRequested);
            }
        });
        suite.run("concurrent app mutations reject one owner and release ownership after completion", [] {
            Repository repo;
            const auto fake = repo.fakeGit(": > .after-add\n: > .operation-started\nwhile [ ! -e .release ]; do sleep 0.01; done\n");
            GitAdapter first(fake);
            GitAdapter second(fake);
            Completion firstDone(first);
            Completion secondDone(second);
            QTimer startSecond;
            QObject::connect(&startSecond, &QTimer::timeout, &startSecond, [&] {
                if (QFile::exists(repo.path(".operation-started"))) {
                    startSecond.stop();
                    CHECK(second.start(repo.request(Operation::StageFile, "tracked.txt")));
                }
            });
            QObject::connect(&second, &GitAdapter::finished, &startSecond, [&](const Result &) {
                writeFile(repo.path(".release"), "release\n");
            });
            startSecond.start(5);
            CHECK(first.start(repo.request(Operation::StageFile, "tracked.txt")));
            CHECK(firstDone.wait().outcome == Outcome::Success);
            const auto rejected = secondDone.wait();
            CHECK(rejected.outcome == Outcome::Rejected && rejected.mutation == Mutation::NotAttempted);
            CHECK(rejected.diagnostic.contains("Another app-owned mutation"));
            CHECK(run(second, repo.request(Operation::StageFile, "tracked.txt")).outcome == Outcome::Success);
        });
        suite.run("output limit keeps a bounded incomplete prefix and a separate inspection budget", [] {
            Repository repo;
            const auto fake = repo.fakeGit(": > .after-add\nprintf 'operation prefix'\n/usr/bin/head -c 9000000 /dev/zero\n");
            GitAdapter adapter(fake);
            const auto result = run(adapter, repo.request(Operation::StageFile, "tracked.txt"));
            CHECK(result.outcome == Outcome::OutputLimit && result.outputIncomplete);
            CHECK(result.standardOutput.startsWith("operation prefix"));
            CHECK(result.standardOutput.size() <= 8 * 1024 * 1024);
            CHECK(result.mutation == Mutation::ObservedChanged && result.indexAfter.contains(QByteArray(40, 'b')));
        });
        suite.run("the operation deadline cancels the process without reporting success", [] {
            Repository repo;
            const auto fake = repo.fakeGit("printf 'deadline prefix'\nsleep 60\n");
            GitAdapter adapter(fake);
            Completion completion(adapter);
            QElapsedTimer clock;
            clock.start();
            const auto result = completion.run(repo.request(Operation::Status), 35000);
            CHECK(result.outcome == Outcome::TimedOut && result.mutation == Mutation::NotAttempted);
            CHECK(result.standardOutput == "deadline prefix" && result.diagnostic.contains("deadline"));
            CHECK(clock.elapsed() >= 25000 && clock.elapsed() < 35000);
        });
        return suite.report();
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    if (arguments.size() == 4 && arguments[1] == "--mergetool-fixture") {
        try {
            return mergetoolFixture(arguments[2], arguments[3]);
        } catch (const std::exception &error) {
            std::cerr << "mergetool fixture: " << error.what() << '\n';
            return 2;
        }
    }
    return GitAdapterTests{}.runTests();
}
