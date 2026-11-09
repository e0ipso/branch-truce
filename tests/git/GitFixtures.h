#pragma once

// What the Git adapter, conflict classifier and text conversion suites share: a repository with
// one tracked file, scripted stand-ins for Git, merge and rebase histories, and one-request
// drivers for the three services.

#include "TestHarness.h"
#include "fixtures/EventLoop.h"
#include "fixtures/Files.h"
#include "fixtures/GitRepository.h"
#include "git/ConflictClassifier.h"
#include "git/GitAdapter.h"
#include "git/TextConversion.h"

#include <QByteArray>
#include <QDir>
#include <QFileSystemWatcher>
#include <QList>
#include <QString>
#include <QTemporaryDir>

#include <fcntl.h>
#include <functional>
#include <sys/stat.h>
#include <unistd.h>

namespace git_fixture {
using namespace branch_truce::git;
using test_support::readFile;
using test_support::writeFile;

// Filenames that a pathspec, an option parser or a UTF-8 decoder would misread.
inline QList<QByteArray> oddPaths()
{
    return {":leading.txt",
            ":(glob)*",
            ":!tracked.txt",
            "[ab].txt",
            "--all",
            "-",
            " space name\twith\nnewline ",
            "quote\"and\\backslash",
            QByteArray::fromHex("636166c3a92e747874"),
            QByteArray("odd-") + char(0xff) + ".txt",
            QByteArray("odd-") + char(0xfe) + ".txt",
            QByteArray("nested/odd-") + char(0xff) + ".txt"};
}

// A repository on main with tracked.txt staged as "before\n", and a private directory for
// scripted Git executables.
class Repository : public test_support::GitRepository {
public:
    explicit Repository(const QStringList &initArguments = {}) : GitRepository(initArguments)
    {
        CHECK(executables_.isValid());
        writeFile(path("tracked.txt"), "before\n");
        git({"add", "--", "tracked.txt"});
    }
    Request request(Operation operation, const QByteArray &file = {}) const
    {
        return {operation, root(), file, {}, 71, 93};
    }
    QString executables() const { return executables_.path(); }
    // Installs `script` as the private executable git-fixture and returns its path.
    QString executable(const QByteArray &script) const
    {
        const auto selected = executables() + "/git-fixture";
        writeFile(selected, script, true);
        return selected;
    }
    // A scripted Git for the adapter's failure paths. It answers preflight and inspection the way
    // a one-file repository would, runs `operation` for add, status and cat-file, and exits 97
    // for anything else. Literal path and index handling is covered against real Git instead.
    QString fakeGit(
        const QByteArray &operation,
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
    // Removes the worktree and the scripted executables. Returns whether both are gone.
    bool removeAll()
    {
        const auto scripts = executables();
        return remove() && executables_.remove() && !QFileInfo::exists(scripts);
    }

private:
    QTemporaryDir executables_;
};

// A gate a scripted Git blocks on with `read -r line < <name>` until the case opens it. The
// case holds the FIFO open for writing, so the script never sees end of file early and opening
// it never blocks. open() writes enough lines that every later read passes too.
class ScriptGate {
public:
    explicit ScriptGate(const QString &path)
    {
        const auto native = QFile::encodeName(path);
        CHECK(::mkfifo(native.constData(), 0600) == 0);
        fd_ = ::open(native.constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        CHECK(fd_ >= 0);
    }
    ~ScriptGate()
    {
        if (fd_ >= 0) ::close(fd_);
    }
    ScriptGate(const ScriptGate &) = delete;
    ScriptGate &operator=(const ScriptGate &) = delete;
    void open()
    {
        const QByteArray lines(256, '\n');
        EXPECT(::write(fd_, lines.constData(), static_cast<std::size_t>(lines.size())) == lines.size());
    }

private:
    int fd_ = -1;
};

// Calls `action` once, from the event loop, when `path` first exists. The directory is watched
// through inotify, so a script's marker file is seen without polling.
class WhenCreated {
public:
    WhenCreated(const QString &path, std::function<void()> action) : path_(path), action_(std::move(action))
    {
        CHECK(watcher_.addPath(QFileInfo(path).absolutePath()));
        QObject::connect(&watcher_, &QFileSystemWatcher::directoryChanged, &watcher_, [this] { fire(); });
        fire();
    }

private:
    void fire()
    {
        if (done_ || !QFileInfo::exists(path_)) return;
        done_ = true;
        action_();
    }
    QString path_;
    std::function<void()> action_;
    QFileSystemWatcher watcher_;
    bool done_ = false;
};

// The adapter's results, with run() also checking that the result carries the request identity.
class Completion : public test_support::AsyncCompletion<GitAdapter, Result> {
public:
    explicit Completion(GitAdapter &adapter) : AsyncCompletion(adapter, &GitAdapter::finished) {}
    Result run(const Request &request, int deadlineMs = test_support::kRequestDeadlineMs)
    {
        const auto result = AsyncCompletion::run(request, deadlineMs);
        CHECK(result.request.operation == request.operation && result.request.directory == request.directory);
        CHECK(result.request.path == request.path && result.request.objectId == request.objectId);
        CHECK(result.request.sessionGeneration == 71 && result.request.inputRevision == 93);
        return result;
    }
};

// One request through each service, checked against the service contract.
inline Result run(GitAdapter &adapter, const Request &request)
{
    Completion completion(adapter);
    return completion.run(request);
}

inline TextConversionResult run(TextConversion &conversion, const TextConversionRequest &request)
{
    test_support::AsyncCompletion completion(conversion, &TextConversion::finished);
    const auto result = completion.run(request);
    CHECK_EQ(result.request.sessionGeneration, request.sessionGeneration);
    CHECK_EQ(result.request.inputRevision, request.inputRevision);
    return result;
}

inline ConflictClassificationResult run(ConflictClassifier &classifier, const ConflictClassificationRequest &request)
{
    test_support::AsyncCompletion completion(classifier, &ConflictClassifier::finished);
    const auto result = completion.run(request);
    CHECK_EQ(result.request.sessionGeneration, request.sessionGeneration);
    CHECK_EQ(result.request.inputRevision, request.inputRevision);
    return result;
}

inline QByteArray commitFixture(const Repository &repo, const QString &message)
{
    repo.git({"-c", "user.name=Branch Truce Tests", "-c", "user.email=tests@example.invalid", "commit", "--quiet",
              "-am", message});
    return repo.git({"rev-parse", "HEAD"}).trimmed();
}

// Fifty numbered lines with `tenth` as line ten, so a rename keeps enough content to be detected.
inline QByteArray numberedText(const QByteArray &tenth)
{
    QByteArray bytes;
    for (int line = 1; line <= 50; ++line) bytes += (line == 10 ? tenth : QByteArray::number(line)) + '\n';
    return bytes;
}

struct ConflictHistory {
    QByteArray base;
    QByteArray main;
    QByteArray side;
};

struct ConflictContents {
    QByteArray base = "base\n";
    QByteArray main = "main\n";
    QByteArray side = "side\n";
};

// Commits base, then side and main branches that both change tracked.txt, and leaves main
// checked out. With `renamed`, main also renames a.txt to b.txt while side edits a.txt.
inline ConflictHistory conflictHistory(Repository &repo, bool renamed = false, const ConflictContents &contents = {})
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

// Discovery verified `operation` with the history's commits as its sources.
inline void checkOperationIdentity(const Result &result, const ConflictHistory &history, RepositoryOperation operation,
                                   const QByteArray &currentHead = {}, const QByteArray &earlier = {})
{
    const auto expectedHead = currentHead.isEmpty() ? history.main : currentHead;
    CHECK(result.outcome == Outcome::Success && result.exitCode == 0);
    CHECK(result.mutation == Mutation::NotAttempted && !result.outputIncomplete);
    CHECK(result.repository.supported && result.repository.operation == operation);
    CHECK(result.repository.unsupportedPaths.isEmpty() && result.repository.diagnostic.isEmpty());
    CHECK(result.repository.stage2.commitId == expectedHead && result.repository.stage3.commitId == history.side);
    if (expectedHead == history.main) CHECK(result.repository.stage2.name == QStringLiteral("main"));
    CHECK(result.repository.stage3.name == QStringLiteral("side"));
    CHECK(result.repository.baseCommitId == (earlier.isEmpty() ? history.base : earlier));
    CHECK(!result.repository.stage2.label.contains(QString::fromLatin1(expectedHead)));
    CHECK(!result.repository.stage3.label.contains(QString::fromLatin1(history.side)));
    if (operation == RepositoryOperation::Rebase) {
        CHECK(result.repository.stage2.label.startsWith("Current rebased state"));
        CHECK(result.repository.stage3.label.startsWith("Commit being replayed"));
        CHECK(result.repository.headName == "refs/heads/side");
        CHECK(result.repository.originalHead == history.side && result.repository.onto == history.main);
    } else {
        CHECK(result.repository.stage2.label.startsWith("Current checkout"));
        CHECK(result.repository.stage3.label.startsWith("Merged commit"));
        CHECK(result.repository.headName.isEmpty() && result.repository.originalHead.isEmpty()
              && result.repository.onto.isEmpty());
    }
}

// Discovery reports `reason` for an operation it cannot attribute, keeps the conflicted index
// entries visible and labels no source.
inline void checkUnsupportedDiscovery(Repository &repo, const QString &reason,
                                      Outcome outcome = Outcome::InspectionFailure)
{
    const auto before = repo.git({"ls-files", "--unmerged", "-z"});
    GitAdapter adapter(test_support::gitExecutable());
    const auto result = run(adapter, repo.request(Operation::DiscoverRepository));
    CHECK(result.outcome == outcome && !result.repository.supported);
    CHECK(result.repository.operation == RepositoryOperation::Unsupported);
    CHECK(result.repository.diagnostic.contains(reason));
    CHECK(result.repository.stage2.commitId.isEmpty() && result.repository.stage3.commitId.isEmpty());
    CHECK(result.indexEntries.size() == before.count('\0') && !result.indexEntries.isEmpty());
    CHECK(result.mutation == Mutation::NotAttempted);
    CHECK(repo.git({"ls-files", "--unmerged", "-z"}) == before);
}

inline TextConversionRequest textRequest(const Repository &repo, TextOperation operation, const QByteArray &object)
{
    TextConversionRequest request;
    request.operation = operation;
    request.directory = repo.root();
    request.path = "tracked.txt";
    request.objectId = object;
    request.sessionGeneration = 71;
    request.inputRevision = 93;
    request.stages = {{request.path, "100644", object, 2}, {request.path, "100644", object, 3}};
    return request;
}

// The service left nothing behind that keeps the fixture from being removed.
inline void checkConversionCleanup(Repository &repo) { CHECK(repo.removeAll()); }

} // namespace git_fixture
