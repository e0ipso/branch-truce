// GitAdapter against real Git and against scripted stand-ins: repository discovery, literal
// path handling, the helper and offline policy, staging, and the failure, cancellation and limit
// paths of one supervised Git process.
#include "TestHarness.h"
#include "core/ResourceLimits.h"
#include "git/GitFixtures.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <QVersionNumber>

#include <bit>
#include <memory>
#include <span>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <vector>

#include <iostream>
#include <memory>
#include <optional>
#include <string>

using namespace git_fixture;

static_assert(std::is_same_v<std::underlying_type_t<Operation>, int>);

namespace {
// The tool `git mergetool` runs in the parent staging cases, from the command line.
QString parentStagingFixture;

QByteArray indexWithoutPath(const QByteArray &index, const QByteArray &path)
{
    QByteArray otherEntries;
    for (const auto &record : index.split('\0')) {
        if (!record.isEmpty() && record.mid(record.indexOf('\t') + 1) != path) otherEntries += record + '\0';
    }
    return otherEntries;
}

// Sets one variable of this process's environment for the lifetime of the object.
class EnvironmentValue {
public:
    EnvironmentValue(QByteArray name, const QByteArray &value)
        : name_(std::move(name)),
          previous_(qgetenv(name_.constData())),
          set_(qEnvironmentVariableIsSet(name_.constData()))
    {
        CHECK(qputenv(name_.constData(), value));
    }
    ~EnvironmentValue()
    {
        if (set_)
            qputenv(name_.constData(), previous_);
        else
            qunsetenv(name_.constData());
    }
    EnvironmentValue(const EnvironmentValue &) = delete;
    EnvironmentValue &operator=(const EnvironmentValue &) = delete;

private:
    QByteArray name_;
    QByteArray previous_;
    bool set_;
};

// Leaves tracked.txt conflicted with marker text and registers the parent staging fixture as the
// tool truce-coverage, which installs `scenario`'s hazard before its preflight.
void registerMergetool(Repository &repo, const QString &scenario)
{
    const auto base = repo.git({"hash-object", "-w", "--stdin"}, "base\n").trimmed();
    const auto ours = repo.git({"hash-object", "-w", "--stdin"}, "ours\n").trimmed();
    const auto theirs = repo.git({"hash-object", "-w", "--stdin"}, "theirs\n").trimmed();
    const auto nul = QByteArray(1, '\0');
    repo.git({"update-index", "-z", "--index-info"},
             "0 " + QByteArray(40, '0') + "\ttracked.txt" + nul + "100644 " + base + " 1\ttracked.txt" + nul + "100644 "
                 + ours + " 2\ttracked.txt" + nul + "100644 " + theirs + " 3\ttracked.txt" + nul);
    writeFile(repo.path("tracked.txt"), "<<<<<<< ours\nours\n=======\ntheirs\n>>>>>>> theirs\n");
    const auto command = test_support::shellQuote(parentStagingFixture) + ' ' + scenario + " \"$MERGED\"";
    repo.git({"config", "mergetool.truce-coverage.cmd", command});
    repo.git({"config", "mergetool.truce-coverage.trustExitCode", "true"});
    repo.git({"config", "mergetool.keepBackup", "false"});
}

// Operation metadata that discovery must refuse to label, applied to a repository in the middle
// of a merge or rebase of conflictHistory().
struct MetadataCase {
    const char *name;
    ProblemCode reason;
    Outcome outcome;
    void (*change)(Repository &, const ConflictHistory &);
};

std::span<const MetadataCase> unsupportedMetadata()
{
    static const MetadataCase rows[] = {
        {"a rebase directory beside the merge", ProblemCode::DiscoveryConflictingOperations, Outcome::InspectionFailure,
         [](Repository &repo, const ConflictHistory &) { CHECK(QDir().mkdir(repo.path(".git/rebase-merge"))); }},
        {"two MERGE_HEAD commits", ProblemCode::DiscoveryMultipleMergeHeads, Outcome::InspectionFailure,
         [](Repository &repo, const ConflictHistory &history) {
             writeFile(repo.path(".git/MERGE_HEAD"), history.side + '\n' + history.main + '\n');
         }},
        {"an invalid MERGE_HEAD", ProblemCode::DiscoveryInvalidCommitIdentity, Outcome::InspectionFailure,
         [](Repository &repo, const ConflictHistory &) { writeFile(repo.path(".git/MERGE_HEAD"), "not-a-commit\n"); }},
        {"a symbolic link as MERGE_HEAD", ProblemCode::DiscoveryMetadataNotFile, Outcome::InspectionFailure,
         [](Repository &repo, const ConflictHistory &history) {
             CHECK(QFile::remove(repo.path(".git/MERGE_HEAD")));
             writeFile(repo.path("marker"), history.side + '\n');
             CHECK(QFile::link(repo.path("marker"), repo.path(".git/MERGE_HEAD")));
         }},
        {"a cherry-pick in place of the merge", ProblemCode::DiscoverySequencerUnsupported, Outcome::Success,
         [](Repository &repo, const ConflictHistory &history) {
             CHECK(QFile::remove(repo.path(".git/MERGE_HEAD")));
             writeFile(repo.path(".git/CHERRY_PICK_HEAD"), history.side + '\n');
         }},
        {"a revert in place of the merge", ProblemCode::DiscoverySequencerUnsupported, Outcome::Success,
         [](Repository &repo, const ConflictHistory &history) {
             CHECK(QFile::remove(repo.path(".git/MERGE_HEAD")));
             writeFile(repo.path(".git/REVERT_HEAD"), history.side + '\n');
         }},
        {"a mailbox application in place of the merge", ProblemCode::DiscoveryApplyMailbox, Outcome::Success,
         [](Repository &repo, const ConflictHistory &) {
             CHECK(QFile::remove(repo.path(".git/MERGE_HEAD")));
             CHECK(QDir().mkdir(repo.path(".git/rebase-apply")));
             writeFile(repo.path(".git/rebase-apply/applying"), "");
         }},
        {"REBASE_HEAD without a rebase", ProblemCode::DiscoveryRebaseHeadAlone, Outcome::InspectionFailure,
         [](Repository &repo, const ConflictHistory &history) {
             CHECK(QFile::remove(repo.path(".git/MERGE_HEAD")));
             writeFile(repo.path(".git/REBASE_HEAD"), history.side + '\n');
         }}};
    return rows;
}

std::span<const MetadataCase> inconsistentRebases()
{
    static const MetadataCase rows[] = {
        {"stopped-sha names another commit", ProblemCode::DiscoveryReplayDisagrees, Outcome::InspectionFailure,
         [](Repository &repo, const ConflictHistory &history) {
             writeFile(repo.path(".git/rebase-merge/stopped-sha"), history.main + '\n');
         }},
        {"an invalid head-name", ProblemCode::DiscoveryInvalidHeadName, Outcome::InspectionFailure,
         [](Repository &repo, const ConflictHistory &) {
             writeFile(repo.path(".git/rebase-merge/head-name"), "refs/heads/../side\n");
         }},
        {"orig-head outside the rebased history", ProblemCode::DiscoveryReplayOutsideHistory,
         Outcome::InspectionFailure,
         [](Repository &repo, const ConflictHistory &history) {
             writeFile(repo.path(".git/rebase-merge/orig-head"), history.main + '\n');
         }},
        {"onto that the replay is not based on", ProblemCode::DiscoveryHeadNotOnOnto, Outcome::InspectionFailure,
         [](Repository &repo, const ConflictHistory &history) {
             writeFile(repo.path(".git/rebase-merge/onto"), history.side + '\n');
         }},
        {"no commit being replayed", ProblemCode::DiscoveryNoReplayedCommit, Outcome::InspectionFailure,
         [](Repository &repo, const ConflictHistory &) {
             CHECK(QFile::remove(repo.path(".git/REBASE_HEAD")));
             CHECK(QFile::remove(repo.path(".git/rebase-merge/stopped-sha")));
         }},
        {"no orig-head", ProblemCode::DiscoveryMetadataMissing, Outcome::InspectionFailure,
         [](Repository &repo, const ConflictHistory &) {
             CHECK(QFile::remove(repo.path(".git/rebase-merge/orig-head")));
         }}};
    return rows;
}

// How each promisor case marks the repository as able to fetch missing objects lazily.
const char *const promisorSources[] = {"extensions.partialClone", "remote.origin.promisor", "a promisor pack",
                                       "a promisor pack in an alternate"};
void renamedConflictsKeepVerifiedPaths()
{
    Repository repo;
    const auto history = conflictHistory(repo, true);
    repo.git({"merge", "--no-edit", "side"}, {}, 1);
    const auto before = repo.git({"ls-files", "--unmerged", "-z"});
    GitAdapter adapter(repo.program());
    const auto discovered = run(adapter, repo.request(Operation::DiscoverRepository));
    checkOperationIdentity(discovered, history, RepositoryOperation::Merge);
    CHECK(discovered.indexEntries.size() == 6);
    ConflictClassifier classifier(repo.program());
    const auto classified = run(classifier, {repo.root(), 71, 93});
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
                     (wrongMode ? QByteArray("100755 ") : QByteArray("100644 ")) + (wrongMode ? object : unrelated)
                         + " 3\t" + path + nul);
            const auto index = repo.git({"ls-files", "--unmerged", "-z"});
            const auto result = run(adapter, repo.request(Operation::DiscoverRepository));
            CHECK(result.outcome == Outcome::Success && result.repository.supported);
            CHECK(result.repository.operation == RepositoryOperation::Merge && result.indexEntries.size() == 6);
            CHECK(result.repository.unsupportedPaths.size() == 1 && result.repository.unsupportedPaths.contains(path));
            CHECK(hasProblem(result.repository.unsupportedPaths.value(path), ProblemCode::DiscoveryStageProvenance)
                  && result.repository.unsupportedPaths.value(path).arguments.value(0) == QStringLiteral("3"));
            CHECK(result.repository.stage2.commitId == history.main
                  && result.repository.stage3.commitId == history.side);
            const auto classifiedInvalid = run(classifier, {repo.root(), 71, 93});
            CHECK(classifiedInvalid.outcome == Outcome::Success && classifiedInvalid.conflicts.size() == 2);
            for (const auto &conflict : classifiedInvalid.conflicts) {
                if (conflict.path == path) {
                    CHECK(!conflict.editable() && !conflict.reason.isEmpty() && !conflict.result
                          && conflict.sources.isEmpty());
                    CHECK(conflict.support
                          == (wrongMode ? ConflictSupport::ModeConflict : ConflictSupport::UnverifiedSource));
                } else
                    CHECK(conflict.editable());
            }
            CHECK(repo.git({"ls-files", "--unmerged", "-z"}) == index);
        }
    }
}

