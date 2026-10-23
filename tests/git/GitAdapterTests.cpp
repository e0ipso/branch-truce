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
#include <vector>

using namespace branch_truce::git;

namespace {
constexpr auto gitExecutable = "/usr/bin/git";

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
    QString path(const QString &relative) const { return root.path() + '/' + relative; }
    QByteArray git(const QStringList &arguments, const QByteArray &input = {}) const
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
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            throw std::runtime_error("fixture git: " + process.readAllStandardError().toStdString());
        }
        return process.readAllStandardOutput();
    }
    Request request(Operation operation, const QString &file = {}) const
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
        -c) shift 2 ;;
        *) break ;;
    esac
done
case "$1" in
    --version) printf 'git version fixture\n' ;;
    config) exit 0 ;;
    rev-parse)
        case "$*" in
            *--git-path*) printf '%s/.git/hooks\n' "$PWD" ;;
            *) printf '%s/.git\n' "$PWD" ;;
        esac ;;
    ls-files)
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
                if (operation == Operation::DiscoverRepository) CHECK(result.standardOutput.contains(repo.root.path().toUtf8()));
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
            const auto base = repo.git({"hash-object", "-w", "--stdin"}, "base\n").trimmed();
            const auto ours = repo.git({"hash-object", "-w", "--stdin"}, "ours\n").trimmed();
            const auto theirs = repo.git({"hash-object", "-w", "--stdin"}, "theirs\n").trimmed();
            const QByteArray entries = "0 0000000000000000000000000000000000000000\ttracked.txt\n100644 " + base
                + " 1\ttracked.txt\n100644 " + ours + " 2\ttracked.txt\n100644 " + theirs + " 3\ttracked.txt\n";
            repo.git({"update-index", "--index-info"}, entries);
            GitAdapter adapter(gitExecutable);
            const auto result = run(adapter, repo.request(Operation::ListUnmerged));
            CHECK(result.outcome == Outcome::Success);
            CHECK(result.standardOutput == repo.git({"ls-files", "--unmerged", "-z"}));
            CHECK(result.standardOutput.count('\0') == 3);
            writeFile(repo.path("tracked.txt"), "resolved\n");
            const auto staged = run(adapter, repo.request(Operation::StageFile, "tracked.txt"));
            CHECK(staged.outcome == Outcome::Success && staged.mutation == Mutation::ObservedChanged);
            CHECK(staged.indexBefore.count('\0') == 3 && staged.indexAfter.count('\0') == 1);
            CHECK(repo.git({"ls-files", "--unmerged", "-z"}).isEmpty());
        });
        suite.run("staging treats colon, glob, option and whitespace names literally", [] {
            Repository repo;
            GitAdapter adapter(gitExecutable);
            const QStringList names{":leading.txt", ":(glob)*", "[ab].txt", "--all", "space name\twith\nnewline"};
            writeFile(repo.path("a.txt"), "must remain untracked\n");
            writeFile(repo.path("b.txt"), "must remain untracked\n");
            const auto untouched = repo.git({"ls-files", "--stage", "-z", "--", "tracked.txt"});
            for (const auto &name : names) {
                writeFile(repo.path(name), "literal content\n");
                const auto result = run(adapter, repo.request(Operation::StageFile, name));
                CHECK(result.outcome == Outcome::Success && result.exitCode == 0);
                CHECK(result.mutation == Mutation::ObservedChanged && result.indexBefore.isEmpty());
                CHECK(result.indexAfter == repo.git({"ls-files", "--stage", "-z", "--", name}));
                CHECK(result.indexAfter.endsWith(QByteArray("\t") + name.toUtf8() + QByteArray(1, '\0')));
                CHECK(repo.git({"show", ":" + name}) == "literal content\n");
            }
            CHECK(repo.git({"ls-files", "--stage", "-z", "--", "a.txt", "b.txt"}).isEmpty());
            CHECK(repo.git({"ls-files", "--stage", "-z", "--", "tracked.txt"}) == untouched);
            const auto unchanged = run(adapter, repo.request(Operation::StageFile, ":leading.txt"));
            CHECK(unchanged.outcome == Outcome::Success && unchanged.mutation == Mutation::ObservedUnchanged);
        });
        suite.run("invalid operands cannot reach Git or mutate the fixture", [] {
            Repository repo;
            GitAdapter adapter(gitExecutable);
            const auto before = repo.git({"ls-files", "--stage", "-z"});
            const QStringList paths{"", "/absolute", "../escape", "dir/../file", "./tracked.txt", "dir//file", "dir/",
                                    QString::fromLatin1("a\0b", 3), QString(QChar(0xd800))};
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
        suite.run("external helpers and executable hooks are rejected before mutation", [] {
            for (const auto &key : {"filter.fixture.clean", "filter.fixture.smudge", "filter.fixture.process",
                                    "core.fsmonitor", "remote.origin.promisor"}) {
                Repository repo;
                writeFile(repo.path(".gitattributes"), "* filter=fixture\n");
                const auto before = repo.git({"ls-files", "--stage", "-z"});
                const auto blob = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
                repo.git({"config", key, key == QByteArray("remote.origin.promisor") ? "true" : "touch .helper-ran"});
                GitAdapter adapter(gitExecutable);
                const auto result = run(adapter, repo.request(Operation::StageFile, "tracked.txt"));
                CHECK(result.outcome == Outcome::Rejected && result.mutation == Mutation::NotAttempted);
                CHECK(result.diagnostic.contains("Unsupported"));
                if (QByteArray(key).startsWith("filter.") || QByteArray(key).endsWith(".promisor")) {
                    auto source = repo.request(Operation::MaterializeSource, "tracked.txt");
                    source.objectId = blob;
                    const auto materialized = run(adapter, source);
                    CHECK(materialized.outcome == Outcome::Rejected && materialized.standardOutput.contains(key));
                    CHECK(materialized.mutation == Mutation::NotAttempted);
                }
                CHECK(!QFile::exists(repo.path(".helper-ran")));
                repo.git({"config", "--unset", key});
                CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
            }
            Repository repo;
            writeFile(repo.path(".git/hooks/pre-commit"), "#!/bin/sh\ntouch .hook-ran\n", true);
            GitAdapter adapter(gitExecutable);
            const auto result = run(adapter, repo.request(Operation::StageFile, "tracked.txt"));
            CHECK(result.outcome == Outcome::Rejected && result.diagnostic.contains("hooks"));
            CHECK(result.mutation == Mutation::NotAttempted && !QFile::exists(repo.path(".hook-ran")));
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
            Repository repo;
            const auto fake = repo.fakeGit(": > .after-add\nprintf 'before cancellation'\n: > .operation-started\nsleep 60\n");
            GitAdapter adapter(fake);
            Completion completion(adapter);
            QTimer cancel;
            QObject::connect(&cancel, &QTimer::timeout, &cancel, [&] {
                if (QFile::exists(repo.path(".operation-started"))) {
                    cancel.stop();
                    adapter.cancel();
                }
            });
            cancel.start(5);
            const auto result = completion.run(repo.request(Operation::StageFile, "tracked.txt"));
            CHECK(result.outcome == Outcome::Cancelled && result.mutation == Mutation::ObservedChanged);
            CHECK(result.standardOutput == "before cancellation" && !result.indexAfter.isEmpty());
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
    return GitAdapterTests{}.runTests();
}
