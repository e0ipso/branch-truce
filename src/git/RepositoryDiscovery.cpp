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
        for (qsizetype i = pathOffset; i < std::min(paths.size(), pathOffset + treeBatchSize); ++i) args.append(paths[i]);
        return args;
    }
    case Step::SourceTree: return {"ls-tree", "--full-tree", "-r", "-z", treeQueue.front()};
    case Step::Done: return {};
    }
    return {};
}

bool RepositoryDiscovery::fail(const QString &reason)
{
    unsupported(reason);
    result.outcome = Outcome::InspectionFailure;
    step = Step::Done;
    return false;
}
void RepositoryDiscovery::unsupported(const QString &reason)
{
    result.repository.operation = RepositoryOperation::Unsupported;
    result.repository.supported = false;
    result.repository.diagnostic = reason;
    result.diagnostic = reason;
}

bool RepositoryDiscovery::captureMetadata(QMap<QString, QByteArray> &snapshot)
{
    constexpr qsizetype metadataLimit = 64 * 1024;
    qsizetype total = 0;
    const auto read = [&](const QString &name, bool directory) {
        const auto path = result.repository.gitDirectory + QLatin1Char('/') + name;
        const QFileInfo info(path);
        if (!info.exists() && !info.isSymLink()) return true;
        if (info.isSymLink() || (directory ? !info.isDir() : !info.isFile())) {
            return fail(QStringLiteral("Operation metadata is not a regular %1: %2")
                            .arg(directory ? QStringLiteral("directory") : QStringLiteral("file"), name));
        }
        if (directory) {
            if (::access(QFile::encodeName(path).constData(), R_OK | X_OK) != 0)
                return fail(QStringLiteral("Operation metadata directory is unreadable: ") + name);
            snapshot.insert(name, "directory");
            return true;
        }
        // Do not block on a FIFO or follow a final-component symlink if another
        // program replaces a marker after inspection. Recheck contents at end.
        const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        struct stat stat {};
        QFile file;
        if (fd < 0) return fail(QStringLiteral("Operation metadata is unreadable: ") + name);
        if (::fstat(fd, &stat) != 0 || !S_ISREG(stat.st_mode)) {
            ::close(fd);
            return fail(QStringLiteral("Operation metadata changed type: ") + name);
        }
        if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
            ::close(fd);
            return fail(QStringLiteral("Operation metadata cannot be read: ") + name);
        }
        const auto bytes = file.read(metadataLimit - total + 1);
        total += bytes.size();
        if (file.error() != QFileDevice::NoError || total > metadataLimit)
            return fail(QStringLiteral("Operation metadata is unreadable or oversized: ") + name);
        snapshot.insert(name, bytes);
        return true;
    };
    for (const auto &name : {"MERGE_HEAD", "REBASE_HEAD", "CHERRY_PICK_HEAD", "REVERT_HEAD"}) {
        if (!read(QLatin1String(name), false)) return false;
    }
    for (const auto &name : {"rebase-merge", "rebase-apply", "sequencer"}) {
        if (!read(QLatin1String(name), true)) return false;
    }
    if (snapshot.contains(QStringLiteral("rebase-merge"))) {
        for (const auto &name : {"head-name", "orig-head", "onto", "stopped-sha"}) {
            if (!read(QStringLiteral("rebase-merge/") + QLatin1String(name), false)) return false;
        }
    }
    if (snapshot.contains(QStringLiteral("rebase-apply"))) {
        for (const auto &name : {"head-name", "orig-head", "onto", "original-commit", "rebasing", "applying"}) {
            if (!read(QStringLiteral("rebase-apply/") + QLatin1String(name), false)) return false;
        }
    }
    return true;
}

