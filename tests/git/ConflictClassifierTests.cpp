// ConflictClassifier against real Git: which conflicts it offers for editing, why it refuses the
// others, and that classifying never changes the repository.
#include "TestHarness.h"
#include "core/ResourceLimits.h"
#include "fixtures/mergetool/GitGate.h"
#include "git/GitFixtures.h"

#include <QCoreApplication>
#include <QSet>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace git_fixture;

namespace {
// A merge of conflictHistory() stopped on tracked.txt, with a manual result.
ConflictHistory mergeStopped(Repository &repo)
{
    const auto history = conflictHistory(repo);
    repo.git({"merge", "--no-edit", "side"}, {}, 1);
    writeFile(repo.path("tracked.txt"), "manual result\n");
    return history;
}

// Holds the classifier's first source read: its Git waits on `check-attr ... -- tracked.txt`.
void waitForHeldSourceRead(const mergetool_fixture::GitGate &gate)
{
    test_support::waitUntil([&] { return !gate.held().isEmpty(); }, "no source read reached Git");
}
void adjustClassificationEntries(Repository &repo, const QByteArray &scenario, const ConflictHistory &history,
                                 QList<branch_truce::git::IndexEntry> &entries)
{
    for (auto &entry : entries) {
        if (entry.stage == 3 && scenario == "mode-disagreement") entry.mode = "100755";
        if ((entry.stage == 3 && scenario == "symlink-source") || (entry.stage == 1 && scenario == "symlink-base"))
            entry.mode = "120000";
        if ((entry.stage == 3 && scenario == "submodule-source")
            || (entry.stage == 1 && scenario == "submodule-base")) {
            entry.mode = "160000";
            entry.objectId = history.side;
        }
        if (entry.stage == 3 && scenario == "unverified-source")
            entry.objectId = repo.git({"hash-object", "-w", "--stdin"}, "not in either source\n").trimmed();
    }
}

void runClassificationScenario(const QByteArray &scenario, ConflictSupport expected)
{
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

    GitAdapter adapter(repo.program());
    auto entries = run(adapter, repo.request(Operation::DiscoverRepository)).indexEntries;
    adjustClassificationEntries(repo, scenario, history, entries);
    QByteArray index = "0 " + QByteArray(40, '0') + "\ttracked.txt" + QByteArray(1, '\0');
    for (const auto &entry : entries) {
        if ((scenario == "missing-ours" && entry.stage == 2) || (scenario == "missing-theirs" && entry.stage == 3))
            continue;
        index += entry.mode + ' ' + entry.objectId + ' ' + QByteArray::number(entry.stage) + "\ttracked.txt"
            + QByteArray(1, '\0');
    }
    repo.git({"update-index", "-z", "--index-info"}, index);
    if (scenario == "invalid-result") writeFile(repo.path("tracked.txt"), QByteArray(1, char(0xff)));
    if (scenario == "missing-result" || scenario == "directory-result" || scenario == "fifo-result"
        || scenario == "symlink-result") {
        CHECK(QFile::remove(repo.path("tracked.txt")));
        if (scenario == "directory-result") CHECK(QDir(repo.root()).mkdir("tracked.txt"));
        if (scenario == "fifo-result")
            CHECK(::mkfifo(QFile::encodeName(repo.path("tracked.txt")).constData(), 0600) == 0);
        if (scenario == "symlink-result") {
            writeFile(repo.path("outside.txt"), "do not follow\n");
            CHECK(::symlink("outside.txt", QFile::encodeName(repo.path("tracked.txt")).constData()) == 0);
        }
    }
    if (scenario == "oversized-result")
        writeFile(repo.path("tracked.txt"),
                  QByteArray("x\n").repeated(static_cast<qsizetype>(branch_truce::kMaxRecoveryPayloadBytes) / 2 + 1));
    if (scenario == "overlong-line-result")
        writeFile(repo.path("tracked.txt"),
                  QByteArray(static_cast<qsizetype>(branch_truce::kMaxLineBytes + 1), 'a') + '\n');
    if (scenario == "excluded-attributes") writeFile(repo.path(".gitattributes"), "tracked.txt -text eol=crlf\n");
    if (scenario == "filter") {
        writeFile(repo.path(".gitattributes"), "tracked.txt filter=external\n");
        repo.git({"config", "filter.external.clean", "touch .filter-ran; cat"});
    }
    const auto before = repo.git({"ls-files", "--stage", "-z"});
    const auto objects = repo.git({"count-objects", "-v"});
    ConflictClassifier classifier(repo.program());
    const auto result = run(classifier, {repo.root(), 71, 93});
    CHECK(result.outcome == Outcome::Success && result.repository.supported && result.conflicts.size() == 1);
    const auto &conflict = result.conflicts.front();
    CHECK(conflict.path == "tracked.txt" && conflict.support == expected);
    CHECK(conflict.editable() == (expected == ConflictSupport::SupportedText));
    if (conflict.editable()) {
        CHECK(conflict.reason.isEmpty() && conflict.sources.size() == 3 && conflict.result);
        if (!conflict.result) throw test_support::Failure("an editable conflict has no result");
        CHECK(conflict.result->utf8 == manual && conflict.result->originalBytes == manual);
        CHECK(conflict.mode == (scenario == "executable" ? "100755" : "100644"));
    } else {
        CHECK(!conflict.reason.isEmpty() && conflict.sources.isEmpty() && !conflict.result);
        if (scenario.endsWith("-result") && expected == ConflictSupport::LoadFailure && scenario != "missing-result")
            CHECK(conflict.reason.code == ProblemCode::ClassificationResultUnreadable
                  || conflict.reason.code == ProblemCode::ClassificationLineTooLong
                  || conflict.reason.code == ProblemCode::ClassificationTextTooLarge);
    }
    CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
    CHECK(repo.git({"count-objects", "-v"}) == objects);
    CHECK(repo.git({"rev-parse", "HEAD"}).trimmed() == history.main);
    CHECK(!QFile::exists(repo.path(".filter-ran")));
    if (scenario == "symlink-result") {
        QFile outside(repo.path("outside.txt"));
        CHECK(outside.open(QIODevice::ReadOnly) && outside.readAll() == "do not follow\n");
    }
}
void addClassificationMatrixCases(test_support::Suite &suite)
{
    for (const auto &[scenario, expected] : std::vector<std::pair<QByteArray, ConflictSupport>>{
             {"regular", ConflictSupport::SupportedText},
             {"executable", ConflictSupport::SupportedText},
             {"missing-ours", ConflictSupport::MissingSide},
             {"missing-theirs", ConflictSupport::MissingSide},
             {"mode-disagreement", ConflictSupport::ModeConflict},
             {"symlink-source", ConflictSupport::UnsupportedType},
             {"submodule-source", ConflictSupport::UnsupportedType},
             {"binary-source", ConflictSupport::UnsupportedText},
             {"symlink-base", ConflictSupport::UnsupportedType},
             {"submodule-base", ConflictSupport::UnsupportedType},
             {"invalid-utf8-source", ConflictSupport::UnsupportedText},
             {"mixed-newlines-source", ConflictSupport::UnsupportedText},
             {"invalid-result", ConflictSupport::UnsupportedText},
             {"missing-result", ConflictSupport::LoadFailure},
             {"directory-result", ConflictSupport::TopologyConflict},
             {"fifo-result", ConflictSupport::TopologyConflict},
             {"symlink-result", ConflictSupport::TopologyConflict},
             {"excluded-attributes", ConflictSupport::UnsupportedConversion},
             {"filter", ConflictSupport::UnsupportedConversion},
             {"unverified-source", ConflictSupport::UnverifiedSource},
             {"oversized-result", ConflictSupport::LoadFailure},
             {"overlong-line-result", ConflictSupport::LoadFailure}}) {
        suite.run("complete-conflict classification matrix: " + scenario.toStdString(),
                  [=] { runClassificationScenario(scenario, expected); });
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
        ConflictClassifier classifier(repo.program());
        const auto result = run(classifier, {repo.root(), 71, 93});
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
        CHECK(QDir(repo.root()).mkdir("nested"));
        auto names = oddPaths();
        names.prepend("tracked.txt");
        for (const auto &name : names) repo.writePath(name, "base\n");
        repo.git({"add", "--", "."});
        commitFixture(repo, "literal base");
        repo.git({"checkout", "--quiet", "-b", "side"});
        for (const auto &name : names)
            repo.writePath(name, name == names.last() ? QByteArray(1, char(0xff)) : QByteArray("side\n"));
        commitFixture(repo, "literal side");
        repo.git({"checkout", "--quiet", "main"});
        for (const auto &name : names) repo.writePath(name, "main\n");
        commitFixture(repo, "literal main");
        repo.git({"merge", "--no-edit", "side"}, {}, 1);
        for (const auto &name : names) repo.writePath(name, "manual UTF-8 \xc3\xa9\n");
        const auto before = repo.git({"ls-files", "--stage", "-z"});
        ConflictClassifier classifier(repo.program());
        const auto result = run(classifier, {repo.root(), 71, 93});
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
                CHECK(conflict.editable() && conflict.reason.isEmpty() && conflict.result
                      && conflict.sources.size() == 3);
                CHECK(conflict.result->originalBytes == "manual UTF-8 \xc3\xa9\n");
            }
        }
        CHECK(seen.size() == names.size() && repo.git({"ls-files", "--stage", "-z"}) == before);
    });
}

