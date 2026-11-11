// Repository discovery verifies merge or rebase sources and their paths before the app presents them.
#include "RepositoryDiscovery.h"
#include "GitParsing.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace branch_truce::git::detail {
namespace {
QByteArray line(QByteArray bytes)
{
    if (bytes.endsWith('\n')) bytes.chop(1);
    return bytes;
}
bool branchContext(const QByteArray &name)
{
    if (name == "detached HEAD") return true;
    if (!name.startsWith("refs/heads/") || name.contains("..") || name.contains("@{")) return false;
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte == 0x7f || QByteArray("~^:?*[\\").contains(c)) return false;
    }
    for (const auto &part : name.split('/')) {
        if (part.isEmpty() || part.startsWith('.') || part.endsWith('.') || part.endsWith(".lock")) return false;
    }
    return true;
}
} // namespace

RepositoryDiscovery::RepositoryDiscovery(Result &target, const QString &root) : result(target)
{
    result.repository.worktreeRoot = root;
}

QList<QByteArray> RepositoryDiscovery::arguments() const
{
    switch (step) {
    case Step::GitDirectory: return {"rev-parse", "--absolute-git-dir"};
    case Step::Index:
    case Step::FinalIndex: return {"ls-files", "--unmerged", "-z"};
    case Step::Head:
    case Step::FinalHead: return {"rev-parse", "--verify", "--end-of-options", "HEAD"};
    case Step::Commit: return {"cat-file", "commit", commitQueue.front()};
    case Step::MergeBase: return {"merge-base", "--all", head, other};
    case Step::ReplayAncestor: return {"merge-base", "--is-ancestor", other, originalHead};
    case Step::OntoAncestor: return {"merge-base", "--is-ancestor", onto, head};
    case Step::Tree: {
        QList<QByteArray> args{"ls-tree", "--full-tree", "-z", treeQueue.front(), "--"};
        for (qsizetype i = pathOffset; i < std::min(paths.size(), pathOffset + treeBatchSize); ++i)
            args.append(paths[i]);
        return args;
    }
    case Step::SourceTree: return {"ls-tree", "--full-tree", "-r", "-z", treeQueue.front()};
    case Step::Names: return {"for-each-ref", "--format=%(objectname)%00%(refname)", "refs/heads/"};
    case Step::Done: return {};
    }
    return {};
}

bool RepositoryDiscovery::fail(Problem reason)
{
    unsupported(std::move(reason));
    result.outcome = Outcome::InspectionFailure;
    step = Step::Done;
    return false;
}
void RepositoryDiscovery::unsupported(Problem reason)
{
    result.repository.operation = RepositoryOperation::Unsupported;
    result.repository.supported = false;
    result.repository.problem = reason;
    result.problem = std::move(reason);
}

bool RepositoryDiscovery::readMetadata(const QString &name, bool directory, QMap<QString, QByteArray> &snapshot,
                                       qsizetype &total)
{
    constexpr qsizetype metadataLimit = qsizetype{64} * 1024;
    const auto path = result.repository.gitDirectory + QLatin1Char('/') + name;
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) return true;
    if (info.isSymLink() || (directory ? !info.isDir() : !info.isFile())) {
        return fail(
            {directory ? ProblemCode::DiscoveryMetadataNotDirectory : ProblemCode::DiscoveryMetadataNotFile, {name}});
    }
    if (directory) {
        if (::access(QFile::encodeName(path).constData(), R_OK | X_OK) != 0)
            return fail({ProblemCode::DiscoveryMetadataDirectoryUnreadable, {name}});
        snapshot.insert(name, "directory");
        return true;
    }
    // Do not block on a FIFO or follow a final-component symlink if another
    // program replaces a marker after inspection. Recheck contents at end.
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat stat{};
    QFile file;
    if (fd < 0) return fail({ProblemCode::DiscoveryMetadataUnreadable, {name}});
    if (::fstat(fd, &stat) != 0 || !S_ISREG(stat.st_mode)) {
        ::close(fd);
        return fail({ProblemCode::DiscoveryMetadataChangedType, {name}});
    }
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        return fail({ProblemCode::DiscoveryMetadataCannotRead, {name}});
    }
    const auto bytes = file.read(metadataLimit - total + 1);
    total += bytes.size();
    if (file.error() != QFileDevice::NoError || total > metadataLimit)
        return fail({ProblemCode::DiscoveryMetadataOversized, {name}});
    snapshot.insert(name, bytes);
    return true;
}