void promisorRejectsEveryOperation(int scenario)
{
    Repository repo;
    Repository alternate;
    const auto blob = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
    const auto before = repo.git({"ls-files", "--stage", "-z"});
    if (scenario == 0) repo.git({"config", "extensions.partialClone", "origin"});
    if (scenario == 1) repo.git({"config", "remote.origin.promisor", "true"});
    if (scenario == 2) writeFile(repo.path(".git/objects/pack/pack-fixture.promisor"), "");
    if (scenario == 3) {
        writeFile(alternate.path(".git/objects/pack/pack-fixture.promisor"), "");
        writeFile(repo.path(".git/objects/info/alternates"),
                  "# shared with base\n" + alternate.path(".git/objects").toUtf8() + '\n');
    }
    repo.git({"config", "remote.origin.url", repo.root()});
    repo.git({"config", "remote.origin.uploadpack", "touch .fetch-ran"});
    GitAdapter adapter(repo.program());
    for (const auto operation : {Operation::DiscoverRepository, Operation::InspectIndex, Operation::ReadObject,
                                 Operation::ReadAttributes, Operation::MaterializeSource, Operation::CanonicalizeText,
                                 Operation::CheckParentStaging, Operation::StageFile}) {
        const bool noPath = operation == Operation::DiscoverRepository || operation == Operation::ReadObject;
        auto request = repo.request(operation, noPath ? "" : "tracked.txt");
        if (operation == Operation::ReadObject || operation == Operation::MaterializeSource) request.objectId = blob;
        if (operation == Operation::CanonicalizeText) request.input = "reviewed\n";
        const auto result = run(adapter, request);
        CHECK(result.outcome == Outcome::Rejected && result.mutation == Mutation::NotAttempted);
        CHECK(result.problem.code
              == (scenario >= 2 ? ProblemCode::GitPromisorPacks : ProblemCode::GitPartialOrMonitor));
    }
    // A missing object must be rejected before Git can try lazy retrieval.
    auto missing = repo.request(Operation::ReadObject);
    missing.objectId = QByteArray(40, '1');
    CHECK(run(adapter, missing).outcome == Outcome::Rejected);
    auto materializeMissing = repo.request(Operation::MaterializeSource, "tracked.txt");
    materializeMissing.objectId = missing.objectId;
    CHECK(run(adapter, materializeMissing).outcome == Outcome::Rejected);
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
    checkConversionCleanup(repo);
}

void addRepositoryDiscoveryCases(test_support::Suite &suite)
{
    suite.run("real Git fixtures report their version and enforce the 2.43 floor", [] {
        Repository repo;
        const auto version = repo.git({"--version"}).trimmed();
        std::cout << "GIT FIXTURE " << test_support::fixtureGit() << ' ' << version.toStdString() << '\n';
        CHECK(version.startsWith("git version "));
        CHECK(QVersionNumber::fromString(QString::fromLatin1(version.mid(12))) >= QVersionNumber(2, 43));
        checkConversionCleanup(repo);
    });
    suite.run("merge discovery uses per-worktree metadata and verified stage identities", [] {
        for (const bool linked : {false, true}) {
            Repository repo;
            const auto history = conflictHistory(repo);
            QTemporaryDir linkedParent;
            CHECK(linkedParent.isValid());
            auto directory = repo.root();
            if (linked) {
                directory = linkedParent.path() + "/checkout";
                repo.git({"worktree", "add", "--quiet", "--detach", directory, "main"});
            }
            repo.git({"-C", directory, "merge", "--no-edit", "side"}, {}, 1);
            CHECK(QDir().mkpath(directory + "/nested"));
            auto request = repo.request(Operation::DiscoverRepository);
            request.directory = directory + "/nested";
            const auto before = repo.git({"-C", directory, "ls-files", "--unmerged", "-z"});
            GitAdapter adapter(repo.program());
            const auto result = run(adapter, request);
            checkOperationIdentity(result, history, RepositoryOperation::Merge);
            CHECK(result.repository.worktreeRoot == directory && result.indexEntries.size() == 3);
            const auto gitDirectory =
                QString::fromUtf8(repo.git({"-C", directory, "rev-parse", "--absolute-git-dir"}).trimmed());
            CHECK(result.repository.gitDirectory == QFileInfo(gitDirectory).canonicalFilePath());
            writeFile(directory + "/tracked.txt", "launch-time manual result\n");
            ConflictClassifier classifier(repo.program());
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
                CHECK(original.outcome == Outcome::Success
                      && original.repository.operation == RepositoryOperation::None);
                CHECK(!original.repository.supported && original.indexEntries.isEmpty());
            }
        }
    });
    suite.run("ambiguous branch names fall back to the verified commit role", [] {
        Repository repo;
        const auto history = conflictHistory(repo);
        repo.git({"merge", "--no-edit", "side"}, {}, 1);
        repo.git({"branch", "same-commit", "main"});
        GitAdapter adapter(repo.program());
        const auto result = run(adapter, repo.request(Operation::DiscoverRepository));
        CHECK(result.outcome == Outcome::Success && result.repository.supported);
        CHECK(result.repository.stage2.commitId == history.main);
        CHECK(result.repository.stage2.name.isEmpty());
        CHECK(result.repository.stage2.label == QStringLiteral("Current checkout"));
        CHECK(result.repository.stage3.name == QStringLiteral("side"));
    });
    for (const auto &metadata : unsupportedMetadata()) {
        suite.run(std::string("unsupported and inconsistent operation metadata cannot acquire source labels: ")
                      + metadata.name,
                  [&metadata] {
                      Repository repo;
                      const auto history = conflictHistory(repo);
                      repo.git({"merge", "--no-edit", "side"}, {}, 1);
                      metadata.change(repo, history);
                      checkUnsupportedDiscovery(repo, metadata.reason, metadata.outcome);
                  });
    }
    for (const auto &metadata : inconsistentRebases()) {
        suite.run(std::string("rebase discovery rejects disagreement and unverified ancestry: ") + metadata.name,
                  [&metadata] {
                      Repository repo;
                      const auto history = conflictHistory(repo);
                      repo.git({"checkout", "--quiet", "side"});
                      repo.git({"rebase", "--merge", "main"}, {}, 1);
                      metadata.change(repo, history);
                      checkUnsupportedDiscovery(repo, metadata.reason, metadata.outcome);
                  });
    }
    suite.run("multiple merge bases and replayed merge commits reject ambiguous provenance", [] {
        Repository repo;
        const auto history = conflictHistory(repo);
        const auto mergeCommit = [&](const QByteArray &first, const QByteArray &second) {
            const auto tree = repo.git({"rev-parse", QString::fromLatin1(first) + "^{tree}"}).trimmed();
            return repo
                .git({"-c", "user.name=Branch Truce Tests", "-c", "user.email=tests@example.invalid", "commit-tree",
                      QString::fromLatin1(tree), "-p", QString::fromLatin1(first), "-p", QString::fromLatin1(second),
                      "-m", "criss-cross"})
                .trimmed();
        };
        const auto left = mergeCommit(history.main, history.side);
        const auto right = mergeCommit(history.side, history.main);
        repo.git({"reset", "--hard", QString::fromLatin1(left)});
        repo.git({"update-ref", "refs/heads/side", QString::fromLatin1(right)});
        CHECK(repo.git({"merge-base", "--all", "main", "side"}).count('\n') == 2);
        repo.git({"merge", "--no-edit", "side"}, {}, 1);
        checkUnsupportedDiscovery(repo, ProblemCode::DiscoveryMultipleBases);
        CHECK(QFile::remove(repo.path(".git/MERGE_HEAD")));
        CHECK(QDir().mkdir(repo.path(".git/rebase-merge")));
        writeFile(repo.path(".git/rebase-merge/head-name"), "refs/heads/side\n");
        writeFile(repo.path(".git/rebase-merge/orig-head"), right + '\n');
        writeFile(repo.path(".git/rebase-merge/onto"), history.main + '\n');
        writeFile(repo.path(".git/rebase-merge/stopped-sha"), right + '\n');
        writeFile(repo.path(".git/REBASE_HEAD"), right + '\n');
        checkUnsupportedDiscovery(repo, ProblemCode::DiscoveryMergeCommitReplay);
    });
    suite.run("renamed text conflicts verify old-path blobs and isolate invalid paths",
              renamedConflictsKeepVerifiedPaths);
}