void addRebaseProvenanceCases(test_support::Suite &suite)
{
    suite.run("both rebase backends verify replayed history in regular and linked worktrees", [] {
        for (const auto backend : {"--merge", "--apply"}) {
            for (const bool linked : {false, true}) {
                SCOPE(std::string(backend) + (linked ? " in a linked worktree" : " in the main worktree"));
                Repository repo;
                auto history = conflictHistory(repo);
                repo.git({"checkout", "--quiet", "side"});
                // Replay a clean commit before reaching the conflicting commit.
                // Stage 2 must identify the new HEAD, rather than the onto commit.
                repo.git({"reset", "--hard", QString::fromLatin1(history.base)});
                writeFile(repo.path("already-replayed.txt"), "earlier side change\n");
                repo.git({"add", "--", "already-replayed.txt"});
                const auto earlier = commitFixture(repo, "earlier side change");
                repo.git({"cherry-pick", QString::fromLatin1(history.side)});
                history.side = repo.git({"rev-parse", "HEAD"}).trimmed();
                repo.git({"checkout", "--quiet", "main"});
                const auto originalIndex = repo.git({"ls-files", "--stage", "-z"});
                QTemporaryDir linkedParent;
                CHECK(linkedParent.isValid());
                const auto directory = linked ? linkedParent.path() + "/checkout" : repo.root();
                if (linked)
                    repo.git({"worktree", "add", "--quiet", directory, "side"});
                else
                    repo.git({"checkout", "--quiet", "side"});
                repo.git({"-C", directory, "rebase", backend, "main"}, {}, 1);
                const auto currentHead = repo.git({"-C", directory, "rev-parse", "HEAD"}).trimmed();
                CHECK(currentHead != history.main && currentHead != history.side);
                CHECK(repo.git({"-C", directory, "rev-parse", "HEAD^"}).trimmed() == history.main);
                CHECK(repo.git({"-C", directory, "show", "HEAD:already-replayed.txt"}) == "earlier side change\n");
                CHECK(QDir().mkpath(directory + "/nested"));
                auto request = repo.request(Operation::DiscoverRepository);
                request.directory = directory + "/nested";
                GitAdapter adapter(repo.program());
                const auto before = repo.git({"-C", directory, "ls-files", "--stage", "-z"});
                const auto result = run(adapter, request);
                checkOperationIdentity(result, history, RepositoryOperation::Rebase, currentHead, earlier);
                CHECK(result.indexEntries.size() == 3 && result.repository.worktreeRoot == directory);
                const auto gitDirectory =
                    QString::fromUtf8(repo.git({"-C", directory, "rev-parse", "--absolute-git-dir"}).trimmed());
                CHECK(result.repository.gitDirectory == QFileInfo(gitDirectory).canonicalFilePath());
                for (const auto &entry : result.indexEntries) {
                    const auto sourceCommit = entry.stage == 1 ? earlier
                        : entry.stage == 2                     ? currentHead
                                                               : history.side;
                    CHECK(entry.path == "tracked.txt" && entry.mode == "100644");
                    CHECK(entry.objectId
                          == repo.git({"rev-parse", QString::fromLatin1(sourceCommit) + ":tracked.txt"}).trimmed());
                }
                ConflictClassifier classifier(repo.program());
                const auto classified = run(classifier, {request.directory, 71, 93});
                CHECK(classified.outcome == Outcome::Success && classified.conflicts.size() == 1);
                CHECK(classified.conflicts.front().editable() && classified.conflicts.front().sources.size() == 3);
                CHECK(repo.git({"-C", directory, "ls-files", "--stage", "-z"}) == before);
                if (linked) {
                    CHECK(result.repository.gitDirectory != repo.path(".git"));
                    const auto original = run(adapter, repo.request(Operation::DiscoverRepository));
                    CHECK(original.outcome == Outcome::Success
                          && original.repository.operation == RepositoryOperation::None);
                    CHECK(original.indexEntries.isEmpty() && repo.git({"ls-files", "--stage", "-z"}) == originalIndex);
                    CHECK(linkedParent.remove() && !QFileInfo::exists(directory));
                }
                checkConversionCleanup(repo);
            }
        }
    });
}

