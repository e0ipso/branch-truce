#include "GitAdapter.h"
#include "TextConversion.h"
#include "ConflictClassifier.h"
#include "TestHarness.h"
#include "fixtures/conversions/ConversionCases.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <fcntl.h>
#include <sys/stat.h>
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
                    const QByteArray &inspection = "printf '100644 bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb 0\\ttracked.txt\\0'\n",
                    const QByteArray &configuration = {},
                    const QByteArray &attributes = "printf 'tracked.txt\\0filter\\0unspecified\\0'\n") const
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
    config)
)SH";
        script += configuration;
        script += R"SH(
        ;;
    rev-parse)
        case "$*" in
            *--git-path*objects*) printf '%s/.git/objects\n' "$PWD" ;;
            *--git-path*hooks*) printf '%s/.git/hooks\n' "$PWD" ;;
            *--show-toplevel*) printf '%s\n' "$PWD" ;;
            *) printf '%s/.git\n' "$PWD" ;;
        esac ;;
    check-attr)
)SH";
        script += attributes;
        script += R"SH(
        ;;
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

QByteArray commitFixture(const Repository &repo, const QString &message)
{
    repo.git({"-c", "user.name=Branch Truce Tests", "-c", "user.email=tests@example.invalid",
              "commit", "--quiet", "-am", message});
    return repo.git({"rev-parse", "HEAD"}).trimmed();
}

QByteArray numberedText(const QByteArray &tenth)
{
    QByteArray bytes;
    for (int line = 1; line <= 50; ++line) bytes += (line == 10 ? tenth : QByteArray::number(line)) + '\n';
    return bytes;
}

struct ConflictHistory { QByteArray base; QByteArray main; QByteArray side; };

struct ConflictContents {
    QByteArray base = "base\n";
    QByteArray main = "main\n";
    QByteArray side = "side\n";
};

ConflictHistory conflictHistory(Repository &repo, bool renamed = false, const ConflictContents &contents = {})
{
    repo.git({"config", "user.name", "Branch Truce Tests"});
    repo.git({"config", "user.email", "tests@example.invalid"});
    writeFile(repo.path("tracked.txt"), contents.base);
    if (renamed) {
        writeFile(repo.path("a.txt"), numberedText("10"));
        repo.git({"add", "--", "a.txt"});
    }
    ConflictHistory history;
    history.base = commitFixture(repo, "base");
    repo.git({"checkout", "--quiet", "-b", "side"});
    writeFile(repo.path("tracked.txt"), contents.side);
    if (renamed) writeFile(repo.path("a.txt"), numberedText("side"));
    history.side = commitFixture(repo, "side");
    repo.git({"checkout", "--quiet", "main"});
    writeFile(repo.path("tracked.txt"), contents.main);
    if (renamed) {
        repo.git({"mv", "--", "a.txt", "b.txt"});
        writeFile(repo.path("b.txt"), numberedText("main"));
    }
    history.main = commitFixture(repo, "main");
    return history;
}

void checkOperationIdentity(const Result &result, const ConflictHistory &history, RepositoryOperation operation)
{
    CHECK(result.outcome == Outcome::Success && result.exitCode == 0);
    CHECK(result.mutation == Mutation::NotAttempted && !result.outputIncomplete);
    CHECK(result.repository.supported && result.repository.operation == operation);
    CHECK(result.repository.unsupportedPaths.isEmpty() && result.repository.diagnostic.isEmpty());
    CHECK(result.repository.stage2.commitId == history.main && result.repository.stage3.commitId == history.side);
    CHECK(result.repository.baseCommitId == history.base);
    CHECK(result.repository.stage2.label.contains(QString::fromLatin1(history.main)));
    CHECK(result.repository.stage3.label.contains(QString::fromLatin1(history.side)));
    if (operation == RepositoryOperation::Rebase) {
        CHECK(result.repository.stage2.label.startsWith("Current rebased state:"));
        CHECK(result.repository.stage3.label.startsWith("Commit being replayed:"));
        CHECK(result.repository.headName == "refs/heads/side");
        CHECK(result.repository.originalHead == history.side && result.repository.onto == history.main);
    } else {
        CHECK(result.repository.stage2.label.startsWith("Current checkout:"));
        CHECK(result.repository.stage3.label.startsWith("Merged commit:"));
        CHECK(result.repository.headName.isEmpty() && result.repository.originalHead.isEmpty() && result.repository.onto.isEmpty());
    }
}

void checkUnsupportedDiscovery(Repository &repo, const QString &reason, Outcome outcome = Outcome::InspectionFailure)
{
    const auto before = repo.git({"ls-files", "--unmerged", "-z"});
    GitAdapter adapter(gitExecutable);
    const auto result = run(adapter, repo.request(Operation::DiscoverRepository));
    CHECK(result.outcome == outcome && !result.repository.supported);
    CHECK(result.repository.operation == RepositoryOperation::Unsupported);
    CHECK(result.repository.diagnostic.contains(reason));
    CHECK(result.repository.stage2.commitId.isEmpty() && result.repository.stage3.commitId.isEmpty());
    CHECK(result.indexEntries.size() == before.count('\0') && !result.indexEntries.isEmpty());
    CHECK(result.mutation == Mutation::NotAttempted);
    CHECK(repo.git({"ls-files", "--unmerged", "-z"}) == before);
}

TextConversionRequest textRequest(const Repository &repo, TextOperation operation, const QByteArray &object)
{
    TextConversionRequest request;
    request.operation = operation;
    request.directory = repo.root.path();
    request.path = "tracked.txt";
    request.objectId = object;
    request.sessionGeneration = 71;
    request.inputRevision = 93;
    request.stages = {{request.path, "100644", object, 2}, {request.path, "100644", object, 3}};
    return request;
}

TextConversionResult run(TextConversion &conversion, const TextConversionRequest &request)
{
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    std::vector<TextConversionResult> results;
    QObject::connect(&conversion, &TextConversion::finished, &loop, [&](const TextConversionResult &result) {
        results.push_back(result);
        loop.quit();
    });
    CHECK(conversion.start(request));
    CHECK(conversion.busy() && results.empty() && !conversion.start(request));
    deadline.start(10000);
    loop.exec();
    CHECK(results.size() == 1 && !conversion.busy());
    CHECK(results.front().request.sessionGeneration == request.sessionGeneration);
    CHECK(results.front().request.inputRevision == request.inputRevision);
    return results.front();
}

void checkConversionCleanup(Repository &repo)
{
    const auto root = repo.root.path();
    const auto executables = repo.executables.path();
    CHECK(repo.root.remove() && repo.executables.remove());
    CHECK(!QFileInfo::exists(root) && !QFileInfo::exists(executables));
}