void addReadAndStageCases(test_support::Suite &suite)
{
    suite.run("named reads use real Git and preserve request identity", [] {
        Repository repo;
        GitAdapter adapter(repo.program());
        for (const auto operation :
             {Operation::DiscoverRepository, Operation::InspectIndex, Operation::ReadAttributes}) {
            const bool needsPath = operation == Operation::InspectIndex || operation == Operation::ReadAttributes;
            const auto result = run(adapter, repo.request(operation, needsPath ? "tracked.txt" : ""));
            CHECK(result.outcome == Outcome::Success && result.exitCode == 0);
            CHECK(result.mutation == Mutation::NotAttempted && !result.outputIncomplete);
            if (operation == Operation::DiscoverRepository) {
                CHECK(result.standardOutput.contains(repo.root().toUtf8()));
                CHECK(result.repository.worktreeRoot == repo.root());
                CHECK(result.repository.gitDirectory == repo.path(".git"));
                CHECK(result.repository.operation == RepositoryOperation::None && !result.repository.supported);
                CHECK(!result.repository.problem.isEmpty() && result.indexEntries.isEmpty());
            }
            if (operation == Operation::InspectIndex)
                CHECK(result.standardOutput.endsWith(QByteArray("\ttracked.txt\0", 13)));
        }
        const auto object = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
        for (const auto operation : {Operation::ReadObject, Operation::MaterializeSource}) {
            auto request = repo.request(operation, operation == Operation::MaterializeSource ? "tracked.txt" : "");
            request.objectId = object;
            const auto result = run(adapter, request);
            CHECK(result.outcome == Outcome::Success && result.standardOutput == "before\n");
            CHECK(result.mutation == Mutation::NotAttempted);
        }
        TextConversion conversion(repo.program());
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
        CHECK(wrongMode.outcome == Outcome::Rejected
              && wrongMode.problem.code == ProblemCode::ConversionStagedIdentity);
    });
    suite.run("literal conflict staging preserves every unrelated staged entry", [] {
        Repository repo;
        CHECK(QDir(repo.root()).mkdir("nested"));
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
        const QByteArray unrelated = "unrelated staged\tfile.txt";
        repo.writePath(unrelated, "staged content\n");
        repo.git({"add", "--", QString::fromUtf8(unrelated)});
        repo.git({"update-index", "--chmod=+x", "--", QString::fromUtf8(unrelated)});
        const auto unrelatedIndex = repo.git({"ls-files", "--stage", "-z", "--", QString::fromUtf8(unrelated)});
        CHECK(unrelatedIndex.startsWith("100755 "));
        repo.writePath(unrelated, "unstaged content must remain unstaged\n");
        GitAdapter adapter(repo.program());
        // Synthetic unmerged stages without an operation must stay visible,
        // but cannot acquire merge/rebase source labels from their position.
        const auto discovery = run(adapter, repo.request(Operation::DiscoverRepository));
        CHECK(discovery.outcome == Outcome::Success && !discovery.repository.supported);
        CHECK(discovery.repository.operation == RepositoryOperation::Unsupported);
        CHECK(repo.git({"ls-files", "--unmerged", "-z"}).count('\0') == names.size() * 3);
        CHECK(discovery.indexEntries.size() == names.size() * 3);
        CHECK(discovery.repository.stage2.commitId.isEmpty() && discovery.repository.stage3.commitId.isEmpty());
        CHECK(discovery.repository.problem.code == ProblemCode::DiscoveryUnverifiedIndex);
        ConflictClassifier classifier(repo.program());
        const auto classified = run(classifier, {repo.root(), 71, 93});
        CHECK(classified.outcome == Outcome::Success && classified.conflicts.size() == names.size());
        for (const auto &conflict : classified.conflicts) {
            CHECK(names.contains(conflict.path) && conflict.stages.size() == 3 && !conflict.editable());
            CHECK(conflict.support == ConflictSupport::UnsupportedOperation && !conflict.reason.isEmpty());
            // The classification wraps discovery's reason instead of repeating it.
            CHECK(conflict.reason.code == ProblemCode::ClassificationUnsupportedOperation && conflict.reason.cause
                  && conflict.reason.cause->code == ProblemCode::DiscoveryUnverifiedIndex);
        }
        for (const auto &name : names) {
            unsigned int stages = 0;
            for (const auto &entry : discovery.indexEntries) {
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
                CHECK(source.standardOutput
                      == (entry.stage == 1       ? "base\n"
                              : entry.stage == 2 ? "ours\n"
                                                 : "theirs\n"));
            }
            CHECK(stages == 14);
            repo.writePath(name, "resolved\n");
            const auto before = repo.git({"ls-files", "--stage", "-z"});
            const auto staged = run(adapter, repo.request(Operation::StageFile, name));
            CHECK(staged.outcome == Outcome::Success && staged.mutation == Mutation::ObservedChanged);
            CHECK(staged.indexBefore.count('\0') == 3 && staged.indexAfter.count('\0') == 1);
            CHECK(staged.indexAfter.endsWith('\t' + name + QByteArray(1, '\0')));
            const auto after = repo.git({"ls-files", "--stage", "-z"});
            CHECK(indexWithoutPath(before, name) == indexWithoutPath(after, name));
            CHECK(repo.git({"ls-files", "--stage", "-z", "--", QString::fromUtf8(unrelated)}) == unrelatedIndex);
        }
        CHECK(repo.git({"ls-files", "--unmerged", "-z"}).isEmpty());
        CHECK(repo.git({"show", ":" + QString::fromUtf8(unrelated)}) == "staged content\n");
        QFile untouched(repo.path(QString::fromUtf8(unrelated)));
        CHECK(untouched.open(QIODevice::ReadOnly) && untouched.readAll() == "unstaged content must remain unstaged\n");
        untouched.close();
        checkConversionCleanup(repo);
    });
}

