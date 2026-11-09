// TextConversion against real Git: decoding sources, preparing a result Git will store as the
// reviewed bytes, and verifying what was published, across the supported text formats.
#include "TestHarness.h"
#include "fixtures/conversions/ConversionCases.h"
#include "git/GitFixtures.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>

#include <string>
#include <utility>

using namespace git_fixture;

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    test_support::configure(argc, argv);
    test_support::Suite suite("TextConversionTests");
    for (const auto &format : conversion_fixtures::accepted) {
        suite.run(
            std::string("conversion format matrix preserves reviewed bytes through real Git publication: ")
                + format.name,
            [&format] {
                Repository repo;
                if (!format.attributes.isEmpty())
                    writeFile(repo.path(".gitattributes"), "tracked.txt " + format.attributes + '\n');
                if (!format.configuration.isEmpty())
                    repo.git({"config", format.configuration[0], format.configuration[1]});
                const auto object = repo.git({"hash-object", "-w", "--stdin"}, format.canonicalSource).trimmed();
                const auto nul = QByteArray(1, '\0');
                repo.git({"update-index", "-z", "--index-info"},
                         "0 " + QByteArray(40, '0') + "\ttracked.txt" + nul + format.mode + ' ' + object
                             + " 2\ttracked.txt" + nul + format.mode + ' ' + object + " 3\ttracked.txt" + nul);
                GitAdapter adapter(test_support::gitExecutable());
                const auto captured = run(adapter, repo.request(Operation::InspectIndex, "tracked.txt"));
                CHECK(captured.outcome == Outcome::Success && captured.indexEntries.size() == 2);
                auto sourceRequest = textRequest(repo, TextOperation::ReadSource, object);
                sourceRequest.stages = captured.indexEntries;
                TextConversion conversion(test_support::gitExecutable());
                const auto source = run(conversion, sourceRequest);
                CHECK(source.outcome == Outcome::Success && source.text);
                CHECK(source.text->canonicalBytes == format.canonicalSource
                      && source.text->canonicalObjectId == object);
                CHECK(source.text->document.originalBytes == format.checkoutSource);
                CHECK(source.text->document.utf8 == format.displayedSource);
                CHECK(source.text->mode == format.mode && source.text->sourceMode == format.mode);
                if (!format.configuration.isEmpty()) {
                    CHECK(source.text->policy.configuration.value(format.configuration[0].toLatin1())
                          == format.configuration[1].toLatin1());
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
                CHECK(prepared.text->canonicalBytes == format.canonicalResult
                      && prepared.text->policy == source.text->policy);
                const auto expectedObject =
                    repo.git({"hash-object", "--no-filters", "--stdin"}, format.canonicalResult).trimmed();
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
                CHECK(published.text->canonicalBytes == format.canonicalResult
                      && published.text->canonicalObjectId == expectedObject);
                CHECK(published.text->document.utf8 == format.editedUtf8
                      && published.text->document.originalBytes == format.checkoutResult);
                CHECK(published.text->document.format == format.resultFormat && published.text->mode == format.mode);
                CHECK(repo.git({"show", ":tracked.txt"}) == format.canonicalResult);
                QFile written(repo.path("tracked.txt"));
                CHECK(written.open(QIODevice::ReadOnly) && written.readAll() == format.checkoutResult);
                written.close();
                checkConversionCleanup(repo);
            });
    }
    for (const auto &invalid : conversion_fixtures::rejectedSources) {
        suite.run(std::string("invalid source bytes reject without accepted text or writes: ") + invalid.name,
                  [&invalid] {
                      Repository repo;
                      if (!invalid.attributes.isEmpty())
                          writeFile(repo.path(".gitattributes"), "tracked.txt " + invalid.attributes + '\n');
                      const auto object = repo.git({"hash-object", "-w", "--stdin"}, invalid.canonical).trimmed();
                      const auto before = repo.git({"ls-files", "--stage", "-z"});
                      TextConversion conversion(test_support::gitExecutable());
                      const auto rejected = run(conversion, textRequest(repo, TextOperation::ReadSource, object));
                      CHECK(rejected.outcome == Outcome::Rejected && !rejected.text && !rejected.diagnostic.isEmpty());
                      CHECK(rejected.failure == invalid.failure);
                      CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
                      checkConversionCleanup(repo);
                  });
    }
    for (const auto attributes : conversion_fixtures::rejectedAttributes) {
        suite.run(
            std::string("contradictory attributes reject reads and prepared results without writes: ") + attributes,
            [attributes] {
                Repository repo;
                writeFile(repo.path(".gitattributes"), QByteArray("tracked.txt ") + attributes + '\n');
                const auto object = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
                const auto before = repo.git({"ls-files", "--stage", "-z"});
                TextConversion conversion(test_support::gitExecutable());
                for (const auto operation : {TextOperation::ReadSource, TextOperation::PrepareResult}) {
                    auto request = textRequest(repo, operation, object);
                    request.editedUtf8 = "reviewed\n";
                    const auto rejected = run(conversion, request);
                    CHECK(rejected.outcome == Outcome::Rejected && !rejected.text && !rejected.diagnostic.isEmpty());
                    CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
                }
                checkConversionCleanup(repo);
            });
    }
    for (const auto &invalid : conversion_fixtures::rejectedEdits) {
        suite.run(std::string("invalid edited text rejects without accepted text or writes: ") + invalid.name,
                  [&invalid] {
                      Repository repo;
                      const auto object = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
                      const auto before = repo.git({"ls-files", "--stage", "-z"});
                      TextConversion conversion(test_support::gitExecutable());
                      auto request = textRequest(repo, TextOperation::PrepareResult, object);
                      request.editedUtf8 = invalid.bytes;
                      const auto rejected = run(conversion, request);
                      CHECK(rejected.outcome == Outcome::Rejected && !rejected.text && !rejected.diagnostic.isEmpty());
                      CHECK(repo.git({"ls-files", "--stage", "-z"}) == before);
                      QFile untouched(repo.path("tracked.txt"));
                      CHECK(untouched.open(QIODevice::ReadOnly) && untouched.readAll() == "before\n");
                      untouched.close();
                      checkConversionCleanup(repo);
                  });
    }
    suite.run("a prepared result that clean and checkout would change is rejected without writes", [] {
        for (const bool bigEndian : {false, true}) {
            SCOPE(bigEndian ? "UTF-16 byte order" : "autocrlf changes LF");
            Repository repo;
            if (bigEndian)
                writeFile(repo.path(".gitattributes"), "tracked.txt text eol=lf working-tree-encoding=UTF-16\n");
            else {
                repo.git({"config", "core.autocrlf", "true"});
                repo.git({"config", "core.safecrlf", "false"});
            }
            const auto object = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
            const auto before = repo.git({"ls-files", "--stage", "-z"});
            TextConversion conversion(test_support::gitExecutable());
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
        TextConversion conversion(test_support::gitExecutable());
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
        GitAdapter adapter(test_support::gitExecutable());
        const auto staged = run(adapter, repo.request(Operation::StageFile, "tracked.txt"));
        CHECK(staged.outcome == Outcome::Success);
        CHECK(staged.indexAfter == "100644 " + object + " 0\ttracked.txt" + QByteArray(1, '\0'));
        auto verify = textRequest(repo, TextOperation::VerifyPublished, {});
        verify.expected = unchanged.text;
        const auto published = run(conversion, verify);
        CHECK(published.outcome == Outcome::Success && published.text);
        CHECK(published.text->document.utf8 == request.editedUtf8 && published.text->canonicalBytes == "$Id$\n");
        SCOPE("ident expansion with staged canonical token");
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
            TextConversion conversion(test_support::gitExecutable());
            const auto source = run(conversion, request);
            CHECK(source.outcome == Outcome::Success && source.text);
            CHECK(source.text->mode == mode && source.text->sourceMode == mode);
            request.operation = TextOperation::PrepareResult;
            request.editedUtf8 = "executable result\n";
            const auto prepared = run(conversion, request);
            CHECK(prepared.outcome == Outcome::Success && prepared.text && prepared.text->mode == mode);
            writeFile(repo.path("tracked.txt"), prepared.text->document.originalBytes, mode == "100755");
            GitAdapter adapter(test_support::gitExecutable());
            CHECK(run(adapter, repo.request(Operation::StageFile, "tracked.txt")).outcome == Outcome::Success);
            auto verify = textRequest(repo, TextOperation::VerifyPublished, {});
            verify.expected = prepared.text;
            CHECK(run(conversion, verify).outcome == Outcome::Success);
            CHECK(repo.git({"ls-files", "--stage", "-z"}).startsWith(mode + ' '));
        }
        Repository repo;
        const auto object = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
        TextConversion conversion(test_support::gitExecutable());
        using Stages = QList<IndexEntry>;
        const std::pair<const char *, void (*)(Stages &)> damaged[] = {
            {"no stage 2", [](Stages &stages) { stages.removeFirst(); }},
            {"no stage 3", [](Stages &stages) { stages.removeLast(); }},
            {"a repeated stage", [](Stages &stages) { stages.append(stages.front()); }},
            {"a stage for another path", [](Stages &stages) { stages.back().path = "other.txt"; }},
            {"a symbolic link stage", [](Stages &stages) { stages.back().mode = "120000"; }},
            {"stages that disagree on the executable bit", [](Stages &stages) { stages.back().mode = "100755"; }},
            {"stage number 4", [](Stages &stages) { stages.back().stage = 4; }},
            {"an invalid object id", [](Stages &stages) { stages.back().objectId = "invalid"; }}};
        for (const auto &[name, damage] : damaged) {
            SCOPE(name);
            auto request = textRequest(repo, TextOperation::PrepareResult, object);
            request.editedUtf8 = "reviewed\n";
            damage(request.stages);
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
        TextConversion conversion(test_support::gitExecutable());
        auto request = textRequest(repo, TextOperation::PrepareResult, object);
        request.editedUtf8 = "reviewed\n";
        const auto prepared = run(conversion, request);
        CHECK(prepared.outcome == Outcome::Success && prepared.text);
        writeFile(repo.path("tracked.txt"), prepared.text->document.originalBytes);
        GitAdapter adapter(test_support::gitExecutable());
        CHECK(run(adapter, repo.request(Operation::StageFile, "tracked.txt")).outcome == Outcome::Success);
        auto verify = textRequest(repo, TextOperation::VerifyPublished, {});
        verify.expected = prepared.text;
        CHECK(run(conversion, verify).outcome == Outcome::Success);
        const auto wrong = repo.git({"hash-object", "-w", "--stdin"}, "different\n").trimmed();
        repo.git({"update-index", "--cacheinfo", "100644", QString::fromLatin1(wrong), "tracked.txt"});
        const auto wrongObject = run(conversion, verify);
        CHECK(wrongObject.outcome == Outcome::Rejected && !wrongObject.text);
        CHECK(wrongObject.diagnostic.contains("canonical identity"));
        repo.git({"update-index", "--cacheinfo", "100644", QString::fromLatin1(prepared.text->canonicalObjectId),
                  "tracked.txt"});
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
        verify.expected->worktreeRoot = repo.executables();
        const auto wrongWorktree = run(conversion, verify);
        CHECK(wrongWorktree.outcome == Outcome::Rejected && !wrongWorktree.text);
        CHECK(wrongWorktree.diagnostic.contains("worktree identity changed"));
    });
    return suite.report();
}