bool RepositoryDiscovery::captureMetadata(QMap<QString, QByteArray> &snapshot)
{
    qsizetype total = 0;
    for (const auto &name : {"MERGE_HEAD", "REBASE_HEAD", "CHERRY_PICK_HEAD", "REVERT_HEAD"}) {
        if (!readMetadata(QLatin1String(name), false, snapshot, total)) return false;
    }
    for (const auto &name : {"rebase-merge", "rebase-apply", "sequencer"}) {
        if (!readMetadata(QLatin1String(name), true, snapshot, total)) return false;
    }
    if (snapshot.contains(QStringLiteral("rebase-merge"))) {
        for (const auto &name : {"head-name", "orig-head", "onto", "stopped-sha"}) {
            if (!readMetadata(QStringLiteral("rebase-merge/") + QLatin1String(name), false, snapshot, total))
                return false;
        }
    }
    if (snapshot.contains(QStringLiteral("rebase-apply"))) {
        for (const auto &name : {"head-name", "orig-head", "onto", "original-commit", "rebasing", "applying"}) {
            if (!readMetadata(QStringLiteral("rebase-apply/") + QLatin1String(name), false, snapshot, total))
                return false;
        }
    }
    return true;
}

bool RepositoryDiscovery::readId(const QString &name, QByteArray &value, bool required)
{
    if (!metadata.contains(name)) {
        return !required || fail({ProblemCode::DiscoveryMetadataMissing, {name}});
    }
    value = line(metadata.value(name));
    if (!objectId(value)) return fail({ProblemCode::DiscoveryInvalidCommitIdentity, {name}});
    value = value.toLower();
    return true;
}

bool RepositoryDiscovery::identifyRebase(bool rebaseMerge)
{
    const bool rebaseApply = !rebaseMerge;
    auto &context = result.repository;
    const auto directory = rebaseMerge ? QStringLiteral("rebase-merge/") : QStringLiteral("rebase-apply/");
    if (rebaseApply && metadata.contains(directory + QStringLiteral("applying"))) {
        if (metadata.contains(directory + QStringLiteral("rebasing"))
            || metadata.contains(QStringLiteral("REBASE_HEAD")))
            return fail(ProblemCode::DiscoveryApplyAndRebase);
        unsupported(ProblemCode::DiscoveryApplyMailbox);
        return true;
    }
    if (rebaseApply && !metadata.contains(directory + QStringLiteral("rebasing")))
        return fail(ProblemCode::DiscoveryApplyNotRebase);
    if (!readId(directory + QStringLiteral("orig-head"), originalHead)
        || !readId(directory + QStringLiteral("onto"), onto))
        return false;
    headName = line(metadata.value(directory + QStringLiteral("head-name")));
    if (metadata.contains(directory + QStringLiteral("head-name")) && !branchContext(headName))
        return fail(ProblemCode::DiscoveryInvalidHeadName);
    QByteArray recorded;
    if (!readId(QStringLiteral("REBASE_HEAD"), other, false)
        || !readId(directory + (rebaseMerge ? QStringLiteral("stopped-sha") : QStringLiteral("original-commit")),
                   recorded, false))
        return false;
    if (other.isEmpty()) other = recorded;
    if (other.isEmpty()) return fail(ProblemCode::DiscoveryNoReplayedCommit);
    if (!recorded.isEmpty() && other != recorded) return fail(ProblemCode::DiscoveryReplayDisagrees);
    context.operation = RepositoryOperation::Rebase;
    return true;
}