bool RepositoryDiscovery::readId(const QString &name, QByteArray &value, bool required)
{
    if (!metadata.contains(name)) {
        return !required || fail(QStringLiteral("Required operation metadata is missing: ") + name);
    }
    value = line(metadata.value(name));
    if (!objectId(value)) return fail(QStringLiteral("Invalid commit identity in operation metadata: ") + name);
    value = value.toLower();
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
        return fail(QStringLiteral("Conflicting merge, rebase or sequencer operation metadata"));
    if (foreign) {
        unsupported(QStringLiteral("Outside app: cherry-pick, revert and sequencer operations are unsupported"));
        return true;
    }
    if (merge) {
        if (metadata.contains(QStringLiteral("REBASE_HEAD")))
            return fail(QStringLiteral("MERGE_HEAD conflicts with REBASE_HEAD"));
        if (line(metadata.value(QStringLiteral("MERGE_HEAD"))).contains('\n'))
            return fail(QStringLiteral("Outside app: merges with multiple MERGE_HEAD commits are unsupported"));
        if (!readId(QStringLiteral("MERGE_HEAD"), other)) return false;
        context.operation = RepositoryOperation::Merge;
        return true;
    }
    if (rebaseMerge || rebaseApply) {
        const auto directory = rebaseMerge ? QStringLiteral("rebase-merge/") : QStringLiteral("rebase-apply/");
        if (rebaseApply && metadata.contains(directory + QStringLiteral("applying"))) {
            if (metadata.contains(directory + QStringLiteral("rebasing")) || metadata.contains(QStringLiteral("REBASE_HEAD")))
                return fail(QStringLiteral("Conflicting apply-mailbox and rebase metadata"));
            unsupported(QStringLiteral("Outside app: apply-mailbox (git am) is not a supported rebase"));
            return true;
        }
        if (rebaseApply && !metadata.contains(directory + QStringLiteral("rebasing")))
            return fail(QStringLiteral("rebase-apply metadata does not identify a rebase"));
        if (!readId(directory + QStringLiteral("orig-head"), originalHead)
            || !readId(directory + QStringLiteral("onto"), onto)) return false;
        headName = line(metadata.value(directory + QStringLiteral("head-name")));
        if (metadata.contains(directory + QStringLiteral("head-name")) && !branchContext(headName))
            return fail(QStringLiteral("Invalid rebase head-name context"));
        QByteArray recorded;
        if (!readId(QStringLiteral("REBASE_HEAD"), other, false)
            || !readId(directory + (rebaseMerge ? QStringLiteral("stopped-sha") : QStringLiteral("original-commit")), recorded, false))
            return false;
        if (other.isEmpty()) other = recorded;
        if (other.isEmpty()) return fail(QStringLiteral("Rebase has no verified commit being replayed"));
        if (!recorded.isEmpty() && other != recorded)
            return fail(QStringLiteral("Rebase replay metadata disagrees with REBASE_HEAD"));
        context.operation = RepositoryOperation::Rebase;
        return true;
    }
    if (metadata.contains(QStringLiteral("REBASE_HEAD")))
        return fail(QStringLiteral("REBASE_HEAD exists without supported rebase metadata"));
    if (!result.indexEntries.isEmpty())
        unsupported(QStringLiteral("Outside app: unresolved index entries have no verified merge or rebase identity"));
    else context.diagnostic = QStringLiteral("No merge or rebase is in progress");
    return true;
}

bool RepositoryDiscovery::readCommit(const QByteArray &bytes)
{
    const auto end = bytes.indexOf("\n\n");
    if (end < 0) return fail(QStringLiteral("Invalid commit object in operation identity"));
    bool tree = false;
    Commit commit;
    for (const auto &header : bytes.first(end).split('\n')) {
        if (header.startsWith("tree ")) {
            if (tree || !objectId(header.mid(5))) return fail(QStringLiteral("Invalid commit tree identity"));
            tree = true;
        } else if (header.startsWith("parent ")) {
            if (!objectId(header.mid(7))) return fail(QStringLiteral("Invalid commit parent identity"));
            commit.parents.append(header.mid(7));
        }
    }
    if (!tree) return fail(QStringLiteral("Operation identity has no commit tree"));
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
        if (end < 0) return fail(QStringLiteral("Invalid NUL-delimited source tree"));
        const auto record = bytes.mid(offset, end - offset);
        const auto tab = record.indexOf('\t');
        const auto fields = record.first(tab < 0 ? 0 : tab).split(' ');
        const auto path = tab < 0 ? QByteArray{} : record.mid(tab + 1);
        const auto batchEnd = std::min(paths.size(), pathOffset + treeBatchSize);
        bool requested = sourceTree;
        for (qsizetype i = pathOffset; i < batchEnd; ++i) requested = requested || paths[i] == path;
        if (fields.size() != 3 || fields[0].size() != 6 || !objectId(fields[2])
            || (fields[1] != "blob" && fields[1] != "commit" && fields[1] != "tree")
            || !relativeFile(path) || !requested || (!sourceTree && tree.contains(path)))
            return fail(QStringLiteral("Source tree does not match the captured conflict paths"));
        if (sourceTree) {
            auto &objects = sourceObjects[treeQueue.front()];
            const auto key = fields[0] + ' ' + fields[2];
            if (objects.contains(key)) objects[key] = true;
        } else tree.insert(path, {fields[0], fields[2]});
        offset = end + 1;
    }
    pathOffset += treeBatchSize;
    if (sourceTree || pathOffset >= paths.size()) {
        treeQueue.removeFirst();
        pathOffset = 0;
    }
    if (treeQueue.isEmpty()) {
        if (!sourceTree) {
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
                QStringLiteral("Outside app: index stage %1 provenance does not match its verified commit at %2")
                    .arg(entry.stage).arg(displayPath(entry.path)));
        }
        present.insert(key);
    }
    for (const auto &path : paths) {
        for (qsizetype i = 0; i < sources.size(); ++i) {
            if (trees.value(sources[i]).contains(path) && !present.contains(QByteArray::number(i + 1) + '\0' + path)) {
                result.repository.unsupportedPaths.insert(path,
                    QStringLiteral("Outside app: missing index stage %1 has ambiguous source topology at %2")
                        .arg(i + 1).arg(displayPath(path)));
            }
        }
    }
}

