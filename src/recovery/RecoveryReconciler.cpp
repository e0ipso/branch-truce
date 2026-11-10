// Recovery reconciliation checks recovered files against the live repository before editing resumes.
#include "recovery/RecoveryReconciler.h"
#include "recovery/SessionIdentity.h"
#include <QEventLoop>
#include <QUuid>
#include <algorithm>

namespace branch_truce {
RepositoryProbe::RepositoryProbe(QString gitExecutable) : executable_(std::move(gitExecutable)) {}

git::Result RepositoryProbe::inspect(const git::Request &request) const
{
    git::GitAdapter adapter(executable_);
    QEventLoop loop;
    git::Result result;
    QObject::connect(&adapter, &git::GitAdapter::finished, &loop, [&](const git::Result &value) {
        result = value;
        loop.quit();
    });
    if (adapter.start(request)) loop.exec();
    return result;
}

storage::DestinationCapture RepositoryProbe::capture(const QString &worktreeRoot, const QByteArray &path) const
{
    return storage::captureDestination(worktreeRoot, path);
}

namespace {
git::Request indexRequest(const git::RepositoryContext &repository, const QByteArray &path)
{
    git::Request request;
    request.operation = git::Operation::InspectIndex;
    request.directory = repository.worktreeRoot;
    request.path = path;
    return request;
}

std::optional<QByteArray> stagedIdentity(const FileSession &file, const Revision &revision,
                                         const QList<git::IndexEntry> &entries,
                                         const git::RepositoryContext &repository, const RepositoryProbe &probe)
{
    const auto *snapshots = file.snapshots();
    const auto path = QByteArray::fromStdString(file.path());
    if (!snapshots || entries.size() != 1 || entries.front().stage != 0 || entries.front().path != path
        || entries.front().mode.toStdString() != snapshots->resultMode)
        return {};
    const auto bytes = serializedRevision(file, revision);
    if (!bytes) return {};
    git::Request request;
    request.operation = git::Operation::CanonicalizeText;
    request.directory = repository.worktreeRoot;
    request.path = path;
    request.input = *bytes;
    const auto canonical = probe.inspect(request);
    if (canonical.outcome != git::Outcome::Success || !samePolicy(snapshots->conversion, canonical.conversionPolicy)
        || canonical.canonicalObjectId != entries.front().objectId)
        return {};
    return canonical.canonicalObjectId;
}

void completeMutations(std::vector<RecoveryMutation> &mutations, const std::string &fileId, MutationKind kind,
                       RevisionId revision)
{
    for (auto &mutation : mutations) {
        if (mutation.kind == kind && mutation.target.id() == fileId && mutation.target.current().id == revision)
            mutation.completed = true;
    }
}

// A mutation whose observation proves nothing reached the repository.
bool untouched(const RecoveryMutation &mutation)
{
    return mutation.observation == MutationObservation::NotAttempted
        || mutation.observation == MutationObservation::ObservedUnchanged;
}

// Git mergetool replaces MERGED with a copy of the same file: a new inode and
// new timestamps, nothing else.
bool sameFileCopy(const SnapshotFileIdentity &launch, const SnapshotFileIdentity &live)
{
    return live.device == launch.device && live.mode == launch.mode && live.size == launch.size;
}

QString kindName(MutationKind kind) { return nameOf(mutationKindNames, kind); }

// A crash can lose the completion callback after Git installed the exact
// reviewed result. Verify content, policy, mode and the written revision,
// then persist that observation before exposing a resolved state.
QString recoverStaging(FileSession &file, std::vector<RecoveryMutation> &mutations, const QByteArray &diskBytes,
                       std::uint32_t diskMode, const QList<git::IndexEntry> &index,
                       const git::RepositoryContext &repository, const RepositoryProbe &probe,
                       storage::RecoveryLease &lease)
{
    for (const auto &mutation : mutations) {
        if (mutation.completed || mutation.kind != MutationKind::Staging || mutation.target.id() != file.id()
            || untouched(mutation) || file.publication().written() != mutation.target.current().id)
            continue;
        const auto *captured = file.snapshots();
        const auto *written = file.revision(*file.publication().written());
        if (!written) continue;
        const auto bytes = serializedRevision(file, *written);
        const auto objectId = stagedIdentity(file, *written, index, repository, probe);
        if (!bytes || !objectId || *bytes != diskBytes || !modeMatches(diskMode, captured->resultMode)) continue;
        auto candidate = file;
        if (candidate.recordStage(written->id, Outcome::Succeeded, objectId->toStdString(), captured->resultMode,
                                  "Recovered staging completion verified against disk and index")
            != Transition::Applied)
            continue;
        const auto committed = lease.commit(recoveryTransaction(candidate, RecoveryAction::StageOutcome,
                                                                QUuid::createUuid().toString(QUuid::WithoutBraces),
                                                                file.current().draft.text));
        if (!committed.succeeded)
            return QStringLiteral("Staging evidence matched, but recovery could not retain its completion: %1")
                .arg(committed.error);
        const auto revision = written->id;
        file = std::move(candidate);
        completeMutations(mutations, file.id(), MutationKind::Staging, revision);
    }
    return {};
}
}

QString copiedLaunchFailure()
{
    return QStringLiteral(
        "Recovered draft retained, but the repository file was replaced by a copy with the same bytes. "
        "Work remains outstanding unless git mergetool is waiting on this path with those bytes.");
}

Reconciliation reconcile(FileSession &file, std::vector<RecoveryMutation> &mutations,
                         const std::vector<FileSession> &observed, const git::RepositoryContext &repository,
                         const RepositoryProbe &probe, storage::RecoveryLease &lease)
{
    if (!file.snapshots())
        return {file.support() == Support::Supported
                    ? QStringLiteral("Recovery has no admitted source and result captures; file remains outstanding.")
                    : QString{},
                {}};
    const auto path = QByteArray::fromStdString(file.path());
    if (file.id() != path.toHex().toStdString())
        return {QStringLiteral("Recovered file identity does not match its exact path."), {}};
    const auto disk = probe.capture(repository.worktreeRoot, path);
    if (!disk.destination)
        return {QStringLiteral("Recovery could not validate the current repository file: %1").arg(disk.diagnostic), {}};
    const auto &destination = *disk.destination;
    const auto index = probe.inspect(indexRequest(repository, path));
    if (index.outcome != git::Outcome::Success)
        return {QStringLiteral("Recovery could not inspect the current Git index."), {}};
    if (auto failure = recoverStaging(file, mutations, destination.bytes(), destination.identity().mode,
                                      index.indexEntries, repository, probe, lease);
        !failure.isEmpty())
        return {failure, {}};
    const auto *captured = file.snapshots();

    const auto live =
        std::find_if(observed.begin(), observed.end(), [&](const auto &entry) { return entry.id() == file.id(); });
    const auto *liveCapture = live != observed.end() ? live->snapshots() : nullptr;
    const bool sourcesMatch = liveCapture && liveCapture->conversion == captured->conversion
        && liveCapture->earlier == captured->earlier && liveCapture->stage2 == captured->stage2
        && liveCapture->stage3 == captured->stage3;
    const bool launchMatches = sourcesMatch && liveCapture->launchResult == captured->launchResult;
    bool policyMatches = liveCapture && liveCapture->conversion == captured->conversion;
    const auto &launch = captured->launchResult;
    const bool launchBytes = destination.bytes() == QByteArray::fromStdString(launch.text.bytes);
    bool diskMatches = launchMatches && destination.identity() == launch.identity && launchBytes;
    // The loaded capture must already show the copy, so nothing changed it
    // between that capture and this one.
    const auto &publication = file.publication();
    const bool copiedLaunch = !diskMatches && !publication.written() && sourcesMatch && launchBytes
        && liveCapture->launchResult.text == launch.text && liveCapture->launchResult.identity == destination.identity()
        && sameFileCopy(launch.identity, destination.identity());
    if (const auto written = publication.written()) {
        const auto *revision = file.revision(*written);
        const auto bytes = revision ? serializedRevision(file, *revision) : std::nullopt;
        diskMatches = bytes && *bytes == destination.bytes();
    }
    bool indexMatches = matchesCapturedConflict(index.indexEntries, *captured);
    if (const auto &staged = publication.staged()) {
        const auto *revision = file.revision(staged->revision);
        const auto objectId =
            revision ? stagedIdentity(file, *revision, index.indexEntries, repository, probe) : std::nullopt;
        policyMatches = objectId.has_value();
        indexMatches = objectId && objectId->toStdString() == staged->objectId && staged->mode == captured->resultMode;
    }
    for (const auto &mutation : mutations) {
        if (mutation.completed || mutation.target.id() != file.id() || untouched(mutation)) continue;
        const auto bytes = serializedRevision(mutation.target, mutation.target.current());
        const bool resultPresent = bytes && *bytes == destination.bytes();
        return {resultPresent ? QStringLiteral("Interrupted %1: result bytes are present; completion is uncertain. "
                                               "Inspect disk and index before retrying.")
                                    .arg(kindName(mutation.kind))
                              : QStringLiteral("Interrupted %1: current disk/index evidence cannot establish "
                                               "completion. Outcome uncertain; no retry was performed.")
                                    .arg(kindName(mutation.kind)),
                {}};
    }
    const bool modeAgrees = modeMatches(destination.identity().mode, captured->resultMode);
    const auto finalDisk = probe.capture(repository.worktreeRoot, path);
    const auto finalIndex = probe.inspect(indexRequest(repository, path));
    if (!finalDisk.destination || finalDisk.destination->identity() != destination.identity()
        || finalDisk.destination->bytes() != destination.bytes() || finalIndex.outcome != git::Outcome::Success
        || finalIndex.standardOutput != index.standardOutput)
        return {QStringLiteral("Disk/index changed during recovery validation; work remains outstanding."), {}};
    if (copiedLaunch && indexMatches && policyMatches && modeAgrees)
        return {copiedLaunchFailure(), destination.identity()};
    if (!diskMatches || !indexMatches || !policyMatches || !modeAgrees)
        return {QStringLiteral("Recovered draft retained, but the repository file, index or conversion policy changed. "
                               "Work remains outstanding; no publication is authorized."),
                {}};
    return {};
}

QString revalidateCopiedLaunch(const FileSnapshots &captured, const SnapshotFileIdentity &identity,
                               const git::RepositoryContext &repository, const RepositoryProbe &probe)
{
    const auto path = QByteArray::fromStdString(captured.path);
    const auto validatedFile = [&] {
        const auto disk = probe.capture(repository.worktreeRoot, path);
        return disk.destination && disk.destination->identity() == identity
            && disk.destination->bytes() == QByteArray::fromStdString(captured.launchResult.text.bytes);
    };
    auto policyRequest = indexRequest(repository, path);
    policyRequest.operation = git::Operation::ReadAttributes;
    const auto changed =
        QStringLiteral("Recovered draft retained, but the repository file, index or conversion policy "
                       "changed after recovery validation. Work remains outstanding; no publication is authorized.");
    if (!validatedFile()) return changed;
    const auto index = probe.inspect(indexRequest(repository, path));
    if (index.outcome != git::Outcome::Success || !matchesCapturedConflict(index.indexEntries, captured))
        return changed;
    const auto policy = probe.inspect(policyRequest);
    if (policy.outcome != git::Outcome::Success || !samePolicy(captured.conversion, policy.conversionPolicy))
        return changed;
    // As in the restore, the file and index must not move while Git inspected them.
    const auto finalIndex = probe.inspect(indexRequest(repository, path));
    if (!validatedFile() || finalIndex.outcome != git::Outcome::Success
        || finalIndex.standardOutput != index.standardOutput)
        return changed;
    return {};
}
}