ConflictClassificationResult run(ConflictClassifier &classifier, const ConflictClassificationRequest &request)
{
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    std::vector<ConflictClassificationResult> results;
    QObject::connect(&classifier, &ConflictClassifier::finished, &loop, [&](const auto &result) {
        results.push_back(result);
        loop.quit();
    });
    CHECK(classifier.start(request));
    CHECK(classifier.busy() && results.empty() && !classifier.start(request));
    deadline.start(10000);
    loop.exec();
    CHECK(results.size() == 1 && !classifier.busy());
    CHECK(results.front().request.sessionGeneration == request.sessionGeneration);
    CHECK(results.front().request.inputRevision == request.inputRevision);
    return results.front();
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
    int runClassificationTests()
    {
        test_support::Suite suite("ConflictClassifierTests");
        for (const auto &[scenario, expected] : std::vector<std::pair<QByteArray, ConflictSupport>>{
                 {"regular", ConflictSupport::SupportedText}, {"executable", ConflictSupport::SupportedText},
                 {"missing-ours", ConflictSupport::MissingSide}, {"missing-theirs", ConflictSupport::MissingSide},
                 {"mode-disagreement", ConflictSupport::ModeConflict}, {"symlink-source", ConflictSupport::UnsupportedType},
                 {"submodule-source", ConflictSupport::UnsupportedType}, {"binary-source", ConflictSupport::UnsupportedText},
                 {"invalid-utf8-source", ConflictSupport::UnsupportedText}, {"mixed-newlines-source", ConflictSupport::UnsupportedText},
                 {"invalid-result", ConflictSupport::UnsupportedText}, {"missing-result", ConflictSupport::TopologyConflict},
                 {"directory-result", ConflictSupport::TopologyConflict}, {"fifo-result", ConflictSupport::TopologyConflict},
                 {"symlink-result", ConflictSupport::TopologyConflict}, {"excluded-attributes", ConflictSupport::UnsupportedConversion},
                 {"filter", ConflictSupport::UnsupportedConversion}, {"unverified-source", ConflictSupport::UnverifiedSource}}) {
            suite.run("complete-conflict classification matrix: " + scenario.toStdString(), [=] {
                Repository repo;
                ConflictContents contents;
                if (scenario == "binary-source") contents.side = QByteArray("binary\0side\n", 12);
                if (scenario == "invalid-utf8-source") contents.side = QByteArray("side-") + char(0xff) + '\n';
                if (scenario == "mixed-newlines-source") contents.side = "side\r\nsecond\n";
                if (scenario == "executable") CHECK(::chmod(QFile::encodeName(repo.path("tracked.txt")).constData(), 0755) == 0);
                const auto history = conflictHistory(repo, false, contents);
                repo.git({"merge", "--no-edit", "side"}, {}, 1);
                const QByteArray manual = "preserved manual UTF-8 \xc3\xa9\n";
                writeFile(repo.path("tracked.txt"), manual);

                GitAdapter adapter(gitExecutable);
                auto entries = run(adapter, repo.request(Operation::ListUnmerged)).indexEntries;
                for (auto &entry : entries) {
                    if (entry.stage == 3 && scenario == "mode-disagreement") entry.mode = "100755";
                    if (entry.stage == 3 && scenario == "symlink-source") entry.mode = "120000";
                    if (entry.stage == 3 && scenario == "submodule-source") { entry.mode = "160000"; entry.objectId = history.side; }
                    if (entry.stage == 3 && scenario == "unverified-source")
                        entry.objectId = repo.git({"hash-object", "-w", "--stdin"}, "not in either source\n").trimmed();
                }
                QByteArray index = "0 " + QByteArray(40, '0') + "\ttracked.txt" + QByteArray(1, '\0');
                for (const auto &entry : entries) {
                    if ((scenario == "missing-ours" && entry.stage == 2) || (scenario == "missing-theirs" && entry.stage == 3)) continue;
                    index += entry.mode + ' ' + entry.objectId + ' ' + QByteArray::number(entry.stage) + "\ttracked.txt" + QByteArray(1, '\0');
                }
                repo.git({"update-index", "-z", "--index-info"}, index);
                if (scenario == "invalid-result") writeFile(repo.path("tracked.txt"), QByteArray(1, char(0xff)));
                if (scenario == "missing-result" || scenario == "directory-result" || scenario == "fifo-result" || scenario == "symlink-result") {
                    CHECK(QFile::remove(repo.path("tracked.txt")));
                    if (scenario == "directory-result") CHECK(QDir(repo.root.path()).mkdir("tracked.txt"));
                    if (scenario == "fifo-result") CHECK(::mkfifo(QFile::encodeName(repo.path("tracked.txt")).constData(), 0600) == 0);
                    if (scenario == "symlink-result") {
                        writeFile(repo.path("outside.txt"), "do not follow\n");
                        CHECK(::symlink("outside.txt", QFile::encodeName(repo.path("tracked.txt")).constData()) == 0);
                    }
                }
                if (scenario == "excluded-attributes") writeFile(repo.path(".gitattributes"), "tracked.txt -text eol=crlf\n");
                if (scenario == "filter") {
                    writeFile(repo.path(".gitattributes"), "tracked.txt filter=external\n");
                    repo.git({"config", "filter.external.clean", "touch .filter-ran; cat"});
                }
                const auto before = repo.git({"ls-files", "--stage", "-z"});
                const auto objects = repo.git({"count-objects", "-v"});
                ConflictClassifier classifier(gitExecutable);
                const auto result = run(classifier, {repo.root.path(), 71, 93});
                CHECK(result.outcome == Outcome::Success && result.repository.supported && result.conflicts.size() == 1);
                const auto &conflict = result.conflicts.front();
                CHECK(conflict.path == "tracked.txt" && conflict.support == expected);
                CHECK(conflict.editable() == (expected == ConflictSupport::SupportedText));
                if (conflict.editable()) {
                    CHECK(conflict.reason.isEmpty() && conflict.sources.size() == 3 && conflict.result);
                    CHECK(conflict.result->utf8 == manual && conflict.result->originalBytes == manual);
                    CHECK(conflict.mode == (scenario == "executable" ? "100755" : "100644"));
                } else {
                    CHECK(!conflict.reason.isEmpty() && conflict.sources.isEmpty() && !conflict.result);
                }
                CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
                CHECK(repo.git({"count-objects", "-v"}) == objects);
                CHECK(repo.git({"rev-parse", "HEAD"}).trimmed() == history.main);
                CHECK(!QFile::exists(repo.path(".filter-ran")));
                if (scenario == "symlink-result") {
                    QFile outside(repo.path("outside.txt"));
                    CHECK(outside.open(QIODevice::ReadOnly) && outside.readAll() == "do not follow\n");
                }
            });
        }
        suite.run("add/add conflicts remain editable without inventing an earlier source", [] {
            Repository repo;
            repo.git({"config", "user.name", "Branch Truce Tests"});
            repo.git({"config", "user.email", "tests@example.invalid"});
            repo.git({"rm", "--force", "--quiet", "--", "tracked.txt"});
            repo.git({"commit", "--allow-empty", "--quiet", "-m", "base"});
            repo.git({"checkout", "--quiet", "-b", "side"});
            writeFile(repo.path("tracked.txt"), "side addition\n");
            repo.git({"add", "--", "tracked.txt"});
            commitFixture(repo, "side addition");
            repo.git({"checkout", "--quiet", "main"});
            writeFile(repo.path("tracked.txt"), "main addition\n");
            repo.git({"add", "--", "tracked.txt"});
            commitFixture(repo, "main addition");
            repo.git({"merge", "--no-edit", "side"}, {}, 1);
            const auto before = repo.git({"ls-files", "--stage", "-z"});
            ConflictClassifier classifier(gitExecutable);
            const auto result = run(classifier, {repo.root.path(), 71, 93});
            CHECK(result.outcome == Outcome::Success && result.conflicts.size() == 1);
            const auto &conflict = result.conflicts.front();
            CHECK(conflict.editable() && conflict.stages.size() == 2 && conflict.sources.size() == 2 && conflict.result);
            CHECK(conflict.stages[0].stage == 2 && conflict.stages[1].stage == 3);
            CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
        });
        suite.run("mixed classification preserves every literal filename byte and per-path exclusion", [] {
            Repository repo;
            repo.git({"config", "user.name", "Branch Truce Tests"});
            repo.git({"config", "user.email", "tests@example.invalid"});
            CHECK(QDir(repo.root.path()).mkdir("nested"));
            auto names = oddPaths();
            names.prepend("tracked.txt");
            for (const auto &name : names) repo.writePath(name, "base\n");
            repo.git({"add", "--", "."});
            commitFixture(repo, "literal base");
            repo.git({"checkout", "--quiet", "-b", "side"});
            for (const auto &name : names) repo.writePath(name, name == names.last() ? QByteArray(1, char(0xff)) : QByteArray("side\n"));
            commitFixture(repo, "literal side");
            repo.git({"checkout", "--quiet", "main"});
            for (const auto &name : names) repo.writePath(name, "main\n");
            commitFixture(repo, "literal main");
            repo.git({"merge", "--no-edit", "side"}, {}, 1);
            for (const auto &name : names) repo.writePath(name, "manual UTF-8 \xc3\xa9\n");
            const auto before = repo.git({"ls-files", "--stage", "-z"});
            ConflictClassifier classifier(gitExecutable);
            const auto result = run(classifier, {repo.root.path(), 71, 93});
            CHECK(result.outcome == Outcome::Success && result.conflicts.size() == names.size());
            QSet<QByteArray> seen;
            for (const auto &conflict : result.conflicts) {
                CHECK(names.contains(conflict.path) && !seen.contains(conflict.path));
                seen.insert(conflict.path);
                CHECK(conflict.stages.size() == 3);
                if (conflict.path == names.last()) {
                    CHECK(conflict.support == ConflictSupport::UnsupportedText && !conflict.editable());
                    CHECK(!conflict.reason.isEmpty() && conflict.sources.isEmpty() && !conflict.result);
                } else {
                    CHECK(conflict.editable() && conflict.reason.isEmpty() && conflict.result && conflict.sources.size() == 3);
                    CHECK(conflict.result->originalBytes == "manual UTF-8 \xc3\xa9\n");
                }
            }
            CHECK(seen.size() == names.size() && repo.git({"ls-files", "--stage", "-z"}) == before);
        });
        return suite.report();
    }
    int runTests()
    {
        test_support::Suite suite("GitAdapterTests");
        suite.run("merge discovery uses per-worktree metadata and verified stage identities", [] {
            for (const bool linked : {false, true}) {
                Repository repo;
                const auto history = conflictHistory(repo);
                QTemporaryDir linkedParent;
                CHECK(linkedParent.isValid());
                auto directory = repo.root.path();
                if (linked) {
                    directory = linkedParent.path() + "/checkout";
                    repo.git({"worktree", "add", "--quiet", "--detach", directory, "main"});
                }
                repo.git({"-C", directory, "merge", "--no-edit", "side"}, {}, 1);
                CHECK(QDir().mkpath(directory + "/nested"));
                auto request = repo.request(Operation::DiscoverRepository);
                request.directory = directory + "/nested";
                const auto before = repo.git({"-C", directory, "ls-files", "--unmerged", "-z"});
                GitAdapter adapter(gitExecutable);
                const auto result = run(adapter, request);
                checkOperationIdentity(result, history, RepositoryOperation::Merge);
                CHECK(result.repository.worktreeRoot == directory && result.indexEntries.size() == 3);
                const auto gitDirectory = QString::fromUtf8(repo.git({"-C", directory, "rev-parse", "--absolute-git-dir"}).trimmed());
                CHECK(result.repository.gitDirectory == QFileInfo(gitDirectory).canonicalFilePath());
                writeFile(directory + "/tracked.txt", "launch-time manual result\n");
                ConflictClassifier classifier(gitExecutable);
                const auto classified = run(classifier, {request.directory, 71, 93});
                CHECK(classified.outcome == Outcome::Success && classified.conflicts.size() == 1);
                const auto &conflict = classified.conflicts.front();
                CHECK(conflict.editable() && conflict.reason.isEmpty() && conflict.path == "tracked.txt");
                CHECK(conflict.mode == "100644" && conflict.stages.size() == 3 && conflict.sources.size() == 3);
                CHECK(conflict.result && conflict.result->originalBytes == "launch-time manual result\n");
                CHECK(repo.git({"-C", directory, "ls-files", "--unmerged", "-z"}) == before);
                if (linked) {
                    CHECK(result.repository.gitDirectory != repo.path(".git"));
                    const auto original = run(adapter, repo.request(Operation::DiscoverRepository));
                    CHECK(original.outcome == Outcome::Success && original.repository.operation == RepositoryOperation::None);
                    CHECK(!original.repository.supported && original.indexEntries.isEmpty());
                }
            }
        });
        suite.run("both rebase backends label the current state and replayed commit truthfully", [] {
            for (const auto backend : {"--merge", "--apply"}) {
                Repository repo;
                const auto history = conflictHistory(repo);
                repo.git({"checkout", "--quiet", "side"});
                repo.git({"-c", "user.name=Branch Truce Tests", "-c", "user.email=tests@example.invalid",
                          "rebase", backend, "main"}, {}, 1);
                GitAdapter adapter(gitExecutable);
                const auto before = repo.git({"ls-files", "--unmerged", "-z"});
                const auto result = run(adapter, repo.request(Operation::DiscoverRepository));
                checkOperationIdentity(result, history, RepositoryOperation::Rebase);
                CHECK(result.indexEntries.size() == 3 && repo.git({"ls-files", "--unmerged", "-z"}) == before);
                CHECK(result.repository.gitDirectory == repo.path(".git"));
                ConflictClassifier classifier(gitExecutable);
                const auto classified = run(classifier, {repo.root.path(), 71, 93});
                CHECK(classified.outcome == Outcome::Success && classified.conflicts.size() == 1);
                CHECK(classified.conflicts.front().editable() && classified.conflicts.front().sources.size() == 3);
            }
        });
        suite.run("unsupported and inconsistent operation metadata cannot acquire source labels", [] {
            for (int scenario = 0; scenario < 8; ++scenario) {
                Repository repo;
                const auto history = conflictHistory(repo);
                repo.git({"merge", "--no-edit", "side"}, {}, 1);
                QString reason;
                auto outcome = Outcome::InspectionFailure;
                if (scenario == 0) {
                    CHECK(QDir().mkdir(repo.path(".git/rebase-merge")));
                    reason = "Conflicting merge, rebase";
                } else if (scenario == 1) {
                    writeFile(repo.path(".git/MERGE_HEAD"), history.side + '\n' + history.main + '\n');
                    reason = "multiple MERGE_HEAD";
                } else if (scenario == 2) {
                    writeFile(repo.path(".git/MERGE_HEAD"), "not-a-commit\n");
                    reason = "Invalid commit identity";
                } else if (scenario == 3) {
                    CHECK(QFile::remove(repo.path(".git/MERGE_HEAD")));
                    writeFile(repo.path("marker"), history.side + '\n');
                    CHECK(QFile::link(repo.path("marker"), repo.path(".git/MERGE_HEAD")));
                    reason = "not a regular file";
                } else {
                    CHECK(QFile::remove(repo.path(".git/MERGE_HEAD")));
                    if (scenario == 4) {
                        writeFile(repo.path(".git/CHERRY_PICK_HEAD"), history.side + '\n');
                        reason = "cherry-pick, revert and sequencer";
                        outcome = Outcome::Success;
                    } else if (scenario == 5) {
                        writeFile(repo.path(".git/REVERT_HEAD"), history.side + '\n');
                        reason = "cherry-pick, revert and sequencer";
                        outcome = Outcome::Success;
                    } else if (scenario == 6) {
                        CHECK(QDir().mkdir(repo.path(".git/rebase-apply")));
                        writeFile(repo.path(".git/rebase-apply/applying"), "");
                        reason = "apply-mailbox";
                        outcome = Outcome::Success;
                    } else {
                        writeFile(repo.path(".git/REBASE_HEAD"), history.side + '\n');
                        reason = "REBASE_HEAD exists without";
                    }
                }
                checkUnsupportedDiscovery(repo, reason, outcome);
            }
        });
        suite.run("rebase discovery rejects disagreement and unverified ancestry", [] {
            for (int scenario = 0; scenario < 6; ++scenario) {
                Repository repo;
                const auto history = conflictHistory(repo);
                repo.git({"checkout", "--quiet", "side"});
                repo.git({"rebase", "--merge", "main"}, {}, 1);
                QString reason;
                if (scenario == 0) {
                    writeFile(repo.path(".git/rebase-merge/stopped-sha"), history.main + '\n');
                    reason = "replay metadata disagrees";
                } else if (scenario == 1) {
                    writeFile(repo.path(".git/rebase-merge/head-name"), "refs/heads/../side\n");
                    reason = "Invalid rebase head-name";
                } else if (scenario == 2) {
                    writeFile(repo.path(".git/rebase-merge/orig-head"), history.main + '\n');
                    reason = "not part of the original rebase history";
                } else if (scenario == 3) {
                    writeFile(repo.path(".git/rebase-merge/onto"), history.side + '\n');
                    reason = "not based on the recorded rebase onto";
                } else if (scenario == 4) {
                    CHECK(QFile::remove(repo.path(".git/REBASE_HEAD")));
                    CHECK(QFile::remove(repo.path(".git/rebase-merge/stopped-sha")));
                    reason = "no verified commit being replayed";
                } else {
                    CHECK(QFile::remove(repo.path(".git/rebase-merge/orig-head")));
                    reason = "Required operation metadata is missing";
                }
                checkUnsupportedDiscovery(repo, reason);
            }
        });
        suite.run("multiple merge bases and replayed merge commits reject ambiguous provenance", [] {
            Repository repo;
            const auto history = conflictHistory(repo);
            const auto mergeCommit = [&](const QByteArray &first, const QByteArray &second) {
                const auto tree = repo.git({"rev-parse", QString::fromLatin1(first) + "^{tree}"}).trimmed();
                return repo.git({"-c", "user.name=Branch Truce Tests", "-c", "user.email=tests@example.invalid",
                    "commit-tree", QString::fromLatin1(tree), "-p", QString::fromLatin1(first), "-p",
                    QString::fromLatin1(second), "-m", "criss-cross"}).trimmed();
            };
            const auto left = mergeCommit(history.main, history.side);
            const auto right = mergeCommit(history.side, history.main);
            repo.git({"reset", "--hard", QString::fromLatin1(left)});
            repo.git({"update-ref", "refs/heads/side", QString::fromLatin1(right)});
            CHECK(repo.git({"merge-base", "--all", "main", "side"}).count('\n') == 2);
            repo.git({"merge", "--no-edit", "side"}, {}, 1);
            checkUnsupportedDiscovery(repo, "multiple merge bases");
            CHECK(QFile::remove(repo.path(".git/MERGE_HEAD")));
            CHECK(QDir().mkdir(repo.path(".git/rebase-merge")));
            writeFile(repo.path(".git/rebase-merge/head-name"), "refs/heads/side\n");
            writeFile(repo.path(".git/rebase-merge/orig-head"), right + '\n');
            writeFile(repo.path(".git/rebase-merge/onto"), history.main + '\n');
            writeFile(repo.path(".git/rebase-merge/stopped-sha"), right + '\n');
            writeFile(repo.path(".git/REBASE_HEAD"), right + '\n');
            checkUnsupportedDiscovery(repo, "replaying merge commits");
        });
        suite.run("renamed text conflicts verify old-path blobs and isolate invalid paths", [] {
            Repository repo;
            const auto history = conflictHistory(repo, true);
            repo.git({"merge", "--no-edit", "side"}, {}, 1);
            const auto before = repo.git({"ls-files", "--unmerged", "-z"});
            GitAdapter adapter(gitExecutable);
            const auto discovered = run(adapter, repo.request(Operation::DiscoverRepository));
            checkOperationIdentity(discovered, history, RepositoryOperation::Merge);
            CHECK(discovered.indexEntries.size() == 6);
            ConflictClassifier classifier(gitExecutable);
            const auto classified = run(classifier, {repo.root.path(), 71, 93});
            CHECK(classified.outcome == Outcome::Success && classified.conflicts.size() == 2);
            for (const auto &conflict : classified.conflicts) CHECK(conflict.editable() && conflict.sources.size() == 3);
            CHECK(repo.git({"ls-tree", QString::fromLatin1(history.base), "--", "b.txt"}).isEmpty());
            CHECK(repo.git({"ls-tree", QString::fromLatin1(history.side), "--", "b.txt"}).isEmpty());
            for (const auto &entry : discovered.indexEntries) {
                if (entry.path != "b.txt") continue;
                const auto commit = entry.stage == 1 ? history.base : entry.stage == 2 ? history.main : history.side;
                const auto sourcePath = entry.stage == 2 ? "b.txt" : "a.txt";
                CHECK(entry.objectId == repo.git({"rev-parse", QString::fromLatin1(commit) + ':' + sourcePath}).trimmed());
                CHECK(entry.mode == "100644");
            }
            const auto unrelated = repo.git({"hash-object", "-w", "--stdin"}, "unrelated\n").trimmed();
            const auto nul = QByteArray(1, '\0');
            for (const auto &path : {QByteArray("b.txt"), QByteArray("tracked.txt")}) {
                for (const bool wrongMode : {false, true}) {
                    repo.git({"update-index", "-z", "--index-info"}, before);
                    QByteArray object;
                    for (const auto &entry : discovered.indexEntries) {
                        if (entry.path == path && entry.stage == 3) object = entry.objectId;
                    }
                    CHECK(!object.isEmpty());
                    repo.git({"update-index", "-z", "--index-info"},
                        (wrongMode ? QByteArray("100755 ") : QByteArray("100644 "))
                        + (wrongMode ? object : unrelated) + " 3\t" + path + nul);
                    const auto index = repo.git({"ls-files", "--unmerged", "-z"});
                    const auto result = run(adapter, repo.request(Operation::DiscoverRepository));
                    CHECK(result.outcome == Outcome::Success && result.repository.supported);
                    CHECK(result.repository.operation == RepositoryOperation::Merge && result.indexEntries.size() == 6);
                    CHECK(result.repository.unsupportedPaths.size() == 1 && result.repository.unsupportedPaths.contains(path));
                    CHECK(result.repository.unsupportedPaths.value(path).contains("stage 3 provenance"));
                    CHECK(result.repository.stage2.commitId == history.main && result.repository.stage3.commitId == history.side);
                    const auto classifiedInvalid = run(classifier, {repo.root.path(), 71, 93});
                    CHECK(classifiedInvalid.outcome == Outcome::Success && classifiedInvalid.conflicts.size() == 2);
                    for (const auto &conflict : classifiedInvalid.conflicts) {
                        if (conflict.path == path) {
                            CHECK(!conflict.editable() && !conflict.reason.isEmpty() && !conflict.result && conflict.sources.isEmpty());
                            CHECK(conflict.support == (wrongMode ? ConflictSupport::ModeConflict : ConflictSupport::UnverifiedSource));
                        } else CHECK(conflict.editable());
                    }
                    CHECK(repo.git({"ls-files", "--unmerged", "-z"}) == index);
                }
            }
        });
        suite.run("conversion format matrix preserves reviewed bytes through real Git publication", [] {
            for (const auto &format : conversion_fixtures::accepted) {
                std::cout << "FORMAT " << format.name << '\n';
                Repository repo;
                if (!format.attributes.isEmpty()) writeFile(repo.path(".gitattributes"), "tracked.txt " + format.attributes + '\n');
                if (!format.configuration.isEmpty()) repo.git({"config", format.configuration[0], format.configuration[1]});
                const auto object = repo.git({"hash-object", "-w", "--stdin"}, format.canonicalSource).trimmed();
                const auto nul = QByteArray(1, '\0');
                repo.git({"update-index", "-z", "--index-info"},
                    "0 " + QByteArray(40, '0') + "\ttracked.txt" + nul
                    + format.mode + ' ' + object + " 2\ttracked.txt" + nul
                    + format.mode + ' ' + object + " 3\ttracked.txt" + nul);
                GitAdapter adapter(gitExecutable);
                const auto captured = run(adapter, repo.request(Operation::InspectIndex, "tracked.txt"));
                CHECK(captured.outcome == Outcome::Success && captured.indexEntries.size() == 2);
                auto sourceRequest = textRequest(repo, TextOperation::ReadSource, object);
                sourceRequest.stages = captured.indexEntries;
                TextConversion conversion(gitExecutable);
                const auto source = run(conversion, sourceRequest);
                CHECK(source.outcome == Outcome::Success && source.text);
                CHECK(source.text->canonicalBytes == format.canonicalSource && source.text->canonicalObjectId == object);
                CHECK(source.text->document.originalBytes == format.checkoutSource);
                CHECK(source.text->document.utf8 == format.displayedSource);
                CHECK(source.text->mode == format.mode && source.text->sourceMode == format.mode);
                if (!format.configuration.isEmpty()) {
                    CHECK(source.text->policy.configuration.value(format.configuration[0].toLatin1()) == format.configuration[1].toLatin1());
                }
                const auto before = repo.git({"ls-files", "--stage", "-z"});
                auto request = sourceRequest;
                request.operation = TextOperation::PrepareResult;
                request.launchResultFormat = format.launch;
                request.editedUtf8 = format.editedUtf8;
                const auto prepared = run(conversion, request);
                CHECK(prepared.outcome == Outcome::Success && prepared.text);
                CHECK(prepared.text->document.utf8 == format.editedUtf8);
                CHECK(prepared.text->document.format == format.resultFormat && prepared.text->mode == format.mode);
                CHECK(prepared.text->document.originalBytes == format.checkoutResult);
                CHECK(prepared.text->canonicalBytes == format.canonicalResult && prepared.text->policy == source.text->policy);
                const auto expectedObject = repo.git({"hash-object", "--no-filters", "--stdin"}, format.canonicalResult).trimmed();
                CHECK(prepared.text->canonicalObjectId == expectedObject);
                CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
                QFile untouched(repo.path("tracked.txt"));
                CHECK(untouched.open(QIODevice::ReadOnly) && untouched.readAll() == "before\n");
                if (expectedObject != object) repo.git({"cat-file", "-e", QString::fromLatin1(expectedObject)}, {}, 1);
                untouched.close();
                writeFile(repo.path("tracked.txt"), prepared.text->document.originalBytes, format.mode == "100755");
                CHECK(QFileInfo(repo.path("tracked.txt")).isExecutable() == (format.mode == "100755"));
                const auto staged = run(adapter, repo.request(Operation::StageFile, "tracked.txt"));
                CHECK(staged.outcome == Outcome::Success && staged.mutation == Mutation::ObservedChanged);
                CHECK(staged.indexBefore == before);
                CHECK(staged.indexAfter == format.mode + ' ' + expectedObject + " 0\ttracked.txt" + nul);
                CHECK(repo.git({"ls-files", "--unmerged", "-z"}).isEmpty());
                auto verify = textRequest(repo, TextOperation::VerifyPublished, {});
                verify.expected = prepared.text;
                const auto published = run(conversion, verify);
                CHECK(published.outcome == Outcome::Success && published.text);
                CHECK(published.text->canonicalBytes == format.canonicalResult && published.text->canonicalObjectId == expectedObject);
                CHECK(published.text->document.utf8 == format.editedUtf8 && published.text->document.originalBytes == format.checkoutResult);
                CHECK(published.text->document.format == format.resultFormat && published.text->mode == format.mode);
                CHECK(repo.git({"show", ":tracked.txt"}) == format.canonicalResult);
                QFile written(repo.path("tracked.txt"));
                CHECK(written.open(QIODevice::ReadOnly) && written.readAll() == format.checkoutResult);
                written.close();
                checkConversionCleanup(repo);
            }
        });
        suite.run("invalid source bytes and contradictory attributes reject without accepted text or writes", [] {
            for (const auto &invalid : conversion_fixtures::rejectedSources) {
                std::cout << "REJECT SOURCE " << invalid.name << '\n';
                Repository repo;
                if (!invalid.attributes.isEmpty()) writeFile(repo.path(".gitattributes"), "tracked.txt " + invalid.attributes + '\n');
                const auto object = repo.git({"hash-object", "-w", "--stdin"}, invalid.canonical).trimmed();
                const auto before = repo.git({"ls-files", "--stage", "-z"});
                TextConversion conversion(gitExecutable);
                const auto rejected = run(conversion, textRequest(repo, TextOperation::ReadSource, object));
                CHECK(rejected.outcome == Outcome::Rejected && !rejected.text && !rejected.diagnostic.isEmpty());
                CHECK(rejected.failure == invalid.failure);
                CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
                checkConversionCleanup(repo);
            }
            for (const auto attributes : conversion_fixtures::rejectedAttributes) {
                std::cout << "REJECT ATTRIBUTES " << attributes << '\n';
                Repository repo;
                writeFile(repo.path(".gitattributes"), QByteArray("tracked.txt ") + attributes + '\n');
                const auto object = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
                const auto before = repo.git({"ls-files", "--stage", "-z"});
                TextConversion conversion(gitExecutable);
                for (const auto operation : {TextOperation::ReadSource, TextOperation::PrepareResult}) {
                    auto request = textRequest(repo, operation, object);
                    request.editedUtf8 = "reviewed\n";
                    const auto rejected = run(conversion, request);
                    CHECK(rejected.outcome == Outcome::Rejected && !rejected.text && !rejected.diagnostic.isEmpty());
                    CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
                }
                checkConversionCleanup(repo);
            }
            for (const auto &invalid : conversion_fixtures::rejectedEdits) {
                std::cout << "REJECT EDIT " << invalid.name << '\n';
                Repository repo;
                const auto object = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
                const auto before = repo.git({"ls-files", "--stage", "-z"});
                TextConversion conversion(gitExecutable);
                auto request = textRequest(repo, TextOperation::PrepareResult, object);
                request.editedUtf8 = invalid.bytes;
                const auto rejected = run(conversion, request);
                CHECK(rejected.outcome == Outcome::Rejected && !rejected.text && !rejected.diagnostic.isEmpty());
                CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
                QFile untouched(repo.path("tracked.txt"));
                CHECK(untouched.open(QIODevice::ReadOnly) && untouched.readAll() == "before\n");
                untouched.close();
                checkConversionCleanup(repo);
            }
            for (const bool bigEndian : {false, true}) {
                std::cout << "REJECT ROUNDTRIP " << (bigEndian ? "UTF-16 byte order" : "autocrlf changes LF") << '\n';
                Repository repo;
                if (bigEndian) writeFile(repo.path(".gitattributes"), "tracked.txt text eol=lf working-tree-encoding=UTF-16\n");
                else {
                    repo.git({"config", "core.autocrlf", "true"});
                    repo.git({"config", "core.safecrlf", "false"});
                }
                const auto object = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
                const auto before = repo.git({"ls-files", "--stage", "-z"});
                TextConversion conversion(gitExecutable);
                auto request = textRequest(repo, TextOperation::PrepareResult, object);
                if (bigEndian) request.launchResultFormat = conversion_fixtures::beBomCrLf;
                request.editedUtf8 = "reviewed\n";
                const auto rejected = run(conversion, request);
                CHECK(rejected.outcome == Outcome::Rejected && !rejected.text);
                CHECK(rejected.diagnostic.contains("clean/checkout changes"));
                CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
                CHECK(repo.git({"show", ":tracked.txt"}) == "before\n");
                checkConversionCleanup(repo);
            }
        });
        suite.run("ident expansion round-trips exactly and rejects an expansion made stale by editing", [] {
            Repository repo;
            writeFile(repo.path(".gitattributes"), "tracked.txt text ident eol=lf\n");
            const auto object = repo.git({"hash-object", "-w", "--stdin"}, "$Id$\n").trimmed();
            TextConversion conversion(gitExecutable);
            const auto source = run(conversion, textRequest(repo, TextOperation::ReadSource, object));
            CHECK(source.outcome == Outcome::Success && source.text);
            CHECK(source.text->document.utf8 == "$Id: " + object + " $\n");
            CHECK(source.text->canonicalBytes == "$Id$\n");
            auto request = textRequest(repo, TextOperation::PrepareResult, object);
            request.launchResultFormat = source.text->document.format;
            request.editedUtf8 = source.text->document.utf8;
            const auto unchanged = run(conversion, request);
            CHECK(unchanged.outcome == Outcome::Success && unchanged.text);
            CHECK(unchanged.text->canonicalObjectId == object && unchanged.text->canonicalBytes == "$Id$\n");
            CHECK(unchanged.text->document.originalBytes == "$Id: " + object + " $\n");
            writeFile(repo.path("tracked.txt"), unchanged.text->document.originalBytes);
            GitAdapter adapter(gitExecutable);
            const auto staged = run(adapter, repo.request(Operation::StageFile, "tracked.txt"));
            CHECK(staged.outcome == Outcome::Success);
            CHECK(staged.indexAfter == "100644 " + object + " 0\ttracked.txt" + QByteArray(1, '\0'));
            auto verify = textRequest(repo, TextOperation::VerifyPublished, {});
            verify.expected = unchanged.text;
            const auto published = run(conversion, verify);
            CHECK(published.outcome == Outcome::Success && published.text);
            CHECK(published.text->document.utf8 == request.editedUtf8 && published.text->canonicalBytes == "$Id$\n");
            std::cout << "FORMAT ident expansion with staged canonical token\n";
            request.editedUtf8 += "changed\n";
            const auto stale = run(conversion, request);
            CHECK(stale.outcome == Outcome::Rejected && !stale.text);
            CHECK(stale.diagnostic.contains("clean/checkout changes"));
            CHECK(repo.git({"show", ":tracked.txt"}) == "$Id$\n");
            CHECK(repo.git({"ls-files", "--stage", "-z"}) == staged.indexAfter);
            checkConversionCleanup(repo);
        });
        suite.run("conversion requires exact unique stages and preserves agreed executable modes", [] {
            for (const auto &mode : {QByteArray("100644"), QByteArray("100755")}) {
                Repository repo;
                const auto object = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
                auto request = textRequest(repo, TextOperation::ReadSource, object);
                for (auto &entry : request.stages) entry.mode = mode;
                TextConversion conversion(gitExecutable);
                const auto source = run(conversion, request);
                CHECK(source.outcome == Outcome::Success && source.text);
                CHECK(source.text->mode == mode && source.text->sourceMode == mode);
                request.operation = TextOperation::PrepareResult;
                request.editedUtf8 = "executable result\n";
                const auto prepared = run(conversion, request);
                CHECK(prepared.outcome == Outcome::Success && prepared.text && prepared.text->mode == mode);
                writeFile(repo.path("tracked.txt"), prepared.text->document.originalBytes, mode == "100755");
                GitAdapter adapter(gitExecutable);
                CHECK(run(adapter, repo.request(Operation::StageFile, "tracked.txt")).outcome == Outcome::Success);
                auto verify = textRequest(repo, TextOperation::VerifyPublished, {});
                verify.expected = prepared.text;
                CHECK(run(conversion, verify).outcome == Outcome::Success);
                CHECK(repo.git({"ls-files", "--stage", "-z"}).startsWith(mode + ' '));
            }
            Repository repo;
            const auto object = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
            TextConversion conversion(gitExecutable);
            for (int scenario = 0; scenario < 8; ++scenario) {
                auto request = textRequest(repo, TextOperation::PrepareResult, object);
                request.editedUtf8 = "reviewed\n";
                switch (scenario) {
                case 0: request.stages.removeFirst(); break;
                case 1: request.stages.removeLast(); break;
                case 2: request.stages.append(request.stages.front()); break;
                case 3: request.stages.back().path = "other.txt"; break;
                case 4: request.stages.back().mode = "120000"; break;
                case 5: request.stages.back().mode = "100755"; break;
                case 6: request.stages.back().stage = 4; break;
                case 7: request.stages.back().objectId = "invalid"; break;
                }
                const auto rejected = run(conversion, request);
                CHECK(rejected.outcome == Outcome::Rejected && !rejected.text && !rejected.diagnostic.isEmpty());
            }
            const auto base = repo.git({"hash-object", "-w", "--stdin"}, "base\n").trimmed();
            auto request = textRequest(repo, TextOperation::ReadSource, object);
            request.objectId = base;
            CHECK(run(conversion, request).outcome == Outcome::Rejected);
            request.sourceStage = 1;
            CHECK(run(conversion, request).outcome == Outcome::Rejected);
            request.stages.prepend({request.path, "100755", base, 1});
            const auto earlier = run(conversion, request);
            CHECK(earlier.outcome == Outcome::Success && earlier.text);
            CHECK(earlier.text->sourceMode == "100755" && earlier.text->mode == "100644");
            CHECK(earlier.text->canonicalObjectId == base && earlier.text->document.utf8 == "base\n");
        });
        suite.run("publication verification rejects changed canonical identity, policy and prepared text", [] {
            Repository repo;
            const auto object = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
            TextConversion conversion(gitExecutable);
            auto request = textRequest(repo, TextOperation::PrepareResult, object);
            request.editedUtf8 = "reviewed\n";
            const auto prepared = run(conversion, request);
            CHECK(prepared.outcome == Outcome::Success && prepared.text);
            writeFile(repo.path("tracked.txt"), prepared.text->document.originalBytes);
            GitAdapter adapter(gitExecutable);
            CHECK(run(adapter, repo.request(Operation::StageFile, "tracked.txt")).outcome == Outcome::Success);
            auto verify = textRequest(repo, TextOperation::VerifyPublished, {});
            verify.expected = prepared.text;
            CHECK(run(conversion, verify).outcome == Outcome::Success);
            const auto wrong = repo.git({"hash-object", "-w", "--stdin"}, "different\n").trimmed();
            repo.git({"update-index", "--cacheinfo", "100644", QString::fromLatin1(wrong), "tracked.txt"});
            const auto wrongObject = run(conversion, verify);
            CHECK(wrongObject.outcome == Outcome::Rejected && !wrongObject.text);
            CHECK(wrongObject.diagnostic.contains("canonical identity"));
            repo.git({"update-index", "--cacheinfo", "100644",
                      QString::fromLatin1(prepared.text->canonicalObjectId), "tracked.txt"});
            writeFile(repo.path(".gitattributes"), "tracked.txt text eol=lf\n");
            const auto wrongPolicy = run(conversion, verify);
            CHECK(wrongPolicy.outcome == Outcome::Rejected && !wrongPolicy.text);
            CHECK(wrongPolicy.diagnostic.contains("conversion policy"));
            CHECK(QFile::remove(repo.path(".gitattributes")));
            verify.expected->document.utf8 += "unreviewed\n";
            const auto wrongText = run(conversion, verify);
            CHECK(wrongText.outcome == Outcome::Rejected && !wrongText.text);
            CHECK(wrongText.diagnostic.contains("no longer matches its serialized bytes"));
            verify.expected = prepared.text;
            verify.expected->worktreeRoot = repo.executables.path();
            const auto wrongWorktree = run(conversion, verify);
            CHECK(wrongWorktree.outcome == Outcome::Rejected && !wrongWorktree.text);
            CHECK(wrongWorktree.diagnostic.contains("worktree identity changed"));
        });
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
            TextConversion conversion(gitExecutable);
            const auto source = run(conversion, textRequest(repo, TextOperation::ReadSource, object));
            CHECK(source.outcome == Outcome::Success && source.text);
            CHECK(source.text->canonicalBytes == "before\n" && source.text->document.utf8 == "before\n");
            CHECK(source.text->canonicalObjectId == object && source.text->mode == "100644");
            auto prepare = textRequest(repo, TextOperation::PrepareResult, object);
            prepare.launchResultFormat = source.text->document.format;
            prepare.editedUtf8 = "conversion result\n";
            const auto prepared = run(conversion, prepare);
            CHECK(prepared.outcome == Outcome::Success && prepared.text);
            CHECK(prepared.text->document.originalBytes == prepare.editedUtf8);
            CHECK(prepared.text->canonicalBytes == prepare.editedUtf8);
            // Preparing text must not insert even an unreachable object in the repository.
            repo.git({"cat-file", "-e", QString::fromLatin1(prepared.text->canonicalObjectId)}, {}, 1);
            CHECK(repo.git({"show", ":tracked.txt"}) == "before\n");
            writeFile(repo.path("tracked.txt"), prepared.text->document.originalBytes);
            CHECK(run(adapter, repo.request(Operation::StageFile, "tracked.txt")).outcome == Outcome::Success);
            auto verify = textRequest(repo, TextOperation::VerifyPublished, {});
            verify.expected = prepared.text;
            CHECK(run(conversion, verify).outcome == Outcome::Success);
            repo.git({"update-index", "--chmod=+x", "--", "tracked.txt"});
            const auto wrongMode = run(conversion, verify);
            CHECK(wrongMode.outcome == Outcome::Rejected && wrongMode.diagnostic.contains("executable mode"));
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
            ConflictClassifier classifier(gitExecutable);
            const auto classified = run(classifier, {repo.root.path(), 71, 93});
            CHECK(classified.outcome == Outcome::Success && classified.conflicts.size() == names.size());
            for (const auto &conflict : classified.conflicts) {
                CHECK(names.contains(conflict.path) && conflict.stages.size() == 3 && !conflict.editable());
                CHECK(conflict.support == ConflictSupport::UnsupportedOperation && !conflict.reason.isEmpty());
            }
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
                for (const auto &kind : {"clean", "smudge", "process", "required"}) {
                    Repository repo;
                    const auto key = QByteArray("filter.") + driver + '.' + kind;
                    writeFile(repo.path(".gitattributes"), "tracked.txt filter=" + QByteArray(driver) + '\n');
                    const auto before = repo.git({"ls-files", "--stage", "-z"});
                    const auto blob = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
                    repo.git({"config", QString::fromLatin1(key),
                              QByteArray(kind) == "required" ? "true" : "touch .helper-ran; cat"});
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
                TextConversion conversion(gitExecutable);
                const auto decoded = run(conversion, textRequest(repo, TextOperation::ReadSource, blob));
                CHECK(decoded.outcome == Outcome::Success && decoded.text);
                CHECK(decoded.text->document.utf8 == "before\n" && decoded.text->document.originalBytes == "before\r\n");
                CHECK(decoded.text->policy.attributes.value("eol") == "crlf");
                CHECK(decoded.text->policy.configuration.value("filter.unused.clean") == "touch .helper-ran; cat");
                auto prepare = textRequest(repo, TextOperation::PrepareResult, blob);
                prepare.editedUtf8 = "after\n";
                const auto prepared = run(conversion, prepare);
                CHECK(prepared.outcome == Outcome::Success && prepared.text);
                CHECK(prepared.text->document.originalBytes == "after\r\n" && prepared.text->canonicalBytes == "after\n");
                CHECK(run(adapter, repo.request(Operation::Status)).outcome == Outcome::Success);
                CHECK(run(adapter, repo.request(Operation::CheckParentStaging, "tracked.txt")).outcome == Outcome::Success);
                writeFile(repo.path("tracked.txt"), "after\r\n");
                CHECK(run(adapter, repo.request(Operation::StageFile, "tracked.txt")).outcome == Outcome::Success);
                CHECK(repo.git({"show", ":tracked.txt"}) == "after\n");
                auto verify = textRequest(repo, TextOperation::VerifyPublished, {});
                verify.expected = prepared.text;
                CHECK(run(conversion, verify).outcome == Outcome::Success);
                // The same adapter must recheck attributes/configuration on every request.
                writeFile(repo.path(".gitattributes"), "/tracked.txt filter=unused\n");
                CHECK(run(adapter, source).outcome == Outcome::Rejected);
                const auto denied = run(conversion, textRequest(repo, TextOperation::ReadSource, blob));
                CHECK(denied.outcome == Outcome::Rejected && denied.diagnostic.contains("filter"));
                CHECK(run(conversion, prepare).outcome == Outcome::Rejected);
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
            {
                Repository repo;
                repo.git({"-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                          "commit", "--quiet", "-m", "base"});
                QTemporaryDir shared;
                CHECK(shared.isValid());
                const auto clone = shared.path() + "/shared";
                repo.git({"clone", "--shared", "--quiet", repo.root.path(), clone});
                const auto alternates = clone + "/.git/objects/info/alternates";
                const auto alternatePath = QFile::encodeName(repo.path(".git/objects")) + '\n';
                auto request = repo.request(Operation::InspectIndex, "tracked.txt");
                request.directory = clone;
                GitAdapter adapter(gitExecutable);
                CHECK(run(adapter, request).outcome == Outcome::Success);
                writeFile(alternates, "# shared with base\n" + alternatePath);
                CHECK(repo.git({"-C", clone, "cat-file", "-t", "HEAD"}) == "commit\n");
                CHECK(repo.git({"-C", clone, "ls-files", "--stage", "-z"})
                      == repo.git({"ls-files", "--stage", "-z"}));
                const auto blob = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
                for (const auto operation : {Operation::DiscoverRepository, Operation::ListUnmerged, Operation::InspectIndex,
                                             Operation::ReadObject, Operation::ReadAttributes, Operation::Status,
                                             Operation::MaterializeSource, Operation::CanonicalizeText,
                                             Operation::CheckParentStaging, Operation::StageFile}) {
                    const bool noPath = operation == Operation::DiscoverRepository || operation == Operation::ListUnmerged
                        || operation == Operation::ReadObject || operation == Operation::Status;
                    request.operation = operation;
                    request.path = noPath ? "" : "tracked.txt";
                    request.objectId = operation == Operation::ReadObject || operation == Operation::MaterializeSource
                        ? blob : QByteArray{};
                    request.input = operation == Operation::CanonicalizeText ? "before\n" : "";
                    const auto result = run(adapter, request);
                    CHECK(result.outcome == Outcome::Success && result.exitCode == 0);
                    if (operation == Operation::ReadObject || operation == Operation::MaterializeSource) {
                        CHECK(result.standardOutput == "before\n");
                    }
                    if (operation == Operation::InspectIndex) {
                        CHECK(result.indexEntries.size() == 1 && result.indexEntries.first().path == "tracked.txt");
                    }
                }
                // Comments cannot hide a real alternate that cannot be inspected.
                writeFile(alternates, "# shared with base\n" + alternatePath
                          + QFile::encodeName(shared.path() + "/missing-objects") + '\n');
                request = repo.request(Operation::InspectIndex, "tracked.txt");
                request.directory = clone;
                const auto denied = run(adapter, request);
                CHECK(denied.outcome == Outcome::Rejected);
                CHECK(denied.diagnostic.contains("object storage cannot be inspected offline"));
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
                    writeFile(repo.path(".git/objects/info/alternates"), "# shared with base\n"
                              + alternate.path(".git/objects").toUtf8() + '\n');
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
                    if (scenario >= 2) CHECK(result.diagnostic.contains("promisor object packs"));
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
        suite.run("status skips absent filters and streams large filter inspections", [] {
            {
                Repository repo;
                const auto fake = repo.fakeGit(": > .status-ran\n", ":\n", {},
                    ": > .attributes-ran\n/usr/bin/head -c 9000000 /dev/zero\n");
                GitAdapter adapter(fake);
                const auto result = run(adapter, repo.request(Operation::Status));
                CHECK(result.outcome == Outcome::Success && result.standardOutput.isEmpty());
                CHECK(!result.outputIncomplete && result.mutation == Mutation::NotAttempted);
                CHECK(QFile::exists(repo.path(".status-ran")) && !QFile::exists(repo.path(".attributes-ran")));
            }
            for (int scenario = 0; scenario < 3; ++scenario) {
                Repository repo;
                // 8,800,000 bytes of complete records exercise arbitrary chunk
                // boundaries and a policy listing larger than the payload cap.
                QByteArray attributes = R"SH(
/usr/bin/awk 'BEGIN { for (i = 0; i < 400000; ++i) printf "tracked.txt%ctext%cauto%c", 0, 0, 0 }'
)SH";
                if (scenario == 1) attributes += "printf 'tracked.txt\\0filter\\0unused\\0'\n";
                if (scenario == 2) attributes += "printf 'tracked.txt\\0filter\\0unused'\n";
                const auto fake = repo.fakeGit(": > .status-ran\n", ":\n",
                    "printf 'filter.unused.clean\\ncat\\0'\n", attributes);
                GitAdapter adapter(fake);
                const auto result = run(adapter, repo.request(Operation::Status));
                CHECK(result.outcome == (scenario == 0 ? Outcome::Success : Outcome::Rejected));
                CHECK(!result.outputIncomplete && result.mutation == Mutation::NotAttempted);
                CHECK(QFile::exists(repo.path(".status-ran")) == (scenario == 0));
                if (scenario == 0) CHECK(result.standardOutput.isEmpty());
                else CHECK(result.diagnostic.contains(scenario == 1 ? "custom filter 'unused'" : "invalid filter"));
            }
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
        suite.run("canonical reads retain the full output budget after preflight", [] {
            Repository repo;
            writeFile(repo.path(".gitattributes"), "tracked.txt text eol=lf\n");
            const auto objects = repo.git({"count-objects", "-v"});
            auto request = repo.request(Operation::CanonicalizeText, "tracked.txt");
            request.input = QByteArray(8 * 1024 * 1024, 'a');
            for (qsizetype i = 79; i < request.input.size(); i += 80) request.input[i] = '\n';
            const auto expected = repo.git({"hash-object", "--no-filters", "--stdin"}, request.input).trimmed();
            GitAdapter adapter(gitExecutable);
            const auto result = run(adapter, request);
            CHECK(result.outcome == Outcome::Success && !result.outputIncomplete);
            CHECK(result.standardOutput == request.input && result.canonicalBytes == request.input);
            CHECK(result.canonicalObjectId == expected && result.mutation == Mutation::NotAttempted);
            CHECK(repo.git({"count-objects", "-v"}) == objects);
            checkConversionCleanup(repo);
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
    if (arguments.size() == 2 && arguments[1] == "--classification-only")
        return GitAdapterTests{}.runClassificationTests();
    return GitAdapterTests{}.runTests();
}
