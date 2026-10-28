#include "app/ConflictCapture.h"
#include "TestHarness.h"
#include <functional>

using namespace branch_truce;

namespace {
const QByteArray path = "dir/file.txt";
const QByteArray mode = "100644";
const QString root = QStringLiteral("/work/repository");

git::RepositoryContext repository(git::RepositoryOperation operation)
{
    git::RepositoryContext context;
    context.worktreeRoot = root;
    context.gitDirectory = root + QStringLiteral("/.git");
    context.operation = operation;
    context.supported = true;
    context.baseCommitId = "base-commit";
    if (operation == git::RepositoryOperation::Rebase) {
        context.stage2 = {"onto-commit", QStringLiteral("Current rebased state: onto-commit")};
        context.stage3 = {"replayed-commit", QStringLiteral("Commit being replayed: replayed-commit")};
        context.headName = "refs/heads/side";
        context.originalHead = "original-head";
        context.onto = "onto-commit";
    } else {
        context.stage2 = {"main-commit", QStringLiteral("Current checkout: main-commit")};
        context.stage3 = {"side-commit", QStringLiteral("Merged commit: side-commit")};
    }
    return context;
}

storage::TextDocument document(const QByteArray &utf8, const QByteArray &bytes, storage::TextFormat format = {})
{
    return {utf8, bytes, format};
}

git::ConversionPolicy policy()
{
    git::ConversionPolicy result;
    result.attributes.insert("text", "auto");
    result.configuration.insert("core.autocrlf", "false");
    return result;
}

// One stage and its source, both of which agree with the conflict.
void addStage(git::Conflict &conflict, unsigned int stage, const QByteArray &text)
{
    const auto objectId = "object-" + QByteArray::number(stage);
    conflict.stages.push_back({path, mode, objectId, stage});
    git::ConvertedText source;
    source.worktreeRoot = root;
    source.path = path;
    source.mode = mode;
    source.sourceMode = mode;
    source.canonicalObjectId = objectId;
    source.canonicalBytes = text;
    source.document = document(text, text);
    source.policy = policy();
    conflict.sources.push_back(source);
}

git::Conflict conflict(std::initializer_list<unsigned int> stages)
{
    git::Conflict result;
    result.path = path;
    result.support = git::ConflictSupport::SupportedText;
    result.mode = mode;
    for (const auto stage : stages) addStage(result, stage, "stage " + QByteArray::number(stage) + "\n");
    result.result = document("manual result\n", "manual result\n");
    result.resultIdentity = SnapshotFileIdentity{1, 2, 0100644, 14, 3, 4, 5, 6};
    return result;
}

void checkSources(const FileSnapshots &captured, const git::RepositoryContext &context)
{
    CHECK(captured.earlier && captured.stage2 && captured.stage3);
    CHECK(captured.earlier->objectId == "object-1" && captured.earlier->rawBytes == "stage 1\n");
    CHECK(captured.earlier->worktree.utf8 == "stage 1\n" && captured.earlier->mode == "100644");
    CHECK(captured.earlier->commitId == std::optional<std::string>("base-commit"));
    CHECK(captured.earlier->label == "Common earlier version");
    CHECK(captured.stage2->objectId == "object-2" && captured.stage2->worktree.utf8 == "stage 2\n");
    CHECK(captured.stage2->commitId == context.stage2.commitId.toStdString());
    CHECK(captured.stage2->label == context.stage2.label.toStdString());
    CHECK(captured.stage3->objectId == "object-3" && captured.stage3->worktree.utf8 == "stage 3\n");
    CHECK(captured.stage3->commitId == context.stage3.commitId.toStdString());
    CHECK(captured.stage3->label == context.stage3.label.toStdString());
}

void checkShared(const FileSnapshots &captured, const git::RepositoryContext &context)
{
    CHECK(captured.path == "dir/file.txt" && captured.resultMode == "100644");
    CHECK(captured.launchResult.text.utf8 == "manual result\n" && captured.launchResult.text.bytes == "manual result\n");
    CHECK(captured.launchResult.identity == (SnapshotFileIdentity{1, 2, 0100644, 14, 3, 4, 5, 6}));
    CHECK(captured.provenance.sessionId == "session" && captured.provenance.worktreeRoot == root.toStdString());
    CHECK(captured.provenance.gitDirectory == context.gitDirectory.toStdString());
    CHECK(captured.provenance.stage2CommitId == context.stage2.commitId.toStdString());
    CHECK(captured.provenance.stage3CommitId == context.stage3.commitId.toStdString());
    CHECK(captured.provenance.baseCommitId == std::optional<std::string>("base-commit"));
    CHECK(captured.conversion.attributes == (std::map<std::string, std::string>{{"text", "auto"}}));
    CHECK(captured.conversion.configuration == (std::map<std::string, std::string>{{"core.autocrlf", "false"}}));
}
}

