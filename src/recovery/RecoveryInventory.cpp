// Recovery inventory lists, leases, chooses, sets aside and expires the stores under the recovery root.
#include "recovery/RecoveryRestore.h"
#include "recovery/SessionIdentity.h"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>
#include <algorithm>

namespace branch_truce {
namespace {
struct RecoveryCandidate {
    std::shared_ptr<storage::RecoveryLease> lease;
    bool userWork = false;
    RecoveryChoice choice;
    bool setAside = false;
    [[nodiscard]] QString name() const { return QFileInfo(lease->directory()).fileName(); }
};

QString setAsideMarker(const QString &directory) { return directory + QStringLiteral("/set-aside"); }

QDateTime lastChanged(const QString &directory)
{
    return QFileInfo(directory + QStringLiteral("/journal")).lastModified(QTimeZone::UTC);
}

bool expired(const QString &directory, const QDateTime &now)
{
    const auto marked = QFileInfo(setAsideMarker(directory)).lastModified(QTimeZone::UTC);
    return now.isValid() && marked.isValid() && marked.addDays(kSetAsideLifetimeDays) < now;
}

// Marks a store whose lease this launch holds. The journal and payloads stay
// complete and in place, so choosing the store later restores it.
Problem setAside(const QString &directory)
{
    const auto path = setAsideMarker(directory);
    QSaveFile marker(path);
    const auto text = QStringLiteral("Set aside by Branch Truce. Start Branch Truce with --recovery-session %1 to "
                                     "restore it within %2 days; after that it is deleted.\n")
                          .arg(QFileInfo(directory).fileName())
                          .arg(kSetAsideLifetimeDays)
                          .toUtf8();
    if (!marker.open(QIODevice::WriteOnly) || marker.write(text) != text.size() || !marker.commit())
        return {ProblemCode::RecoveryMarkerWrite, {marker.errorString()}};
    if (!QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return ProblemCode::RecoveryMarkerPermissions;
    return {};
}

const FileSession *firstCaptured(const std::vector<FileSession> &files)
{
    const auto found = std::find_if(files.begin(), files.end(), [](const auto &file) { return file.snapshots(); });
    return found == files.end() ? nullptr : &*found;
}

bool unrelated(const storage::RecoveryStore &store, const QString &worktree)
{
    const auto hint = store.read(true);
    if (!hint.succeeded || hint.transactions.empty()) return false;
    try {
        auto state = hint.transactions.front().state;
        if (state.contains("checkpointSchema")) {
            const auto files = state.value("files").toArray();
            if (files.empty()) return false;
            state = files.first().toObject().value("file").toObject();
        }
        return RecoveryRecordReader(store, state).worktreeRoot() != worktree.toStdString();
    } catch (const RecoveryRecordError &) {
        return false; // Unknown or malformed identity gets full replay and its diagnostic.
    }
}
}

Problem revalidateCopiedLaunch(const FileSnapshots &captured, const SnapshotFileIdentity &identity,
                               const git::RepositoryContext &repository, const git::GitRunners &git,
                               std::shared_ptr<const storage::FileSystem> files)
{
    return revalidateCopiedLaunch(captured, identity, repository, RepositoryProbe(git, std::move(files)));
}

RecoveryRestore restoreRecovery(const QString &inventory, const QString &exclude,
                                const git::RepositoryContext &repository, const std::vector<FileSession> &observed,
                                const git::GitRunners &git, const QString &chosenSession, const RestoreOptions &options)
{
    RecoveryRestore result;
    std::vector<RecoveryCandidate> candidates;
    bool chosenUnavailable = false;
    const InventoryLimits supported;
    auto limits = options.inventoryLimits;
    limits.sessions = std::clamp(limits.sessions, qsizetype{0}, supported.sessions);
    limits.entries = std::clamp(limits.entries, qsizetype{0}, supported.entries);
    limits.candidates = std::clamp(limits.candidates, qsizetype{0}, supported.candidates);
    limits.bytes = std::clamp(limits.bytes, qint64{0}, supported.bytes);
    limits.decodedBytes = std::clamp(limits.decodedBytes, qint64{0}, supported.decodedBytes);
    qsizetype sessions = 0, entries = 0;
    qint64 bytes = 0, decoded = 0;
    const auto incomplete = [&] {
        RecoveryRestore refused;
        refused.diagnostics.push_back(Problem{ProblemCode::RecoverySessionRejected,
                                              {QStringLiteral("inventory (discovery incomplete; no session selected)")}}
                                          .wrapping(ProblemCode::RecoveryResourceLimit));
        return refused; // An incomplete inventory never chooses from a prefix.
    };
    QDirIterator inventoryEntries(inventory, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    while (inventoryEntries.hasNext()) {
        inventoryEntries.next();
        if (++entries > limits.entries) return incomplete();
        const auto entry = inventoryEntries.fileInfo();
        if (!entry.isDir() && !entry.isSymLink()) continue;
        if (++sessions > limits.sessions) return incomplete();
        if (entry.absoluteFilePath() == exclude) continue;
        if (entry.isSymLink() || entry.ownerId() != options.files->effectiveUser()) {
            result.diagnostics.push_back(Problem{ProblemCode::RecoveryUnsafeDirectory, {entry.fileName()}});
            continue;
        }
        auto lease = storage::RecoveryLease::acquire(entry.absoluteFilePath(), options.files);
        if (!lease) {
            result.diagnostics.push_back(Problem{ProblemCode::RecoverySessionActive, {entry.fileName()}});
            chosenUnavailable = chosenUnavailable || entry.fileName() == chosenSession;
            continue;
        }
        // Streaming enumeration charges malformed, unrelated and expired stores
        // too, before a reader can allocate or any prefix can be adopted.
        QDirIterator contents(entry.absoluteFilePath(),
                              QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                              QDirIterator::Subdirectories);
        while (contents.hasNext()) {
            contents.next();
            const auto size = contents.fileInfo().size();
            if (++entries > limits.entries || size < 0 || size > (limits.bytes - bytes) / 4) return incomplete();
            bytes += 4 * size; // Hint, validation, replay and selected replay.
        }
        if (unrelated(lease->store(), repository.worktreeRoot)) continue;
        // A set-aside store returns only when the user names it, until it expires.
        const bool setAside = QFileInfo::exists(setAsideMarker(entry.absoluteFilePath()));
        if (setAside && entry.fileName() != chosenSession) {
            if (expired(entry.absoluteFilePath(), options.now)) {
                if (lease->remove())
                    result.diagnostics.push_back(Problem{ProblemCode::RecoverySessionExpired,
                                                         {entry.fileName(), QString::number(kSetAsideLifetimeDays)}});
            }
            continue;
        }
        if (candidates.size() >= static_cast<std::size_t>(limits.candidates)) return incomplete();
        const auto read = lease->store().read();
        if (read.retainedBytes > (limits.decodedBytes - decoded) / 2) return incomplete();
        decoded += 2 * read.retainedBytes; // Validation plus the possible selected replay.
        auto replay = replayRecovery(lease->store(), options.correspondence);
        if (!replay.succeeded) {
            result.diagnostics.push_back(
                Problem{ProblemCode::RecoverySessionRejected, {entry.fileName()}}.wrapping(replay.problem));
            continue;
        }
        const auto *captured = firstCaptured(replay.files);
        if (!captured) continue;
        const auto &provenance = captured->snapshots()->provenance;
        if (provenance.worktreeRoot != repository.worktreeRoot.toStdString()) continue;
        if (!sameOperation(provenance, repository)) {
            result.diagnostics.push_back(Problem{ProblemCode::RecoverySessionIdentity, {entry.fileName()}});
            continue;
        }
        RecoveryChoice choice{entry.fileName(), entry.absoluteFilePath(), lastChanged(entry.absoluteFilePath()), {}};
        for (const auto &file : replay.files)
            if (replay.editedFiles.contains(file.id())) choice.paths.push_back(file.path());
        candidates.push_back({std::move(lease), replay.userWork, std::move(choice), setAside});
    }

    RecoveryCandidate *adopted = nullptr;
    bool chosen = false;
    if (!chosenSession.isEmpty()) {
        const auto match = std::find_if(candidates.begin(), candidates.end(),
                                        [&](const auto &candidate) { return candidate.name() == chosenSession; });
        if (match == candidates.end()) {
            if (!chosenUnavailable)
                result.diagnostics.push_back(Problem{ProblemCode::RecoverySessionOtherOperation, {chosenSession}});
        } else if (match->setAside && !QFile::remove(setAsideMarker(match->lease->directory()))) {
            result.diagnostics.push_back(Problem{ProblemCode::RecoveryMarkerNotRemoved, {chosenSession}});
            candidates.erase(match);
        } else {
            adopted = &*match;
            chosen = true;
        }
    }
    // A named session that was not restored is never replaced by another
    // session with app edits. Only a lone store holding launch captures, which
    // leaves nothing to choose between, is still adopted.
    const bool lostChoice = !chosenSession.isEmpty() && !chosen;
    std::vector<RecoveryCandidate *> edited;
    for (auto &candidate : candidates)
        if (candidate.userWork) edited.push_back(&candidate);
    if (!adopted && candidates.size() == 1 && (!lostChoice || edited.empty())) adopted = &candidates.front();
    // Stores holding only launch captures carry no work, so they cannot make
    // the choice ambiguous. Several edited stores still block.
    if (!adopted && !lostChoice && edited.size() == 1) adopted = edited.front();
    if (!adopted && (edited.size() > 1 || (lostChoice && !edited.empty()))) {
        result.diagnostics.push_back(lostChoice ? Problem{ProblemCode::RecoveryChoiceLost, {chosenSession}}
                                                : Problem{ProblemCode::RecoveryChoicesNeeded});
        for (const auto *candidate : edited) {
            auto choice = candidate->choice;
            result.diagnostics.push_back(Problem{ProblemCode::RecoverySessionLastChanged,
                                                 {choice.session, choice.lastChanged.toString(Qt::ISODate)}});
            result.choices.push_back(std::move(choice));
        }
    }
    RecoveryReplay selected;
    if (adopted) {
        selected = replayRecovery(adopted->lease->store(), options.correspondence);
        if (!selected.succeeded) {
            result.diagnostics.push_back(
                Problem{ProblemCode::RecoverySessionRejected, {adopted->name()}}.wrapping(selected.problem));
            return result;
        }
    }
    int removedWithoutEdits = 0;
    for (auto &candidate : candidates) {
        if (&candidate == adopted) continue;
        // A store without app edits holds nothing to restore later.
        if (!candidate.userWork) {
            if (candidate.lease->remove()) ++removedWithoutEdits;
            continue;
        }
        if (candidates.size() < 2 || !chosen) continue;
        if (const auto error = setAside(candidate.lease->directory()); !error.isEmpty())
            result.diagnostics.push_back(
                Problem{ProblemCode::RecoverySetAsideFailed, {candidate.name()}}.wrapping(error));
        else
            result.diagnostics.push_back(
                Problem{ProblemCode::RecoverySetAside, {candidate.name(), QString::number(kSetAsideLifetimeDays)}});
    }
    if (removedWithoutEdits > 0)
        result.diagnostics.push_back(
            Problem{ProblemCode::RecoveryDeletedWithoutEdits, {QString::number(removedWithoutEdits)}});

    if (!adopted) return result;
    auto &replay = selected;
    const auto sessionId = firstCaptured(replay.files)->snapshots()->provenance.sessionId;
    result.found = true;
    AdoptedStore store{adopted->lease, replay.userWork, {}};
    if (replay.incompleteTail) result.diagnostics.push_back(Problem{ProblemCode::RecoveryTailIgnored});
    const RepositoryProbe probe(git, options.files);
    for (auto &file : replay.files) {
        const auto reconciled = reconcile(file, replay.mutations, observed, repository, probe, *adopted->lease);
        if (!reconciled.failure.isEmpty()) store.failures.emplace(file.id(), reconciled.failure);
        if (reconciled.copied) result.copiedLaunches.emplace(file.id(), *reconciled.copied);
        result.files.push_back(std::move(file));
    }
    for (const auto &file : observed) {
        if (std::any_of(result.files.begin(), result.files.end(),
                        [&](const auto &recovered) { return recovered.id() == file.id(); }))
            continue;
        result.newFiles.insert(file.id());
        auto snapshots = file.snapshots() ? std::optional<FileSnapshots>(*file.snapshots()) : std::nullopt;
        if (snapshots) snapshots->provenance.sessionId = sessionId;
        result.files.emplace_back(file.id(), file.path(), file.current().draft,
                                  FileLaunch{.support = file.support(),
                                             .supportDetail = file.supportDetail(),
                                             .snapshots = std::move(snapshots),
                                             .algorithm = options.correspondence});
    }
    git::Request request;
    request.operation = git::Operation::DiscoverRepository;
    request.directory = repository.worktreeRoot;
    const auto fresh = probe.inspect(request);
    for (const auto &file : result.files) {
        if (!file.snapshots()) continue;
        if (fresh.outcome != git::Outcome::Success || !sameOperation(file.snapshots()->provenance, fresh.repository)) {
            store.failures.insert_or_assign(file.id(), Problem{ProblemCode::RecoveryOperationChanged});
            result.copiedLaunches.erase(file.id());
        }
    }
    result.adopted = std::move(store);
    return result;
}
}