bool RepositoryDiscovery::identifyOperation()
{
    auto &context = result.repository;
    const bool merge = metadata.contains(QStringLiteral("MERGE_HEAD"));
    const bool rebaseMerge = metadata.contains(QStringLiteral("rebase-merge"));
    const bool rebaseApply = metadata.contains(QStringLiteral("rebase-apply"));
    const bool cherryPick = metadata.contains(QStringLiteral("CHERRY_PICK_HEAD"));
    const bool revert = metadata.contains(QStringLiteral("REVERT_HEAD"));
    const bool foreign = cherryPick || revert || metadata.contains(QStringLiteral("sequencer"));
    if ((merge && (rebaseMerge || rebaseApply)) || (rebaseMerge && rebaseApply)
        || (foreign && (merge || rebaseMerge || rebaseApply || metadata.contains(QStringLiteral("REBASE_HEAD"))))
        || (cherryPick && revert))
        return fail(ProblemCode::DiscoveryConflictingOperations);
    if (foreign) {
        unsupported(ProblemCode::DiscoverySequencerUnsupported);
        return true;
    }
    if (merge) {
        if (metadata.contains(QStringLiteral("REBASE_HEAD"))) return fail(ProblemCode::DiscoveryMergeAndRebaseHead);
        if (line(metadata.value(QStringLiteral("MERGE_HEAD"))).contains('\n'))
            return fail(ProblemCode::DiscoveryMultipleMergeHeads);
        if (!readId(QStringLiteral("MERGE_HEAD"), other)) return false;
        context.operation = RepositoryOperation::Merge;
        return true;
    }
    if (rebaseMerge || rebaseApply) return identifyRebase(rebaseMerge);
    if (metadata.contains(QStringLiteral("REBASE_HEAD"))) return fail(ProblemCode::DiscoveryRebaseHeadAlone);
    if (!result.indexEntries.isEmpty())
        unsupported(ProblemCode::DiscoveryUnverifiedIndex);
    else
        context.problem = ProblemCode::DiscoveryNoOperation;
    return true;
}

bool RepositoryDiscovery::readCommit(const QByteArray &bytes)
{
    const auto end = bytes.indexOf("\n\n");
    if (end < 0) return fail(ProblemCode::DiscoveryInvalidCommit);
    bool tree = false;
    Commit commit;
    for (const auto &header : bytes.first(end).split('\n')) {
        if (header.startsWith("tree ")) {
            if (tree || !objectId(header.mid(5))) return fail(ProblemCode::DiscoveryInvalidTree);
            tree = true;
        } else if (header.startsWith("parent ")) {
            if (!objectId(header.mid(7))) return fail(ProblemCode::DiscoveryInvalidParent);
            commit.parents.append(header.mid(7));
        }
    }
    if (!tree) return fail(ProblemCode::DiscoveryCommitWithoutTree);
    commits.insert(commitQueue.takeFirst(), commit);
    return true;
}

void RepositoryDiscovery::beginTrees()
{
    treeQueue = {head};
    if (!treeQueue.contains(other)) treeQueue.append(other);
    if (!base.isEmpty() && !treeQueue.contains(base)) treeQueue.append(base);
    pathOffset = 0;
    step = paths.isEmpty() ? Step::FinalIndex : Step::Tree;
}

bool RepositoryDiscovery::readTree(const QByteArray &bytes)
{
    const bool sourceTree = step == Step::SourceTree;
    auto &tree = trees[treeQueue.front()];
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const auto end = bytes.indexOf('\0', offset);
        if (end < 0) return fail(ProblemCode::DiscoveryInvalidSourceTree);
        const auto record = bytes.mid(offset, end - offset);
        const auto tab = record.indexOf('\t');
        const auto fields = record.first(tab < 0 ? 0 : tab).split(' ');
        const auto path = tab < 0 ? QByteArray{} : record.mid(tab + 1);
        const auto batchEnd = std::min(paths.size(), pathOffset + treeBatchSize);
        bool requested = sourceTree;
        for (qsizetype i = pathOffset; i < batchEnd; ++i) requested = requested || paths[i] == path;
        if (fields.size() != 3 || fields[0].size() != 6 || !objectId(fields[2])
            || (fields[1] != "blob" && fields[1] != "commit" && fields[1] != "tree") || !relativeFile(path)
            || !requested || (!sourceTree && tree.contains(path)))
            return fail(ProblemCode::DiscoveryTreeMismatch);
        if (sourceTree) {
            auto &objects = sourceObjects[treeQueue.front()];
            const auto key = fields[0] + ' ' + fields[2];
            if (objects.contains(key)) objects[key] = true;
        } else
            tree.insert(path, {fields[0], fields[2]});
        offset = end + 1;
    }
    pathOffset += treeBatchSize;
    if (sourceTree || pathOffset >= paths.size()) {
        treeQueue.removeFirst();
        pathOffset = 0;
    }
    if (treeQueue.isEmpty()) {
        if (!sourceTree) {
            queueSourceTrees();
            if (!treeQueue.isEmpty()) {
                step = Step::SourceTree;
                return true;
            }
        }
        verifyStages();
        step = Step::FinalIndex;
    }
    return true;
}