void addClassificationFailureCases(test_support::Suite &suite)
{
    suite.run("real cherry-pick revert and mailbox conflicts remain outside merge and rebase support", [] {
        for (const auto operation : {"cherry-pick", "revert", "am"}) {
            SCOPE(operation);
            Repository repo;
            const auto history = conflictHistory(repo);
            const auto mailbox = QByteArray(operation) == "am";
            if (mailbox) {
                const auto patch = repo.git({"format-patch", "--stdout", "-1", QString::fromLatin1(history.side)});
                repo.git({"am", "--3way"}, patch, 128);
            } else
                repo.git({operation, "--no-edit", QString::fromLatin1(history.side)}, {}, 1);
            checkUnsupportedDiscovery(
                repo, mailbox ? ProblemCode::DiscoveryApplyMailbox : ProblemCode::DiscoverySequencerUnsupported,
                Outcome::Success);
            const auto before = repo.git({"ls-files", "--stage", "-z"});
            ConflictClassifier classifier(repo.program());
            const auto result = run(classifier, {repo.root(), 71, 93});
            CHECK(result.outcome == Outcome::Success && result.conflicts.size() == 1);
            const auto &conflict = result.conflicts.front();
            CHECK(conflict.path == "tracked.txt" && conflict.support == ConflictSupport::UnsupportedOperation);
            CHECK(!conflict.editable() && !conflict.reason.isEmpty() && !conflict.result && conflict.sources.isEmpty());
            CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
            CHECK(repo.git({"rev-parse", "HEAD"}).trimmed() == history.main);
            checkConversionCleanup(repo);
        }
    });
    suite.run("a cancel before the queued start completes once as cancelled", [] {
        Repository repo;
        mergeStopped(repo);
        ConflictClassifier classifier(repo.program());
        test_support::AsyncCompletion completion(classifier, &ConflictClassifier::finished);
        CHECK(classifier.start({repo.root(), 71, 93}));
        classifier.cancel();
        classifier.cancel();
        const auto result = completion.wait();
        CHECK_EQ(result.outcome, Outcome::Cancelled);
        CHECK(result.problem.code == ProblemCode::ClassificationCancelled);
        CHECK(result.conflicts.isEmpty());
        classifier.cancel();
        CHECK(!classifier.busy());
    });
    suite.run("a cancel while a source is read completes once as cancelled and offers nothing", [] {
        Repository repo;
        mergeStopped(repo);
        const auto before = repo.index();
        mergetool_fixture::GitGate gate("tracked.txt");
        gate.close(QStringLiteral("check-attr"));
        ConflictClassifier classifier(repo.program(gate.executable()));
        test_support::AsyncCompletion completion(classifier, &ConflictClassifier::finished);
        CHECK(classifier.start({repo.root(), 71, 93}));
        waitForHeldSourceRead(gate);
        classifier.cancel();
        gate.open();
        gate.releaseAll();
        const auto result = completion.wait();
        CHECK_EQ(result.outcome, Outcome::Cancelled);
        for (const auto &conflict : result.conflicts) CHECK(!conflict.editable());
        CHECK_EQ(repo.index(), before);
    });
    suite.run("an index change while sources are read fails the recheck", [] {
        Repository repo;
        mergeStopped(repo);
        mergetool_fixture::GitGate gate("tracked.txt");
        gate.close(QStringLiteral("check-attr"));
        ConflictClassifier classifier(repo.program(gate.executable()));
        test_support::AsyncCompletion completion(classifier, &ConflictClassifier::finished);
        CHECK(classifier.start({repo.root(), 71, 93}));
        waitForHeldSourceRead(gate);
        // As a user running `git add` while the app classifies.
        repo.git({"add", "--", "tracked.txt"});
        gate.open();
        gate.releaseAll();
        const auto result = completion.wait();
        CHECK_EQ(result.outcome, Outcome::InspectionFailure);
        CHECK(result.problem.code == ProblemCode::ClassificationChanged);
        for (const auto &conflict : result.conflicts) CHECK(!conflict.editable());
    });
    suite.run("a result rewritten while sources are read is excluded by the recheck", [] {
        Repository repo;
        mergeStopped(repo);
        mergetool_fixture::GitGate gate("tracked.txt");
        gate.close(QStringLiteral("check-attr"));
        ConflictClassifier classifier(repo.program(gate.executable()));
        test_support::AsyncCompletion completion(classifier, &ConflictClassifier::finished);
        CHECK(classifier.start({repo.root(), 71, 93}));
        waitForHeldSourceRead(gate);
        writeFile(repo.path("tracked.txt"), "written by another editor\n");
        gate.open();
        gate.releaseAll();
        const auto result = completion.wait();
        CHECK_EQ(result.outcome, Outcome::Success);
        CHECK_EQ(result.conflicts.size(), 1);
        const auto &conflict = result.conflicts.front();
        CHECK(!conflict.editable());
        CHECK_EQ(conflict.support, ConflictSupport::TopologyConflict);
        CHECK(conflict.reason.code == ProblemCode::ClassificationResultChanged);
    });
    suite.run("destroying the classifier with a held Git call publishes no result", [] {
        Repository repo;
        mergeStopped(repo);
        const auto before = repo.index();
        mergetool_fixture::GitGate gate("tracked.txt");
        gate.close(QStringLiteral("check-attr"));
        QObject receiver;
        int completions = 0;
        auto classifier = std::make_unique<ConflictClassifier>(repo.program(gate.executable()));
        QObject::connect(classifier.get(), &ConflictClassifier::finished, &receiver,
                         [&](const ConflictClassificationResult &) { ++completions; });
        CHECK(classifier->start({repo.root(), 71, 93}));
        waitForHeldSourceRead(gate);
        classifier.reset();
        gate.open();
        gate.releaseAll();
        // A later classification's result is queued behind anything the destroyed one posted.
        ConflictClassifier later(repo.program());
        CHECK_EQ(run(later, {repo.root(), 71, 93}).outcome, Outcome::Success);
        CHECK_EQ(completions, 0);
        CHECK_EQ(repo.index(), before);
    });
    suite.run("a conversion configuration change before the recheck excludes the conflict", [] {
        Repository repo;
        mergeStopped(repo);
        int discoveries = 0;
        // The second discovery is the recheck after every source was read.
        ConflictClassifier classifier(
            test_support::ScriptedGit::runners(repo.program(), [&](const Request &request) -> std::optional<Result> {
                if (request.operation == Operation::DiscoverRepository && ++discoveries == 2)
                    repo.git({"config", "core.autocrlf", "true"});
                return std::nullopt;
            }));
        const auto result = run(classifier, {repo.root(), 71, 93});
        CHECK_EQ(discoveries, 2);
        CHECK_EQ(result.outcome, Outcome::Success);
        CHECK_EQ(result.conflicts.size(), 1);
        const auto &conflict = result.conflicts.front();
        CHECK(!conflict.editable());
        CHECK_EQ(conflict.support, ConflictSupport::UnsupportedConversion);
        CHECK(conflict.reason.code == ProblemCode::ClassificationConfigurationChanged);
    });
    suite.run("a source over the Git read limit is a load failure that names the limit", [] {
        Repository repo;
        mergeStopped(repo);
        ConflictClassifier classifier(
            test_support::ScriptedGit::runners(repo.program(), [](const Request &request) -> std::optional<Result> {
                if (request.operation != Operation::MaterializeSource) return std::nullopt;
                Result limited;
                limited.outcome = Outcome::OutputLimit;
                limited.outputIncomplete = true;
                limited.problem = {ProblemCode::GitOutputLimit, {QStringLiteral("1")}};
                return limited;
            }));
        const auto result = run(classifier, {repo.root(), 71, 93});
        CHECK_EQ(result.outcome, Outcome::Success);
        CHECK_EQ(result.conflicts.size(), 1);
        const auto &conflict = result.conflicts.front();
        CHECK_EQ(conflict.support, ConflictSupport::LoadFailure);
        CHECK(conflict.reason.code == ProblemCode::ClassificationStageTooLarge);
        CHECK(conflict.reason.arguments.value(1)
              == QString::number(static_cast<qulonglong>(branch_truce::kMaxGitReadBytes)));
    });
    suite.run("a discovery that times out offers nothing", [] {
        Repository repo;
        mergeStopped(repo);
        ConflictClassifier classifier(
            test_support::ScriptedGit::runners(repo.program(), [](const Request &request) -> std::optional<Result> {
                if (request.operation != Operation::DiscoverRepository) return std::nullopt;
                Result late;
                late.outcome = Outcome::TimedOut;
                late.problem = {ProblemCode::GitDeadline, {QStringLiteral("30")}};
                return late;
            }));
        const auto result = run(classifier, {repo.root(), 71, 93});
        CHECK_EQ(result.outcome, Outcome::TimedOut);
        CHECK(hasProblem(result.problem, ProblemCode::GitDeadline));
        for (const auto &conflict : result.conflicts) CHECK(!conflict.editable());
    });
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    test_support::configure(argc, argv);
    test_support::Suite suite("ConflictClassifierTests");
    addClassificationMatrixCases(suite);
    addRebaseProvenanceCases(suite);
    addClassificationFailureCases(suite);
    return suite.report();
}