void addPathAdmissionCases(test_support::Suite &suite)
{
    suite.run("enumerated and staged paths retain special and non-UTF-8 bytes", [] {
        Repository repo;
        CHECK(QDir(repo.root()).mkdir("nested"));
        GitAdapter adapter(repo.program());
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
            const QByteArray identity(name.constData(), name.size());
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
        GitAdapter adapter(repo.program());
        const auto before = repo.git({"ls-files", "--stage", "-z"});
        const QList<QByteArray> paths{"",          "/absolute", "../escape",          "dir/../file", "./tracked.txt",
                                      "dir//file", "dir/",      QByteArray("a\0b", 3)};
        for (const auto &path : paths) {
            const auto result = run(adapter, repo.request(Operation::StageFile, path));
            CHECK(result.outcome == Outcome::Rejected && result.mutation == Mutation::NotAttempted);
            CHECK(result.exitCode == -1 && !result.problem.isEmpty());
        }
        auto bad = repo.request(Operation::ReadObject);
        for (const auto &object : {QByteArray("HEAD"), QByteArray(40, 'g'), QByteArray("--help")}) {
            bad.objectId = object;
            CHECK(run(adapter, bad).outcome == Outcome::Rejected);
        }
        bad = repo.request(Operation::DiscoverRepository, "unexpected");
        CHECK(run(adapter, bad).outcome == Outcome::Rejected);
        bad = repo.request(Operation::InspectIndex, "tracked.txt");
        bad.objectId = QByteArray(40, 'a');
        CHECK(run(adapter, bad).outcome == Outcome::Rejected);
        bad = repo.request(Operation::ReadAttributes, "tracked.txt");
        bad.input = "only canonicalization reads input";
        CHECK(run(adapter, bad).outcome == Outcome::Rejected);
        bad = repo.request(std::bit_cast<Operation>(int{999}));
        CHECK(run(adapter, bad).outcome == Outcome::Rejected);
        bad = repo.request(Operation::DiscoverRepository);
        bad.directory = ".";
        CHECK(run(adapter, bad).outcome == Outcome::Rejected);
        CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
    });
    suite.run("symlink traversal and nonregular staging are rejected", [] {
        Repository repo;
        CHECK(QDir(repo.root()).mkdir("dir"));
        writeFile(repo.path("dir/file"), "data\n");
        CHECK(QFile::link(repo.path("tracked.txt"), repo.path("link")));
        CHECK(QFile::link(repo.path("dir"), repo.path("linked-dir")));
        GitAdapter adapter(repo.program());
        for (const auto &path : {"link", "linked-dir/file", "dir", "missing"}) {
            const auto result = run(adapter, repo.request(Operation::StageFile, path));
            CHECK(result.outcome == Outcome::Rejected && result.mutation == Mutation::NotAttempted);
        }
        const QByteArray raw = QByteArray("link-") + char(0xff);
        const auto link = QFile::encodeName(repo.root()) + '/' + raw;
        CHECK(::symlink(QFile::encodeName(repo.path("tracked.txt")).constData(), link.constData()) == 0);
        repo.adoptRawPath(link);
        repo.writePath(QString::fromUtf8(raw).toUtf8(), "display decoy\n");
        const auto before = repo.git({"ls-files", "--stage", "-z"});
        const auto result = run(adapter, repo.request(Operation::StageFile, raw));
        CHECK(result.outcome == Outcome::Rejected && result.mutation == Mutation::NotAttempted);
        CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
    });
    suite.run("executable trust is checked before launch and again after selection", [] {
        Repository repo;
        const auto selected = repo.executable("#!/bin/sh\nprintf 'must not run' > .unsafe-executable\n");
        GitAdapter changed(repo.program(selected));
        writeFile(selected, "#!/bin/sh\nprintf 'changed executable must not run' > .unsafe-executable\n", true);
        CHECK(run(changed, repo.request(Operation::DiscoverRepository)).outcome == Outcome::Rejected);
        GitAdapter relative(repo.program(QStringLiteral("git")));
        CHECK(run(relative, repo.request(Operation::DiscoverRepository)).outcome == Outcome::Rejected);
        const auto inside = repo.path("git");
        writeFile(inside, "#!/bin/sh\nprintf unsafe > .unsafe-executable\n", true);
        GitAdapter inRepository(repo.program(inside));
        CHECK(run(inRepository, repo.request(Operation::DiscoverRepository)).outcome == Outcome::Rejected);
        CHECK(QFile::setPermissions(selected,
                                    QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                                        | QFileDevice::WriteGroup));
        GitAdapter writable(repo.program(selected));
        CHECK(run(writable, repo.request(Operation::DiscoverRepository)).outcome == Outcome::Rejected);
        CHECK(!QFile::exists(repo.path(".unsafe-executable")));
    });
    suite.run("ambient Git and executable search overrides do not escape the fixture", [] {
        Repository repo;
        Repository other;
        const auto otherBefore = other.git({"ls-files", "--stage", "-z"});
        EnvironmentValue gitDir("GIT_DIR", other.path(".git").toUtf8());
        EnvironmentValue workTree("GIT_WORK_TREE", other.root().toUtf8());
        EnvironmentValue index("GIT_INDEX_FILE", other.path("redirected-index").toUtf8());
        EnvironmentValue configCount("GIT_CONFIG_COUNT", "1");
        EnvironmentValue configKey("GIT_CONFIG_KEY_0", "core.fsmonitor");
        EnvironmentValue configValue("GIT_CONFIG_VALUE_0", "must-not-run");
        EnvironmentValue path("PATH", repo.root().toUtf8());
        GitAdapter adapter(repo.program());
        writeFile(repo.path("tracked.txt"), "new\n");
        const auto result = run(adapter, repo.request(Operation::StageFile, "tracked.txt"));
        CHECK(result.outcome == Outcome::Success && result.mutation == Mutation::ObservedChanged);
        CHECK(repo.git({"show", ":tracked.txt"}) == "new\n");
        CHECK(other.git({"ls-files", "--stage", "-z"}) == otherBefore);
        CHECK(!QFile::exists(other.path("redirected-index")));
    });
}

void addFilterPolicyCases(test_support::Suite &suite)
{
    for (const auto &driver : {"Fixture", "set", "unset", "unspecified"}) {
        for (const auto &kind : {"clean", "smudge", "process", "required"}) {
            suite.run(std::string("a configured filter blocks every operation that would run it: filter=") + driver
                          + ' ' + kind,
                      [driver, kind] {
                          Repository repo;
                          const auto key = QByteArray("filter.") + driver + '.' + kind;
                          writeFile(repo.path(".gitattributes"), "tracked.txt filter=" + QByteArray(driver) + '\n');
                          const auto before = repo.git({"ls-files", "--stage", "-z"});
                          const auto blob = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
                          repo.git({"config", QString::fromLatin1(key),
                                    QByteArray(kind) == "required" ? "true" : "touch .helper-ran; cat"});
                          GitAdapter adapter(repo.program());
                          // Policy inspection and raw identity reads never invoke filters.
                          CHECK(run(adapter, repo.request(Operation::ReadAttributes, "tracked.txt")).outcome
                                == Outcome::Success);
                          auto raw = repo.request(Operation::ReadObject);
                          raw.objectId = blob;
                          CHECK(run(adapter, raw).standardOutput == "before\n");
                          for (const auto operation : {Operation::MaterializeSource, Operation::CanonicalizeText,
                                                       Operation::CheckParentStaging, Operation::StageFile}) {
                              auto request = repo.request(operation, "tracked.txt");
                              if (operation == Operation::MaterializeSource) request.objectId = blob;
                              if (operation == Operation::CanonicalizeText) request.input = "reviewed\n";
                              const auto result = run(adapter, request);
                              CHECK(result.outcome == Outcome::Rejected && result.mutation == Mutation::NotAttempted);
                              CHECK(hasProblem(result.problem, ProblemCode::GitCustomFilter));
                          }
                          TextConversion conversion(repo.program());
                          for (const auto operation : {TextOperation::ReadSource, TextOperation::PrepareResult}) {
                              auto request = textRequest(repo, operation, blob);
                              request.editedUtf8 = "reviewed\n";
                              const auto result = run(conversion, request);
                              CHECK(result.outcome == Outcome::Rejected && !result.text
                                    && hasProblem(result.problem, ProblemCode::GitCustomFilter));
                          }
                          CHECK(!QFile::exists(repo.path(".helper-ran")));
                          repo.git({"config", "--unset", QString::fromLatin1(key)});
                          CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
                      });
        }
    }
    suite.run("unused filters leave reads and staging allowed, and every request rechecks the policy", [] {
        Repository repo;
        GitAdapter adapter(repo.program());
        const auto blob = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
        repo.git({"config", "filter.unused.clean", "touch .helper-ran; cat"});
        repo.git({"config", "filter.unused.smudge", "touch .helper-ran; cat"});
        repo.git({"config", "filter.unspecified.smudge", "touch .helper-ran; cat"});
        writeFile(repo.path(".gitattributes"), "other.txt filter=unused\ntracked.txt text eol=crlf\n");
        auto source = repo.request(Operation::MaterializeSource, "tracked.txt");
        source.objectId = blob;
        CHECK(run(adapter, source).standardOutput == "before\r\n");
        TextConversion conversion(repo.program());
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
        CHECK(denied.outcome == Outcome::Rejected && hasProblem(denied.problem, ProblemCode::GitCustomFilter));
        CHECK(run(conversion, prepare).outcome == Outcome::Rejected);
        CHECK(run(adapter, repo.request(Operation::StageFile, "tracked.txt")).outcome == Outcome::Rejected);
        CHECK(QDir(repo.root()).mkdir("subdirectory"));
        for (const auto operation : {Operation::MaterializeSource, Operation::StageFile}) {
            auto request = repo.request(operation, "tracked.txt");
            request.directory = repo.path("subdirectory");
            if (operation == Operation::MaterializeSource) request.objectId = blob;
            CHECK(run(adapter, request).outcome == Outcome::Rejected);
        }
        writeFile(repo.path(".gitattributes"), "/other.txt filter=unused\n");
        for (const auto operation : {Operation::MaterializeSource, Operation::StageFile}) {
            auto request = repo.request(operation, "tracked.txt");
            request.directory = repo.path("subdirectory");
            if (operation == Operation::MaterializeSource) request.objectId = blob;
            CHECK(run(adapter, request).outcome == Outcome::Success);
        }
        CHECK(!QFile::exists(repo.path(".helper-ran")));
    });
}