void RepositoryDiscovery::queueSourceTrees()
{
    // A text conflict can use its destination path for all index stages
    // while an earlier contribution still lives at its old path. Verify
    // the exact blob and mode in that commit without inferring a rename.
    const QList<QByteArray> sources{base, head, other};
    for (const auto &entry : result.indexEntries) {
        const auto &source = sources[static_cast<qsizetype>(entry.stage) - 1];
        if (!source.isEmpty() && !trees.value(source).contains(entry.path)
            && (entry.mode == "100644" || entry.mode == "100755")) {
            sourceObjects[source].insert(entry.mode + ' ' + entry.objectId, false);
            if (!treeQueue.contains(source)) treeQueue.append(source);
        }
    }
}

void RepositoryDiscovery::verifyStages()
{
    const QList<QByteArray> sources{base, head, other};
    QSet<QByteArray> present;
    for (const auto &entry : result.indexEntries) {
        const auto &source = sources[static_cast<qsizetype>(entry.stage) - 1];
        const auto expected = trees.value(source).value(entry.path);
        const auto key = QByteArray::number(entry.stage) + '\0' + entry.path;
        const bool atPath = expected.mode == entry.mode && expected.objectId == entry.objectId;
        const bool elsewhere = !trees.value(source).contains(entry.path)
            && sourceObjects.value(source).value(entry.mode + ' ' + entry.objectId);
        if (present.contains(key) || source.isEmpty() || (!atPath && !elsewhere)) {
            result.repository.unsupportedPaths.insert(entry.path,
                                                      Problem{ProblemCode::DiscoveryStageProvenance,
                                                              {QString::number(entry.stage), displayPath(entry.path)}});
        }
        present.insert(key);
    }
    for (const auto &path : paths) {
        for (qsizetype i = 0; i < sources.size(); ++i) {
            if (trees.value(sources[i]).contains(path) && !present.contains(QByteArray::number(i + 1) + '\0' + path)) {
                result.repository.unsupportedPaths.insert(
                    path, Problem{ProblemCode::DiscoveryMissingStage, {QString::number(i + 1), displayPath(path)}});
            }
        }
    }
}

bool RepositoryDiscovery::finish()
{
    QMap<QString, QByteArray> current;
    if (!captureMetadata(current)) return false;
    if (current != metadata) return fail(ProblemCode::DiscoveryMetadataChanged);
    auto &context = result.repository;
    if (context.operation == RepositoryOperation::Merge || context.operation == RepositoryOperation::Rebase) {
        const bool rebase = context.operation == RepositoryOperation::Rebase;
        context.stage2.commitId = head;
        context.stage2.label = rebase ? QStringLiteral("Current rebased state") : QStringLiteral("Current checkout");
        context.stage3.commitId = other;
        context.stage3.label = rebase ? QStringLiteral("Commit being replayed") : QStringLiteral("Merged commit");
        context.baseCommitId = base;
        context.headName = headName;
        context.originalHead = originalHead;
        context.onto = onto;
        context.supported = true;
    }
    result.outcome = Outcome::Success;
    result.exitCode = 0;
    // Retain the original discovery output for callers using it as a display.
    // The structured paths are the lossless, unambiguous API.
    result.standardOutput =
        QFile::encodeName(context.worktreeRoot) + '\n' + QFile::encodeName(context.gitDirectory) + '\n';
    step = Step::Done;
    return false;
}

bool RepositoryDiscovery::readDirectory(const QByteArray &output)
{
    const auto bytes = line(output);
    const auto path = QFile::decodeName(bytes);
    const auto canonical = QFileInfo(path).canonicalFilePath();
    if (!output.endsWith('\n') || bytes.isEmpty() || bytes.contains('\0') || QFile::encodeName(path) != bytes
        || !QDir::isAbsolutePath(path) || canonical.isEmpty() || !QFileInfo(canonical).isDir())
        return fail(ProblemCode::DiscoveryGitDirectory);
    result.repository.gitDirectory = canonical;
    step = Step::Index;
    return true;
}

