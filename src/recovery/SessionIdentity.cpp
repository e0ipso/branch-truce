// Session identity: the checks that a repository, its index and its policy still match a session's captures.
#include "recovery/SessionIdentity.h"
#include "storage/TextFormat.h"

namespace branch_truce {
bool sameOperation(const SnapshotProvenance &p, const git::RepositoryContext &repository)
{
    const bool operation =
        (p.operation == SnapshotProvenance::Operation::Merge && repository.operation == git::RepositoryOperation::Merge)
        || (p.operation == SnapshotProvenance::Operation::Rebase
            && repository.operation == git::RepositoryOperation::Rebase);
    return repository.supported && operation && p.worktreeRoot == repository.worktreeRoot.toStdString()
        && p.gitDirectory == repository.gitDirectory.toStdString()
        && p.stage2CommitId == repository.stage2.commitId.toStdString()
        && p.stage3CommitId == repository.stage3.commitId.toStdString()
        && p.baseCommitId.value_or("") == repository.baseCommitId.toStdString()
        && p.headName == repository.headName.toStdString() && p.originalHead == repository.originalHead.toStdString()
        && p.onto == repository.onto.toStdString();
}

QList<git::IndexEntry> capturedStages(const FileSnapshots &snapshots)
{
    QList<git::IndexEntry> stages;
    const auto path = QByteArray::fromStdString(snapshots.path);
    const auto add = [&](unsigned int stage, const std::optional<SourceSnapshot> &source) {
        if (!source) return;
        stages.push_back(
            {path, QByteArray::fromStdString(source->mode), QByteArray::fromStdString(source->objectId), stage});
    };
    add(1, snapshots.earlier);
    add(2, snapshots.stage2);
    add(3, snapshots.stage3);
    return stages;
}

bool matchesCapturedConflict(const QList<git::IndexEntry> &entries, const FileSnapshots &snapshots)
{
    if (!snapshots.stage2 || !snapshots.stage3 || entries.size() != (snapshots.earlier ? 3 : 2)) return false;
    bool seen[4]{};
    for (const auto &entry : entries) {
        if (entry.stage < 1 || entry.stage > 3 || seen[entry.stage]) return false;
        const auto &source = entry.stage == 1 ? snapshots.earlier
            : entry.stage == 2                ? snapshots.stage2
                                              : snapshots.stage3;
        if (!source || entry.path.toStdString() != snapshots.path || entry.objectId.toStdString() != source->objectId
            || entry.mode.toStdString() != source->mode)
            return false;
        seen[entry.stage] = true;
    }
    return true;
}

bool samePolicy(const ConversionSnapshot &captured, const git::ConversionPolicy &live)
{
    const auto same = [](const std::map<std::string, std::string> &expected,
                         const QHash<QByteArray, QByteArray> &observed) {
        if (expected.size() != static_cast<std::size_t>(observed.size())) return false;
        for (auto it = observed.cbegin(); it != observed.cend(); ++it) {
            const auto found = expected.find(it.key().toStdString());
            if (found == expected.end() || found->second != it.value().toStdString()) return false;
        }
        return true;
    };
    return same(captured.attributes, live.attributes) && same(captured.configuration, live.configuration);
}

std::optional<QByteArray> serializedRevision(const FileSession &file, const Revision &revision)
{
    const auto *snapshots = file.snapshots();
    if (!snapshots) return std::nullopt;
    const auto result =
        storage::serializeText(QByteArray::fromStdString(revision.draft.text), snapshots->launchResult.text.format);
    return result.document ? std::optional<QByteArray>(result.document->originalBytes) : std::nullopt;
}

bool modeMatches(std::uint32_t fileMode, std::string_view gitMode)
{
    return ((fileMode & 0111U) != 0) == (gitMode == "100755");
}
}