void addLocalHelperPolicyCases(test_support::Suite &suite)
{
    for (const auto &hooksPath : {".git/hooks", "custom hooks"}) {
        suite.run(std::string("a post-index-change hook blocks parent staging and staging: ") + hooksPath, [hooksPath] {
            Repository repo;
            CHECK(QDir(repo.root()).mkpath(hooksPath));
            repo.git({"config", "core.hooksPath", hooksPath});
            const auto blob = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
            const auto before = repo.git({"ls-files", "--stage", "-z"});
            GitAdapter adapter(repo.program());
            writeFile(repo.path(QString(hooksPath) + "/pre-commit"), "#!/bin/sh\ntouch .hook-ran\n", true);
            CHECK(run(adapter, repo.request(Operation::StageFile, "tracked.txt")).outcome == Outcome::Success);
            const auto hook = repo.path(QString(hooksPath) + "/post-index-change");
            writeFile(hook, "#!/bin/sh\ntouch .hook-ran\n", true);
            for (const auto operation : {Operation::CheckParentStaging, Operation::StageFile}) {
                const auto result = run(adapter, repo.request(operation, "tracked.txt"));
                CHECK(result.outcome == Outcome::Rejected && result.mutation == Mutation::NotAttempted);
                CHECK(result.problem.code == ProblemCode::GitPostIndexChangeHook);
            }
            auto source = repo.request(Operation::MaterializeSource, "tracked.txt");
            source.objectId = blob;
            CHECK(run(adapter, source).outcome == Outcome::Success);
            CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
            CHECK(!QFile::exists(repo.path(".hook-ran")));
            CHECK(QFile::setPermissions(hook, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
            CHECK(run(adapter, repo.request(Operation::CheckParentStaging, "tracked.txt")).outcome == Outcome::Success);
            repo.git({"config", "core.hooksPath", "/dev/null"});
            CHECK(run(adapter, repo.request(Operation::StageFile, "tracked.txt")).outcome == Outcome::Success);
        });
    }
    suite.run("core.fsmonitor blocks parent staging and staging but no read", [] {
        Repository repo;
        GitAdapter adapter(repo.program());
        const auto blob = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
        const auto before = repo.git({"ls-files", "--stage", "-z"});
        repo.git({"config", "core.fsmonitor", "touch .monitor-ran"});
        for (const auto operation :
             {Operation::DiscoverRepository, Operation::InspectIndex, Operation::ReadObject, Operation::ReadAttributes,
              Operation::MaterializeSource, Operation::CanonicalizeText}) {
            const bool noPath = operation == Operation::DiscoverRepository || operation == Operation::ReadObject;
            auto request = repo.request(operation, noPath ? "" : "tracked.txt");
            if (operation == Operation::ReadObject || operation == Operation::MaterializeSource)
                request.objectId = blob;
            if (operation == Operation::CanonicalizeText) request.input = "before\n";
            CHECK(run(adapter, request).outcome == Outcome::Success);
        }
        for (const auto operation : {Operation::CheckParentStaging, Operation::StageFile}) {
            const auto result = run(adapter, repo.request(operation, "tracked.txt"));
            CHECK(result.outcome == Outcome::Rejected && result.mutation == Mutation::NotAttempted);
            CHECK(hasProblem(result.problem, ProblemCode::GitPartialOrMonitor, {QStringLiteral("core.fsmonitor")}));
        }
        CHECK(!QFile::exists(repo.path(".monitor-ran")));
        repo.git({"config", "core.fsmonitor", "false"});
        CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
        CHECK(run(adapter, repo.request(Operation::StageFile, "tracked.txt")).outcome == Outcome::Success);
    });
}

void addObjectStoragePolicyCases(test_support::Suite &suite)
{
    suite.run("alternate object storage is allowed only when every alternate can be inspected offline", [] {
        Repository repo;
        repo.git(
            {"-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "--quiet", "-m", "base"});
        QTemporaryDir shared;
        CHECK(shared.isValid());
        const auto clone = shared.path() + "/shared";
        repo.git({"clone", "--shared", "--quiet", repo.root(), clone});
        const auto alternates = clone + "/.git/objects/info/alternates";
        const auto alternatePath = QFile::encodeName(repo.path(".git/objects")) + '\n';
        auto request = repo.request(Operation::InspectIndex, "tracked.txt");
        request.directory = clone;
        GitAdapter adapter(repo.program());
        CHECK(run(adapter, request).outcome == Outcome::Success);
        writeFile(alternates, "# shared with base\n" + alternatePath);
        CHECK(repo.git({"-C", clone, "cat-file", "-t", "HEAD"}) == "commit\n");
        CHECK(repo.git({"-C", clone, "ls-files", "--stage", "-z"}) == repo.git({"ls-files", "--stage", "-z"}));
        const auto blob = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
        for (const auto operation :
             {Operation::DiscoverRepository, Operation::InspectIndex, Operation::ReadObject, Operation::ReadAttributes,
              Operation::MaterializeSource, Operation::CanonicalizeText, Operation::CheckParentStaging,
              Operation::StageFile}) {
            const bool noPath = operation == Operation::DiscoverRepository || operation == Operation::ReadObject;
            request.operation = operation;
            request.path = noPath ? "" : "tracked.txt";
            request.objectId =
                operation == Operation::ReadObject || operation == Operation::MaterializeSource ? blob : QByteArray{};
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
        writeFile(alternates,
                  "# shared with base\n" + alternatePath + QFile::encodeName(shared.path() + "/missing-objects")
                      + '\n');
        request = repo.request(Operation::InspectIndex, "tracked.txt");
        request.directory = clone;
        const auto denied = run(adapter, request);
        CHECK(denied.outcome == Outcome::Rejected);
        CHECK(denied.problem.code == ProblemCode::GitObjectStoreUninspectable);
    });
    for (int scenario = 0; scenario < 4; ++scenario) {
        suite.run(std::string("promisor and partial-clone repositories reject every operation before lazy "
                              "retrieval: ")
                      + promisorSources[scenario],
                  [scenario] { promisorRejectsEveryOperation(scenario); });
    }
}

void addParentAndFailureCases(test_support::Suite &suite)
{
    suite.run("a registered mergetool parent stages only after a successful release preflight", [] {
        for (const auto &scenario : {"plain", "crlf"}) {
            SCOPE(scenario);
            Repository repo;
            if (QByteArray(scenario) == "crlf") writeFile(repo.path(".gitattributes"), "/tracked.txt text eol=crlf\n");
            registerMergetool(repo, scenario);
            CHECK(repo.git({"ls-files", "--unmerged", "-z"}).count('\0') == 3);
            repo.git({"mergetool", "--no-prompt", "--tool=truce-coverage", "--", "tracked.txt"});
            CHECK(QFile::exists(repo.path(".handoff-released")) && !QFile::exists(repo.path(".handoff-rejected")));
            CHECK(repo.git({"ls-files", "--unmerged", "-z"}).isEmpty());
            CHECK(repo.git({"show", ":tracked.txt"}) == "resolved\n");
            GitAdapter adapter(repo.program());
            const auto acknowledged = run(adapter, repo.request(Operation::InspectIndex, "tracked.txt"));
            CHECK(acknowledged.outcome == Outcome::Success && acknowledged.indexEntries.size() == 1);
            CHECK(acknowledged.indexEntries.first().stage == 0);
            CHECK(acknowledged.indexEntries.first().objectId
                  == repo.git({"hash-object", "--stdin"}, "resolved\n").trimmed());
        }
    });
    suite.run("a registered mergetool parent retains conflicts when helper or offline preflight rejects release", [] {
        for (const auto &scenario : {"filter", "hook", "custom-hook", "monitor", "promisor"}) {
            SCOPE(scenario);
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
        const auto preflight =
            repo.executable("#!/bin/sh\nprintf 'preflight stdout'\nprintf 'preflight stderr' >&2\nexit 17\n");
        GitAdapter failing(repo.program(preflight));
        const auto result = run(failing, repo.readRequest());
        CHECK(result.outcome == Outcome::ExitFailure && result.exitCode == -1);
        // A policy check's output stays out of the result; its standard error and status do not.
        CHECK(result.standardOutput.isEmpty() && result.standardError == "preflight stderr");
        CHECK(hasProblem(result.problem, ProblemCode::GitExitCode, {QStringLiteral("17")})
              && result.mutation == Mutation::NotAttempted);
        const auto missing = repo.executable("#!/definitely/missing/interpreter\n");
        GitAdapter launch(repo.program(missing));
        const auto absent = run(launch, repo.readRequest());
        CHECK(absent.outcome == Outcome::LaunchFailure && absent.exitCode == -1);
        CHECK(!absent.problem.isEmpty() && absent.mutation == Mutation::NotAttempted);
    });
    suite.run("a crashed operation retains its output without a normal exit code", [] {
        Repository repo;
        const auto fake = repo.fakeGit("printf 'crash stdout'\nprintf 'crash stderr' >&2\nkill -KILL $$\n");
        GitAdapter adapter(repo.program(fake));
        const auto result = run(adapter, repo.readRequest());
        CHECK(result.outcome == Outcome::ProcessFailure && result.exitCode == -1);
        CHECK(result.standardOutput == "crash stdout" && result.standardError == "crash stderr");
        CHECK(result.mutation == Mutation::NotAttempted && result.problem.code == ProblemCode::GitCrashed);
    });
    suite.run("destruction before queued launch publishes no result or mutation", [] {
        Repository repo;
        const auto before = repo.git({"ls-files", "--stage", "-z"});
        QObject receiver;
        int completions = 0;
        {
            GitAdapter adapter(repo.program());
            QObject::connect(&adapter, &GitAdapter::finished, &receiver, [&](const Result &) { ++completions; });
            CHECK(adapter.start(repo.request(Operation::StageFile, "tracked.txt")));
        }
        QCoreApplication::processEvents();
        CHECK(completions == 0);
        // A later request's result is queued behind anything the destroyed adapter posted, so its
        // arrival shows that no completion of the destroyed adapter is still on its way.
        GitAdapter later(repo.program());
        CHECK(run(later, repo.request(Operation::InspectIndex, "tracked.txt")).outcome == Outcome::Success);
        CHECK(completions == 0);
        CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
    });
    suite.run("failed mutation retains operation output, stderr and exit through reinspection", [] {
        Repository repo;
        const auto fake =
            repo.fakeGit(": > .after-add\nprintf 'operation stdout'\nprintf 'operation stderr' >&2\nexit 23\n");
        GitAdapter adapter(repo.program(fake));
        const auto result = run(adapter, repo.request(Operation::StageFile, "tracked.txt"));
        CHECK(result.outcome == Outcome::ExitFailure && result.exitCode == 23);
        CHECK(result.standardOutput == "operation stdout" && result.standardError == "operation stderr");
        CHECK(hasProblem(result.problem, ProblemCode::GitExitCode, {QStringLiteral("23")})
              && result.mutation == Mutation::ObservedChanged);
        CHECK(result.indexBefore.contains(QByteArray(40, 'a')) && result.indexAfter.contains(QByteArray(40, 'b')));
    });
    suite.run("failed reinspection retains the original failed operation diagnostics", [] {
        Repository repo;
        const auto fake =
            repo.fakeGit(": > .after-add\nprintf 'original stdout'\nprintf 'original stderr' >&2\nexit 23\n",
                         "printf 'inspection stdout'\nprintf 'inspection stderr' >&2\nexit 29\n");
        GitAdapter adapter(repo.program(fake));
        const auto result = run(adapter, repo.request(Operation::StageFile, "tracked.txt"));
        CHECK(result.outcome == Outcome::ExitFailure && result.exitCode == 23);
        CHECK(result.mutation == Mutation::Uncertain && result.standardOutput == "original stdout");
        CHECK(result.standardError.contains("original stderr") && result.standardError.contains("inspection stderr"));
        CHECK(result.indexAfter == "inspection stdout");
        CHECK(hasProblem(result.problem, ProblemCode::GitExitCode, {QStringLiteral("23")})
              && result.problem.code == ProblemCode::GitReinspectionFailed);
    });
    suite.run("successful child exit cannot claim staging when index inspection fails", [] {
        Repository repo;
        const auto fake =
            repo.fakeGit(": > .after-add\nprintf 'successful add stdout'\n", "printf 'bad index'\nexit 29\n");
        GitAdapter adapter(repo.program(fake));
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
            GitAdapter adapter(repo.program(fake));
            const auto result = run(adapter, repo.request(Operation::StageFile, "tracked.txt"));
            CHECK(result.outcome == Outcome::InspectionFailure && result.exitCode == 0);
            CHECK(result.mutation
                  == (entry.isEmpty() || entry.contains(" 2\t") ? Mutation::ObservedChanged : Mutation::Uncertain));
        }
    });
    suite.run("cancellation before queued launch completes once without mutation", [] {
        Repository repo;
        GitAdapter adapter(repo.program());
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
}

void addCancellationAndShutdownCases(test_support::Suite &suite)
{
    suite.run("cancellation after mutation reinspects while preserving operation output", [] {
        // Cancel either the running mutation or its post-exit reinspection.
        for (const int exitCode : {-1, 0, 128}) {
            SCOPE(exitCode < 0 ? "cancelled while Git stages"
                               : "cancelled while the index is reinspected, Git exit " + std::to_string(exitCode));
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
            std::optional<ScriptGate> release;
            if (finished) {
                release.emplace(repo.path(".release-inspection"));
                inspection = ": > .inspection-started\nread -r line < .release-inspection\n";
            }
            inspection += "printf '100644 bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb 0\\ttracked.txt\\0'\n";
            const auto fake = repo.fakeGit(operation, inspection);
            GitAdapter adapter(repo.program(fake));
            Completion completion(adapter);
            const WhenCreated cancelThere(repo.path(finished ? ".inspection-started" : ".operation-started"), [&] {
                adapter.cancel();
                adapter.cancel();
                if (release) release->open();
            });
            const auto result = completion.run(repo.request(Operation::StageFile, "tracked.txt"));
            const auto expected = !finished ? Outcome::Cancelled
                : exitCode == 0             ? Outcome::Success
                                            : Outcome::ExitFailure;
            CHECK(result.outcome == expected && result.cancellationRequested);
            CHECK(result.mutation == Mutation::ObservedChanged);
            CHECK(result.standardOutput == "before cancellation" && !result.indexAfter.isEmpty());
            if (finished) {
                CHECK(result.exitCode == exitCode);
                CHECK(result.problem
                      == (exitCode == 0 ? Problem{} : Problem{ProblemCode::GitExitCode, {QStringLiteral("128")}}));
                CHECK(result.standardError == (exitCode == 0 ? QByteArray{} : QByteArray("fatal: index.lock exists")));
            }
            adapter.cancel();
            const auto next = run(adapter, repo.request(Operation::InspectIndex, "tracked.txt"));
            CHECK(next.outcome == Outcome::Success && !next.cancellationRequested);
        }
    });
    suite.run("destroying the adapter while Git stages stops Git with SIGTERM before SIGKILL", [] {
        Repository repo;
        // Git removes index.lock when SIGTERM stops it; this stand-in records that it got the chance.
        const auto fake = repo.fakeGit(": > .after-add\ntrap ': > .terminated; exit 143' TERM\n"
                                       ": > .operation-started\nsleep 30 & wait $!\n");
        auto adapter = std::make_unique<GitAdapter>(repo.program(fake));
        bool destroyed = false;
        const WhenCreated destroy(repo.path(".operation-started"), [&] {
            adapter.reset();
            destroyed = true;
        });
        CHECK(adapter->start(repo.request(Operation::StageFile, "tracked.txt")));
        test_support::waitUntil([&] { return destroyed; }, "the staging operation did not start");
        CHECK(QFile::exists(repo.path(".terminated")));
    });
    suite.run("an owner that stops its Git processes waits one grace period for all of them", [] {
        Repository repo;
        // Each stand-in ignores SIGTERM, so only SIGKILL after the grace period ends it.
        const auto fake = repo.fakeGit(": > .after-add\ntrap '' TERM\n: > .started-$$\nsleep 30\n");
        auto program = repo.program(fake);
        program.processes = std::make_shared<GitProcesses>();
        std::vector<std::unique_ptr<GitAdapter>> adapters;
        for (int count = 0; count < 3; ++count) {
            adapters.push_back(std::make_unique<GitAdapter>(program));
            CHECK(adapters.back()->start(repo.request(Operation::StageFile, "tracked.txt")));
        }
        test_support::waitUntil(
            [&] { return QDir(repo.root()).entryList({".started-*"}, QDir::Files | QDir::Hidden).size() == 3; },
            "the three staging operations did not start");
        QElapsedTimer clock;
        clock.start();
        program.processes->stopAll();
        adapters.clear();
        // An adapter alone waits 500 ms after SIGTERM; three in turn would take 1500 ms.
        CHECK_LT(clock.elapsed(), 1200);
    });
}

void addRunnerAndEnvelopeCases(test_support::Suite &suite)
{
    suite.run("real Git and a scripted answer keep the same runner contract", [] {
        Repository repo;
        GitAdapter adapter(repo.program());
        test_support::ScriptedGit passThrough(repo.program(), [](const Request &) { return std::nullopt; });
        test_support::ScriptedGit scripted(
            repo.program(),
            [](const Request &) -> std::optional<Result> {
                Result failed;
                failed.outcome = Outcome::ExitFailure;
                return failed;
            },
            nullptr);
        for (GitRunner *runner : std::initializer_list<GitRunner *>{&adapter, &passThrough, &scripted}) {
            test_support::AsyncCompletion<GitRunner, Result> completion(*runner, &GitRunner::finished);
            const auto result = completion.run(repo.readRequest());
            CHECK(result.request.operation == Operation::ReadObject && result.request.sessionGeneration == 71);
            runner->cancel();
            CHECK(!runner->busy());
        }
    });
    for (const auto mutation :
         {Mutation::NotAttempted, Mutation::ObservedUnchanged, Mutation::ObservedChanged, Mutation::Uncertain}) {
        for (const auto outcome : {Outcome::Success, Outcome::ExitFailure, Outcome::TimedOut}) {
            suite.run("scripted cancellation preserves the observed mutation " + std::to_string(int(mutation))
                          + " and outcome " + std::to_string(int(outcome)),
                      [mutation, outcome] {
                          Repository repo;
                          test_support::ScriptedGit runner(repo.program(), [=](const Request &) {
                              Result result;
                              result.outcome = outcome;
                              result.mutation = mutation;
                              result.problem = {ProblemCode::GitDeadline, {QStringLiteral("30")}};
                              result.indexAfter = "observed index";
                              return std::optional<Result>(result);
                          });
                          test_support::AsyncCompletion<GitRunner, Result> completion(runner, &GitRunner::finished);
                          const auto request = repo.request(Operation::StageFile, "tracked.txt");
                          CHECK(runner.start(request));
                          CHECK(runner.busy() && completion.results.empty());
                          CHECK(!runner.start(request));
                          runner.cancel();
                          runner.cancel();
                          CHECK(runner.busy() && completion.results.empty());
                          const auto result = completion.wait();
                          CHECK(result.cancellationRequested && result.mutation == mutation);
                          CHECK(result.outcome == (mutation == Mutation::NotAttempted ? Outcome::Cancelled : outcome));
                          CHECK(result.problem.code
                                == (mutation == Mutation::NotAttempted ? ProblemCode::GitCancelled
                                                                       : ProblemCode::GitDeadline));
                          CHECK(result.indexAfter == "observed index");
                          CHECK(result.request.path == request.path && result.request.sessionGeneration == 71);
                          runner.cancel();
                          test_support::AsyncCompletion<GitRunner, Result> later(runner, &GitRunner::finished);
                          CHECK(!later.run(request).cancellationRequested);
                      });
        }
    }
    suite.run("destroying a scripted runner drops its queued answer", [] {
        Repository repo;
        QObject receiver;
        int completions = 0;
        auto runner = std::make_unique<test_support::ScriptedGit>(
            repo.program(), [](const Request &) { return std::optional<Result>(Result{}); });
        QObject::connect(runner.get(), &GitRunner::finished, &receiver, [&](const Result &) { ++completions; });
        CHECK(runner->start(repo.readRequest()));
        runner.reset();
        QCoreApplication::processEvents();
        CHECK_EQ(completions, 0);
    });
    suite.run("only staging is a mutation", [] {
        for (const auto operation :
             {Operation::DiscoverRepository, Operation::InspectIndex, Operation::ReadObject, Operation::ReadAttributes,
              Operation::MaterializeSource, Operation::CheckParentStaging, Operation::CanonicalizeText})
            CHECK(!isMutation(operation));
        CHECK(isMutation(Operation::StageFile));
        CHECK(!isMutation(std::bit_cast<Operation>(int{999})));
    });
    suite.run("output limit keeps a bounded incomplete prefix and a separate inspection budget", [] {
        Repository repo;
        const auto fake =
            repo.fakeGit(": > .after-add\nprintf 'operation prefix'\n/usr/bin/head -c 9000000 /dev/zero\n");
        GitAdapter adapter(repo.program(fake));
        const auto result = run(adapter, repo.request(Operation::StageFile, "tracked.txt"));
        CHECK(result.outcome == Outcome::OutputLimit && result.outputIncomplete);
        CHECK(result.standardOutput.startsWith("operation prefix"));
        CHECK(result.standardOutput.size() == static_cast<qsizetype>(branch_truce::kMaxGitReadBytes));
        CHECK(result.problem.code == ProblemCode::GitOutputLimit
              && result.problem.arguments.value(0)
                  == QString::number(static_cast<qulonglong>(branch_truce::kMaxGitReadBytes)));
        CHECK(result.mutation == Mutation::ObservedChanged && result.indexAfter.contains(QByteArray(40, 'b')));
    });
    suite.run("measured git output envelope accepts the limit and rejects one extra byte", [] {
        for (const qsizetype bytes : {static_cast<qsizetype>(branch_truce::kMaxGitReadBytes) - 1,
                                      static_cast<qsizetype>(branch_truce::kMaxGitReadBytes),
                                      static_cast<qsizetype>(branch_truce::kMaxGitReadBytes) + 1}) {
            Repository repo;
            const auto fake = repo.fakeGit(QByteArray(": > .after-add\n/usr/bin/head -c ") + QByteArray::number(bytes)
                                           + " /dev/zero\n");
            GitAdapter adapter(repo.program(fake));
            const auto result = run(adapter, repo.request(Operation::StageFile, "tracked.txt"));
            if (bytes <= static_cast<qsizetype>(branch_truce::kMaxGitReadBytes)) {
                CHECK(result.outcome == Outcome::Success && !result.outputIncomplete);
                CHECK(result.standardOutput.size() == bytes);
            } else {
                CHECK(result.outcome == Outcome::OutputLimit && result.outputIncomplete);
                CHECK(result.standardOutput.size() == static_cast<qsizetype>(branch_truce::kMaxGitReadBytes));
                CHECK(result.problem.code == ProblemCode::GitOutputLimit
                      && result.problem.arguments.value(0)
                          == QString::number(static_cast<qulonglong>(branch_truce::kMaxGitReadBytes)));
                CHECK(result.mutation == Mutation::ObservedChanged);
            }
        }
    });
    suite.run("the operation deadline cancels the process without reporting success", [] {
        Repository repo;
        const auto fake = repo.fakeGit("printf 'deadline prefix'\nsleep 60\n");
        // A short deadline, so the case does not wait the production 30 seconds.
        GitAdapter adapter(repo.program(fake, 300));
        Completion completion(adapter);
        const auto result = completion.run(repo.readRequest());
        CHECK(result.outcome == Outcome::TimedOut && result.mutation == Mutation::NotAttempted);
        CHECK(result.standardOutput == "deadline prefix");
        CHECK(result.problem == Problem(ProblemCode::GitDeadline, {QStringLiteral("0.3")}));
    });
    suite.run("expanding canonical text is size-validated before reading", [] {
        const auto gitLimit = QString::number(static_cast<qulonglong>(branch_truce::kMaxGitReadBytes));
        Repository repo;
        writeFile(repo.path(".gitattributes"), "tracked.txt working-tree-encoding=UTF-16LE\n");
        const auto objects = repo.git({"count-objects", "-v"});
        const auto index = repo.git({"ls-files", "--stage", "-z"});
        const auto inputBlock = QByteArray::fromHex("2d4e").repeated(39) + QByteArray::fromHex("0a00");
        const auto canonicalBlock = QByteArray::fromHex("e4b8ad").repeated(39) + '\n';
        GitAdapter adapter(repo.program());
        for (const auto inputSize : {2000000, 3000000, 6000000}) {
            auto request = repo.request(Operation::CanonicalizeText, "tracked.txt");
            request.input = inputBlock.repeated(inputSize / inputBlock.size());
            CHECK(request.input.size() == inputSize);
            const auto result = run(adapter, request);
            CHECK(!result.outputIncomplete && result.mutation == Mutation::NotAttempted);
            if (inputSize == 2000000) {
                const auto expected = canonicalBlock.repeated(inputSize / inputBlock.size());
                CHECK(result.outcome == Outcome::Success);
                CHECK(result.standardOutput == request.input && result.canonicalBytes == expected);
                CHECK(result.canonicalObjectId
                      == repo.git({"hash-object", "--no-filters", "--stdin"}, expected).trimmed());
            } else if (inputSize == 3000000) {
                CHECK(result.outcome == Outcome::Rejected && result.canonicalBytes.isEmpty());
                CHECK(result.problem
                      == Problem(ProblemCode::GitCanonicalTooLarge, {QStringLiteral("4425000"), gitLimit}));
            } else {
                CHECK(result.outcome == Outcome::Rejected && result.canonicalBytes.isEmpty());
                CHECK(result.problem == Problem(ProblemCode::GitInputTooLarge, {QStringLiteral("6000000"), gitLimit}));
            }
            CHECK(repo.git({"count-objects", "-v"}) == objects);
            CHECK(repo.git({"ls-files", "--stage", "-z"}) == index);
        }
        checkConversionCleanup(repo);
    });
    suite.run("canonical reads retain the full output budget after preflight", [] {
        Repository repo;
        writeFile(repo.path(".gitattributes"), "tracked.txt text eol=lf\n");
        const auto objects = repo.git({"count-objects", "-v"});
        auto request = repo.request(Operation::CanonicalizeText, "tracked.txt");
        request.input = QByteArray(static_cast<qsizetype>(branch_truce::kMaxGitReadBytes), 'a');
        for (qsizetype i = 79; i < request.input.size(); i += 80) request.input[i] = '\n';
        const auto expected = repo.git({"hash-object", "--no-filters", "--stdin"}, request.input).trimmed();
        GitAdapter adapter(repo.program());
        const auto result = run(adapter, request);
        CHECK(result.outcome == Outcome::Success && !result.outputIncomplete);
        CHECK(result.standardOutput == request.input && result.canonicalBytes == request.input);
        CHECK(result.canonicalObjectId == expected && result.mutation == Mutation::NotAttempted);
        CHECK(repo.git({"count-objects", "-v"}) == objects);
        checkConversionCleanup(repo);
    });
    suite.run("a SHA-256 repository is discovered, classified and staged like a SHA-1 one", [] {
        Repository repo({"--object-format=sha256"});
        CHECK_EQ(repo.git({"rev-parse", "--show-object-format"}).trimmed(), QByteArray("sha256"));
        const auto history = conflictHistory(repo);
        CHECK_EQ(history.main.size(), 64);
        repo.git({"merge", "--no-edit", "side"}, {}, 1);
        GitAdapter adapter(repo.program());
        const auto discovered = run(adapter, repo.request(Operation::DiscoverRepository));
        checkOperationIdentity(discovered, history, RepositoryOperation::Merge);
        CHECK_EQ(discovered.indexEntries.size(), 3);
        for (const auto &entry : discovered.indexEntries) CHECK_EQ(entry.objectId.size(), 64);
        writeFile(repo.path("tracked.txt"), "manual result\n");
        ConflictClassifier classifier(repo.program());
        const auto classified = run(classifier, {repo.root(), 71, 93});
        CHECK_EQ(classified.outcome, Outcome::Success);
        CHECK_EQ(classified.conflicts.size(), 1);
        CHECK(classified.conflicts.front().editable());
        CHECK_EQ(classified.conflicts.front().sources.size(), 3);
        const auto staged = run(adapter, repo.request(Operation::StageFile, "tracked.txt"));
        CHECK_EQ(staged.outcome, Outcome::Success);
        CHECK_EQ(staged.mutation, Mutation::ObservedChanged);
        const auto expected = repo.git({"hash-object", "--stdin"}, "manual result\n").trimmed();
        CHECK_EQ(expected.size(), 64);
        CHECK_EQ(staged.indexAfter, "100644 " + expected + " 0\ttracked.txt" + QByteArray(1, '\0'));
        CHECK(repo.git({"ls-files", "--unmerged", "-z"}).isEmpty());
    });
    suite.run("standard error shares the output limit and stops a flooding operation", [] {
        Repository repo;
        const auto fake =
            repo.fakeGit(": > .after-add\nprintf 'error prefix' >&2\n/usr/bin/head -c 9000000 /dev/zero >&2\n");
        GitAdapter adapter(repo.program(fake));
        const auto result = run(adapter, repo.request(Operation::StageFile, "tracked.txt"));
        CHECK_EQ(result.outcome, Outcome::OutputLimit);
        CHECK(result.outputIncomplete);
        CHECK(result.standardError.startsWith("error prefix"));
        CHECK_LE(result.standardError.size(), static_cast<qsizetype>(branch_truce::kMaxGitReadBytes) + 4096);
        CHECK(result.problem.code == ProblemCode::GitOutputLimit
              && result.problem.arguments.value(0)
                  == QString::number(static_cast<qulonglong>(branch_truce::kMaxGitReadBytes)));
        CHECK_EQ(result.mutation, Mutation::ObservedChanged);
    });
    // The failure-path cases run against the scripted Git, so it must answer the success paths
    // the way real Git does. Each row runs the same requests and expectations.
    for (const bool scripted : {false, true}) {
        suite.run(std::string("staging, reading and inspection succeed alike through ")
                      + (scripted ? "the scripted Git" : "real Git"),
                  [scripted] {
                      Repository repo;
                      const auto executable =
                          scripted ? repo.fakeGit(": > .after-add\n") : test_support::gitExecutable();
                      writeFile(repo.path("tracked.txt"), "after\n");
                      GitAdapter adapter(repo.program(executable));
                      const auto staged = run(adapter, repo.request(Operation::StageFile, "tracked.txt"));
                      CHECK_EQ(staged.outcome, Outcome::Success);
                      CHECK_EQ(staged.exitCode, 0);
                      CHECK_EQ(staged.mutation, Mutation::ObservedChanged);
                      CHECK(!staged.outputIncomplete && staged.problem.isEmpty());
                      for (const auto &index : {staged.indexBefore, staged.indexAfter}) {
                          CHECK_EQ(index.count('\0'), 1);
                          CHECK(index.startsWith("100644 ") && index.endsWith(QByteArray(" 0\ttracked.txt\0", 15)));
                      }
                      CHECK_NE(staged.indexBefore, staged.indexAfter);
                      const auto read = run(adapter, repo.readRequest());
                      CHECK_EQ(read.outcome, Outcome::Success);
                      CHECK_EQ(read.exitCode, 0);
                      CHECK_EQ(read.mutation, Mutation::NotAttempted);
                      CHECK(!read.outputIncomplete);
                      const auto inspected = run(adapter, repo.request(Operation::InspectIndex, "tracked.txt"));
                      CHECK_EQ(inspected.outcome, Outcome::Success);
                      CHECK_EQ(inspected.indexEntries.size(), 1);
                      CHECK_EQ(inspected.indexEntries.front().path, QByteArray("tracked.txt"));
                      CHECK_EQ(inspected.indexEntries.front().stage, 0u);
                      CHECK_EQ(inspected.indexEntries.front().mode, QByteArray("100644"));
                      CHECK_EQ(inspected.standardOutput, staged.indexAfter);
                  });
    }
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const auto arguments = test_support::configure(argc, argv);
    if (arguments.size() != 2) {
        QTextStream(stderr) << "usage: git-adapter-tests [--filter TEXT] <parent-staging-fixture>\n";
        return 2;
    }
    parentStagingFixture = QFile::decodeName(QByteArray::fromStdString(arguments[1]));
    test_support::Suite suite("GitAdapterTests");
    addRepositoryDiscoveryCases(suite);
    addReadAndStageCases(suite);
    addPathAdmissionCases(suite);
    addFilterPolicyCases(suite);
    addLocalHelperPolicyCases(suite);
    addObjectStoragePolicyCases(suite);
    addParentAndFailureCases(suite);
    addCancellationAndShutdownCases(suite);
    addRunnerAndEnvelopeCases(suite);
    return suite.report();
}