bool RepositoryDiscovery::readIndex(const QByteArray &output)
{
    const auto entries = parseIndex(output);
    if (!entries) return fail(ProblemCode::DiscoveryUnmergedRecords);
    for (const auto &entry : *entries) {
        if (entry.stage == 0) return fail(ProblemCode::DiscoveryStageZero);
        if (!paths.contains(entry.path)) paths.append(entry.path);
    }
    result.indexEntries = *entries;
    index = output;
    if (!captureMetadata(metadata) || !identifyOperation()) return false;
    step = result.repository.operation == RepositoryOperation::Merge
            || result.repository.operation == RepositoryOperation::Rebase
        ? Step::Head
        : Step::FinalIndex;
    return true;
}

bool RepositoryDiscovery::readMergeBase(const QByteArray &output)
{
    const auto bases = output.isEmpty() ? QList<QByteArray>{} : line(output).split('\n');
    if (bases.size() > 1) return fail(ProblemCode::DiscoveryMultipleBases);
    if (!bases.isEmpty()) {
        base = bases.front();
        if (!objectId(base)) return fail(ProblemCode::DiscoveryInvalidBase);
    }
    beginTrees();
    return true;
}

bool RepositoryDiscovery::readNames(const QByteArray &output)
{
    QHash<QByteArray, QList<QByteArray>> names;
    for (const auto &record : output.split('\n')) {
        if (record.isEmpty()) continue;
        const auto fields = record.split('\0');
        if (fields.size() != 2 || !objectId(fields[0]) || !branchContext(fields[1]))
            return fail(ProblemCode::DiscoveryInvalidBranch);
        names[fields[0]].append(fields[1].mid(11));
    }
    const auto uniqueName = [&names](const QByteArray &id) {
        const auto found = names.value(id);
        return found.size() == 1 ? displayPath(found.front()) : QString{};
    };
    result.repository.stage2.name = uniqueName(head);
    result.repository.stage3.name = uniqueName(other);
    result.repository.ontoName = uniqueName(onto);
    return finish();
}

bool RepositoryDiscovery::readHead(const QByteArray &output)
{
    head = line(output);
    if (!objectId(head)) return fail(ProblemCode::DiscoveryInvalidHead);
    for (const auto &id : {head, other, originalHead, onto}) {
        if (!id.isEmpty() && !commitQueue.contains(id)) commitQueue.append(id);
    }
    step = Step::Commit;
    return true;
}

bool RepositoryDiscovery::consumeCommit(const QByteArray &output)
{
    if (!readCommit(output)) return false;
    if (commitQueue.isEmpty()) {
        if (result.repository.operation == RepositoryOperation::Rebase) {
            const auto parents = commits.value(other).parents;
            if (parents.size() > 1) return fail(ProblemCode::DiscoveryMergeCommitReplay);
            if (!parents.isEmpty()) base = parents.front();
            step = Step::ReplayAncestor;
        } else
            step = Step::MergeBase;
    }
    return true;
}

bool RepositoryDiscovery::consume(const QByteArray &output, int code)
{
    if (code != 0 && (step != Step::MergeBase || code != 1 || !output.isEmpty())) {
        const auto reason = step == Step::ReplayAncestor ? ProblemCode::DiscoveryReplayOutsideHistory
            : step == Step::OntoAncestor                 ? ProblemCode::DiscoveryHeadNotOnOnto
                                                         : ProblemCode::DiscoveryUnverifiedObject;
        return fail(reason);
    }
    switch (step) {
    case Step::GitDirectory: return readDirectory(output);
    case Step::Index: return readIndex(output);
    case Step::Head: return readHead(output);
    case Step::Commit: return consumeCommit(output);
    case Step::MergeBase: return readMergeBase(output);
    case Step::ReplayAncestor: step = Step::OntoAncestor; break;
    case Step::OntoAncestor: beginTrees(); break;
    case Step::Tree:
    case Step::SourceTree: return readTree(output);
    case Step::FinalIndex:
        if (output != index) return fail(ProblemCode::DiscoveryIndexChanged);
        if (head.isEmpty()) return finish();
        step = Step::FinalHead;
        break;
    case Step::FinalHead:
        if (line(output) != head) return fail(ProblemCode::DiscoveryHeadChanged);
        step = Step::Names;
        break;
    case Step::Names: return readNames(output);
    case Step::Done: return false;
    }
    return true;
}
} // namespace branch_truce::git::detail