bool RepositoryDiscovery::finish()
{
    QMap<QString, QByteArray> current;
    if (!captureMetadata(current)) return false;
    if (current != metadata) return fail(QStringLiteral("Operation metadata changed during discovery; inspect again"));
    auto &context = result.repository;
    if (context.operation == RepositoryOperation::Merge || context.operation == RepositoryOperation::Rebase) {
        const bool rebase = context.operation == RepositoryOperation::Rebase;
        context.stage2 = {head, (rebase ? QStringLiteral("Current rebased state: ") : QStringLiteral("Current checkout: "))
                                   + QString::fromLatin1(head)};
        context.stage3 = {other, (rebase ? QStringLiteral("Commit being replayed: ") : QStringLiteral("Merged commit: "))
                                    + QString::fromLatin1(other)};
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
    result.standardOutput = QFile::encodeName(context.worktreeRoot) + '\n'
        + QFile::encodeName(context.gitDirectory) + '\n';
    step = Step::Done;
    return false;
}

bool RepositoryDiscovery::consume(const QByteArray &output, int code)
{
    if (code != 0 && !(step == Step::MergeBase && code == 1 && output.isEmpty())) {
        const auto reason = step == Step::ReplayAncestor ? QStringLiteral("Replayed commit is not part of the original rebase history")
            : step == Step::OntoAncestor ? QStringLiteral("Current HEAD is not based on the recorded rebase onto commit")
            : QStringLiteral("Git could not verify operation metadata or a required local source object");
        return fail(reason);
    }
    switch (step) {
    case Step::GitDirectory: {
        const auto bytes = line(output);
        const auto path = QFile::decodeName(bytes);
        const auto canonical = QFileInfo(path).canonicalFilePath();
        if (!output.endsWith('\n') || bytes.isEmpty() || bytes.contains('\0') || QFile::encodeName(path) != bytes
            || !QDir::isAbsolutePath(path) || canonical.isEmpty() || !QFileInfo(canonical).isDir())
            return fail(QStringLiteral("Git did not provide a usable per-worktree Git directory"));
        result.repository.gitDirectory = canonical;
        step = Step::Index;
        break;
    }
    case Step::Index: {
        const auto entries = parseIndex(output);
        if (!entries) return fail(QStringLiteral("Invalid unmerged index records during discovery"));
        for (const auto &entry : *entries) {
            if (entry.stage == 0) return fail(QStringLiteral("Stage zero appeared in the unmerged index"));
            if (!paths.contains(entry.path)) paths.append(entry.path);
        }
        result.indexEntries = *entries;
        index = output;
        if (!captureMetadata(metadata) || !identifyOperation()) return false;
        step = result.repository.operation == RepositoryOperation::Merge || result.repository.operation == RepositoryOperation::Rebase
            ? Step::Head : Step::FinalIndex;
        break;
    }
    case Step::Head:
        head = line(output);
        if (!objectId(head)) return fail(QStringLiteral("HEAD has no valid commit identity"));
        for (const auto &id : {head, other, originalHead, onto}) {
            if (!id.isEmpty() && !commitQueue.contains(id)) commitQueue.append(id);
        }
        step = Step::Commit;
        break;
    case Step::Commit:
        if (!readCommit(output)) return false;
        if (commitQueue.isEmpty()) {
            if (result.repository.operation == RepositoryOperation::Rebase) {
                const auto parents = commits.value(other).parents;
                if (parents.size() > 1) return fail(QStringLiteral("Outside app: replaying merge commits has unsupported stage provenance"));
                if (!parents.isEmpty()) base = parents.front();
                step = Step::ReplayAncestor;
            } else step = Step::MergeBase;
        }
        break;
    case Step::MergeBase: {
        const auto bases = output.isEmpty() ? QList<QByteArray>{} : line(output).split('\n');
        if (bases.size() > 1) return fail(QStringLiteral("Outside app: multiple merge bases have ambiguous earlier-stage provenance"));
        if (!bases.isEmpty()) {
            base = bases.front();
            if (!objectId(base)) return fail(QStringLiteral("Invalid common earlier commit identity"));
        }
        beginTrees();
        break;
    }
    case Step::ReplayAncestor: step = Step::OntoAncestor; break;
    case Step::OntoAncestor: beginTrees(); break;
    case Step::Tree:
    case Step::SourceTree: return readTree(output);
    case Step::FinalIndex:
        if (output != index) return fail(QStringLiteral("Unmerged index changed during discovery; inspect again"));
        if (head.isEmpty()) return finish();
        step = Step::FinalHead;
        break;
    case Step::FinalHead:
        if (line(output) != head) return fail(QStringLiteral("HEAD changed during discovery; inspect again"));
        return finish();
    case Step::Done: return false;
    }
    return true;
}
} // namespace branch_truce::git::detail