int main()
{
    test_support::Suite suite("conflict-capture");

    suite.run("a merge conflict with all three stages captures every source and the launch result", [] {
        const auto context = repository(git::RepositoryOperation::Merge);
        const auto captured = captureFile(conflict({1, 2, 3}), context, "session");
        CHECK(captured.has_value());
        checkShared(*captured, context);
        checkSources(*captured, context);
        CHECK(captured->provenance.operation == SnapshotProvenance::Operation::Merge);
        CHECK(captured->provenance.headName.empty() && captured->provenance.onto.empty());
    });

    suite.run("a rebase conflict with all three stages captures the rebase context", [] {
        const auto context = repository(git::RepositoryOperation::Rebase);
        const auto captured = captureFile(conflict({1, 2, 3}), context, "session");
        CHECK(captured.has_value());
        checkShared(*captured, context);
        checkSources(*captured, context);
        CHECK(captured->provenance.operation == SnapshotProvenance::Operation::Rebase);
        CHECK(captured->provenance.headName == "refs/heads/side");
        CHECK(captured->provenance.originalHead == "original-head" && captured->provenance.onto == "onto-commit");
    });

    suite.run("a conflict without the common earlier version still captures both sides", [] {
        const auto captured = captureFile(conflict({2, 3}), repository(git::RepositoryOperation::Merge), "session");
        CHECK(captured && !captured->earlier && captured->stage2 && captured->stage3);
    });

    suite.run("a conflict missing stage 2 or stage 3 is not captured", [] {
        const auto context = repository(git::RepositoryOperation::Merge);
        CHECK(!captureFile(conflict({1, 3}), context, "session"));
        CHECK(!captureFile(conflict({1, 2}), context, "session"));
        CHECK(!captureFile(conflict({3}), context, "session"));
    });

    suite.run("a stage whose path, object ID or mode disagrees with its source is not captured", [] {
        const auto context = repository(git::RepositoryOperation::Merge);
        const std::vector<std::pair<const char *, std::function<void(git::Conflict &)>>> changes{
            {"stage path", [](git::Conflict &file) { file.stages[1].path = "other.txt"; }},
            {"source path", [](git::Conflict &file) { file.sources[1].path = "other.txt"; }},
            {"object ID", [](git::Conflict &file) { file.sources[1].canonicalObjectId = "object-9"; }},
            {"stage mode", [](git::Conflict &file) { file.stages[2].mode = "100755"; }},
            {"source result mode", [](git::Conflict &file) { file.sources[2].mode = "100755"; }},
            {"worktree", [](git::Conflict &file) { file.sources[0].worktreeRoot = QStringLiteral("/elsewhere"); }},
            {"policy", [](git::Conflict &file) { file.sources[2].policy.attributes.insert("eol", "crlf"); }},
        };
        for (const auto &[name, change] : changes) {
            auto file = conflict({1, 2, 3});
            change(file);
            if (captureFile(file, context, "session")) throw std::runtime_error(std::string("captured despite ") + name);
        }
        CHECK(captureFile(conflict({1, 2, 3}), context, "session").has_value());
    });

    suite.run("a conflict outside the app, without a result, or outside a merge or rebase is not captured", [] {
        const auto merge = repository(git::RepositoryOperation::Merge);
        auto excluded = conflict({1, 2, 3});
        excluded.support = git::ConflictSupport::UnsupportedText;
        CHECK(!captureFile(excluded, merge, "session"));
        auto noResult = conflict({1, 2, 3});
        noResult.result.reset();
        CHECK(!captureFile(noResult, merge, "session"));
        auto noIdentity = conflict({1, 2, 3});
        noIdentity.resultIdentity.reset();
        CHECK(!captureFile(noIdentity, merge, "session"));
        auto unsupported = merge;
        unsupported.supported = false;
        CHECK(!captureFile(conflict({1, 2, 3}), unsupported, "session"));
        CHECK(!captureFile(conflict({1, 2, 3}), repository(git::RepositoryOperation::Unsupported), "session"));
        auto unconverted = conflict({1, 2, 3});
        unconverted.sources.pop_back();
        CHECK(!captureFile(unconverted, merge, "session"));
    });

    suite.run("UTF-16 and CRLF formats survive into the captured text snapshots", [] {
        storage::TextFormat utf16;
        utf16.encoding = storage::TextEncoding::Utf16LittleEndian;
        utf16.bom = true;
        utf16.finalNewline = true;
        const QByteArray utf16Bytes("\xff\xfe" "a\0\n\0", 6);
        storage::TextFormat crlf;
        crlf.newline = storage::Newline::CrLf;
        crlf.finalNewline = true;
        auto file = conflict({1, 2, 3});
        file.result = document("a\n", utf16Bytes, utf16);
        file.sources[1].document = document("left\n", "left\r\n", crlf);
        const auto captured = captureFile(file, repository(git::RepositoryOperation::Merge), "session");
        CHECK(captured.has_value());
        const auto &result = captured->launchResult.text;
        CHECK(result.utf8 == "a\n" && result.bytes == utf16Bytes.toStdString());
        CHECK(result.format.encoding == SnapshotFormat::Encoding::Utf16LittleEndian && result.format.bom);
        CHECK(result.format.newline == SnapshotFormat::Newline::Lf && result.format.finalNewline);
        const auto &left = captured->stage2->worktree;
        CHECK(left.utf8 == "left\n" && left.bytes == "left\r\n");
        CHECK(left.format.encoding == SnapshotFormat::Encoding::Utf8 && !left.format.bom);
        CHECK(left.format.newline == SnapshotFormat::Newline::CrLf && left.format.finalNewline);
        // The raw bytes stay the canonical object, separate from the worktree form.
        CHECK(captured->stage2->rawBytes == "stage 2\n");

        storage::TextFormat bigEndian;
        bigEndian.encoding = storage::TextEncoding::Utf16BigEndian;
        bigEndian.newline = storage::Newline::CrLf;
        const auto big = textSnapshot(document("b", QByteArray("\0b", 2), bigEndian));
        CHECK(big.format.encoding == SnapshotFormat::Encoding::Utf16BigEndian && !big.format.bom);
        CHECK(big.format.newline == SnapshotFormat::Newline::CrLf && !big.format.finalNewline);
        CHECK(big.utf8 == "b" && big.bytes == std::string("\0b", 2));
    });

    suite.run("failed reads and inspections are unreadable and other exclusions stay outside the app", [] {
        git::Conflict file;
        for (const auto support : {git::ConflictSupport::LoadFailure, git::ConflictSupport::InspectionFailure}) {
            file.support = support;
            CHECK(supportFor(file) == Support::Unreadable);
        }
        for (const auto support : {git::ConflictSupport::MissingSide, git::ConflictSupport::UnsupportedText,
                 git::ConflictSupport::UnverifiedSource, git::ConflictSupport::UnsupportedOperation}) {
            file.support = support;
            CHECK(supportFor(file) == Support::OutsideApp);
        }
    });

    return suite.report();
}
