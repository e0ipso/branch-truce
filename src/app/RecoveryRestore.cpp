#include "RecoveryRestore.h"
#include "storage/DestinationWriter.h"
#include "storage/TextFormat.h"
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>
#include <QUuid>
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <unistd.h>

namespace branch_truce {
namespace {
using Object = QJsonObject;
void require(bool condition, const char *detail)
{
    if (!condition) throw std::runtime_error(detail);
}
class Decoder {
public:
    explicit Decoder(const storage::RecoveryStore &store) : store(store) {}
    std::string bytes(const QJsonValue &value) const
    {
        require(value.isString(), "Missing recovery payload reference.");
        QString error;
        const auto result = store.payload(value.toString().toLatin1(), &error);
        require(error.isEmpty(), "Recovery payload is missing, unsafe or corrupt.");
        return result.toStdString();
    }
    quint64 number(const QJsonValue &value) const
    {
        bool ok = false;
        const auto result = value.toString().toULongLong(&ok);
        require(ok && QString::number(result) == value.toString(), "Invalid recovery integer.");
        return result;
    }
    qint64 signedNumber(const QJsonValue &value) const
    {
        bool ok = false;
        const auto result = value.toString().toLongLong(&ok);
        require(ok && QString::number(result) == value.toString(), "Invalid recovery signed integer.");
        return result;
    }
    bool boolean(const QJsonValue &value) const
    {
        require(value.isBool(), "Invalid recovery boolean.");
        return value.toBool();
    }
    template<typename Enum> Enum enumeration(const QJsonValue &value, int maximum) const
    {
        const auto integer = value.toInt(-1);
        require(value.isDouble() && value.toDouble() == integer && integer >= 0 && integer <= maximum,
                "Unsupported recovery enum value.");
        return static_cast<Enum>(integer);
    }
    Draft draft(const QJsonValue &value) const
    {
        const auto object = value.toObject();
        // The recorded marker count is advisory. FileSession scans the text
        // again, and the replayed state must encode the same count.
        number(object.value("unresolvedMarkers"));
        Draft result{bytes(object.value("text")), {}, 0};
        require(validUtf8(result.text) && object.value("decisions").isArray(), "Invalid recovered draft.");
        for (const auto &entry : object.value("decisions").toArray()) {
            const auto decision = entry.toObject();
            const auto id = bytes(decision.value("id"));
            Decision state{enumeration<DecisionValue>(decision.value("value"), 2), {}};
            if (decision.contains("order")) state.order = number(decision.value("order"));
            require(result.decisions.emplace(id, state).second, "Duplicate recovered decision.");
        }
        return result;
    }
    // Records written before hand edits were journaled have none.
    std::set<std::string> handEdited(const QJsonValue &value) const
    {
        std::set<std::string> result;
        if (value.isUndefined()) return result;
        require(value.isArray(), "Invalid recovered hand edits.");
        for (const auto &entry : value.toArray())
            require(result.insert(bytes(entry)).second, "Duplicate recovered hand edit.");
        return result;
    }
    TextSnapshot text(const QJsonValue &value) const
    {
        const auto object = value.toObject();
        const auto format = object.value("format").toObject();
        TextSnapshot result{bytes(object.value("utf8")), bytes(object.value("bytes")),
            {enumeration<SnapshotFormat::Encoding>(format.value("encoding"), 2),
             boolean(format.value("bom")), enumeration<SnapshotFormat::Newline>(format.value("newline"), 1),
             boolean(format.value("finalNewline"))}};
        require(validUtf8(result.utf8), "Invalid recovered text encoding.");
        const auto encoding = result.format.encoding == SnapshotFormat::Encoding::Utf8 ? QByteArray{}
            : result.format.encoding == SnapshotFormat::Encoding::Utf16LittleEndian ? QByteArray("UTF-16LE") : QByteArray("UTF-16BE");
        const auto decoded = storage::decodeText(QByteArray::fromStdString(result.bytes), encoding);
        require(decoded.document && decoded.document->utf8.toStdString() == result.utf8,
                "Recovered text bytes and editable representation disagree.");
        require(static_cast<int>(decoded.document->format.encoding) == static_cast<int>(result.format.encoding)
            && decoded.document->format.bom == result.format.bom
            && static_cast<int>(decoded.document->format.newline) == static_cast<int>(result.format.newline)
            && decoded.document->format.finalNewline == result.format.finalNewline,
            "Recovered text format does not describe its bytes.");
        return result;
    }
    std::optional<SourceSnapshot> source(const QJsonValue &value) const
    {
        if (value.isNull()) return {};
        const auto object = value.toObject();
        SourceSnapshot result{bytes(object.value("objectId")), bytes(object.value("mode")),
            bytes(object.value("rawBytes")), text(object.value("worktree")), {}, bytes(object.value("label"))};
        if (!object.value("commitId").isNull()) result.commitId = bytes(object.value("commitId"));
        require(!result.objectId.empty() && (result.mode == "100644" || result.mode == "100755"),
                "Invalid recovered source identity.");
        return result;
    }
    std::map<std::string, std::string> mapping(const QJsonValue &value) const
    {
        require(value.isArray(), "Invalid recovery conversion policy.");
        std::map<std::string, std::string> result;
        for (const auto &entry : value.toArray()) {
            const auto object = entry.toObject();
            require(result.emplace(bytes(object.value("key")), bytes(object.value("value"))).second,
                    "Duplicate recovery conversion policy entry.");
        }
        return result;
    }
    std::optional<FileSnapshots> snapshots(const QJsonValue &value) const
    {
        if (value.isNull()) return {};
        const auto object = value.toObject();
        const auto p = object.value("provenance").toObject();
        const auto launch = object.value("launchResult").toObject();
        const auto identity = launch.value("identity").toObject();
        FileSnapshots result;
        result.path = bytes(object.value("path"));
        result.resultMode = bytes(object.value("resultMode"));
        result.earlier = source(object.value("earlier"));
        result.stage2 = source(object.value("stage2"));
        result.stage3 = source(object.value("stage3"));
        const auto policy = object.value("conversion").toObject();
        result.conversion = {mapping(policy.value("attributes")), mapping(policy.value("configuration"))};
        auto &provenance = result.provenance;
        provenance.sessionId = bytes(p.value("sessionId"));
        provenance.worktreeRoot = bytes(p.value("worktreeRoot"));
        provenance.gitDirectory = bytes(p.value("gitDirectory"));
        provenance.operation = enumeration<SnapshotProvenance::Operation>(p.value("operation"), 1);
        provenance.stage2CommitId = bytes(p.value("stage2CommitId"));
        provenance.stage3CommitId = bytes(p.value("stage3CommitId"));
        if (!p.value("baseCommitId").isNull()) provenance.baseCommitId = bytes(p.value("baseCommitId"));
        provenance.headName = bytes(p.value("headName"));
        provenance.originalHead = bytes(p.value("originalHead"));
        provenance.onto = bytes(p.value("onto"));
        require(!QUuid(QString::fromStdString(provenance.sessionId)).isNull(), "Invalid recovery session identity.");
        require(result.stage2 && result.stage3 && result.stage2->mode == result.resultMode
                    && result.stage3->mode == result.resultMode, "Incomplete recovered source captures.");
        const auto mode = number(identity.value("mode"));
        require(mode <= std::numeric_limits<std::uint32_t>::max(), "Invalid recovery file mode.");
        result.launchResult = {text(launch.value("text")),
            {number(identity.value("device")), number(identity.value("inode")), static_cast<std::uint32_t>(mode),
             signedNumber(identity.value("size")), signedNumber(identity.value("modifiedSeconds")),
             signedNumber(identity.value("modifiedNanoseconds")), signedNumber(identity.value("changedSeconds")),
             signedNumber(identity.value("changedNanoseconds"))}};
        return result;
    }
    OperationOutcome outcome(const QJsonValue &value) const
    {
        const auto object = value.toObject();
        return {number(object.value("revision")), enumeration<Outcome>(object.value("outcome"), 2), bytes(object.value("detail"))};
    }
private:
    const storage::RecoveryStore &store;
};

bool sameOperation(const SnapshotProvenance &p, const git::RepositoryContext &repository)
{
    return p.worktreeRoot == repository.worktreeRoot.toStdString()
        && p.gitDirectory == repository.gitDirectory.toStdString()
        && ((p.operation == SnapshotProvenance::Operation::Merge && repository.operation == git::RepositoryOperation::Merge)
            || (p.operation == SnapshotProvenance::Operation::Rebase && repository.operation == git::RepositoryOperation::Rebase))
        && p.stage2CommitId == repository.stage2.commitId.toStdString()
        && p.stage3CommitId == repository.stage3.commitId.toStdString()
        && p.baseCommitId.value_or("") == repository.baseCommitId.toStdString()
        && p.headName == repository.headName.toStdString() && p.originalHead == repository.originalHead.toStdString()
        && p.onto == repository.onto.toStdString();
}

git::Result inspect(const QString &executable, const git::Request &request)
{
    git::GitAdapter adapter(executable);
    QEventLoop loop;
    git::Result result;
    QObject::connect(&adapter, &git::GitAdapter::finished, &loop, [&](const git::Result &value) {
        result = value;
        loop.quit();
    });
    if (adapter.start(request)) loop.exec();
    return result;
}

std::optional<QByteArray> serialized(const FileSession &file, const Revision &revision)
{
    const auto *snapshots = file.snapshots();
    if (!snapshots) return {};
    const auto &format = snapshots->launchResult.text.format;
    const storage::TextFormat target{static_cast<storage::TextEncoding>(format.encoding), format.bom,
        static_cast<storage::Newline>(format.newline), format.finalNewline};
    const auto result = storage::serializeText(QByteArray::fromStdString(revision.draft.text), target);
    return result.document ? std::optional<QByteArray>(result.document->originalBytes) : std::nullopt;
}

bool originalIndex(const QList<git::IndexEntry> &entries, const FileSnapshots &snapshots)
{
    if (entries.size() != (snapshots.earlier ? 3 : 2)) return false;
    bool seen[4]{};
    for (const auto &entry : entries) {
        const auto &source = entry.stage == 1 ? snapshots.earlier
            : entry.stage == 2 ? snapshots.stage2 : snapshots.stage3;
        if (entry.stage < 1 || entry.stage > 3 || seen[entry.stage] || !source
            || entry.path.toStdString() != snapshots.path || entry.objectId.toStdString() != source->objectId
            || entry.mode.toStdString() != source->mode) return false;
        seen[entry.stage] = true;
    }
    return true;
}

ConversionSnapshot conversionSnapshot(const git::ConversionPolicy &live)
{
    ConversionSnapshot policy;
    for (auto it = live.attributes.cbegin(); it != live.attributes.cend(); ++it)
        policy.attributes.emplace(it.key().toStdString(), it.value().toStdString());
    for (auto it = live.configuration.cbegin(); it != live.configuration.cend(); ++it)
        policy.configuration.emplace(it.key().toStdString(), it.value().toStdString());
    return policy;
}

std::optional<QByteArray> stagedIdentity(const FileSession &file, const Revision &revision,
    const QList<git::IndexEntry> &entries, const git::RepositoryContext &repository, const QString &executable)
{
    const auto *snapshots = file.snapshots();
    const auto path = QByteArray::fromStdString(file.path());
    if (!snapshots || entries.size() != 1 || entries.front().stage != 0 || entries.front().path != path
        || entries.front().mode.toStdString() != snapshots->resultMode) return {};
    const auto bytes = serialized(file, revision);
    if (!bytes) return {};
    git::Request request;
    request.operation = git::Operation::CanonicalizeText;
    request.directory = repository.worktreeRoot;
    request.path = path;
    request.input = *bytes;
    const auto canonical = inspect(executable, request);
    if (canonical.outcome != git::Outcome::Success || conversionSnapshot(canonical.conversionPolicy) != snapshots->conversion
        || canonical.canonicalObjectId != entries.front().objectId) return {};
    return canonical.canonicalObjectId;
}

void completeMutations(std::vector<RecoveryMutation> &mutations, const std::string &fileId,
                       const QString &kind, RevisionId revision)
{
    for (auto &mutation : mutations) {
        if (mutation.kind == kind && mutation.target.id() == fileId
            && mutation.target.current().id == revision) mutation.completed = true;
    }
}

// Git mergetool replaces MERGED with a copy of the same file: a new inode and
// new timestamps, nothing else.
bool sameFileCopy(const SnapshotFileIdentity &launch, const SnapshotFileIdentity &live)
{
    return live.device == launch.device && live.mode == launch.mode && live.size == launch.size;
}

// copied receives the live identity when the only change is such a copy. The
// file still fails; the caller decides whether a waiting launcher proves it.
QString reconcile(FileSession &file, std::vector<RecoveryMutation> &mutations,
                  const std::vector<FileSession> &observed, const git::RepositoryContext &repository,
                  const QString &executable, const QString &directory,
                  std::optional<SnapshotFileIdentity> &copied)
{
    copied.reset();
    const auto *captured = file.snapshots();
    if (!captured) return file.support() == Support::Supported
        ? QStringLiteral("Recovery has no admitted source and result captures; file remains outstanding.") : QString{};
    const auto path = QByteArray::fromStdString(file.path());
    if (file.id() != path.toHex().toStdString()) return QStringLiteral("Recovered file identity does not match its exact path.");
    const auto disk = storage::captureDestination(repository.worktreeRoot, path);
    if (!disk.destination) return QStringLiteral("Recovery could not validate the current repository file: %1").arg(disk.diagnostic);
    git::Request indexRequest;
    indexRequest.operation = git::Operation::InspectIndex;
    indexRequest.directory = repository.worktreeRoot;
    indexRequest.path = path;
    const auto index = inspect(executable, indexRequest);
    if (index.outcome != git::Outcome::Success) return QStringLiteral("Recovery could not inspect the current Git index.");

    // A crash can lose the completion callback after Git installed the exact
    // reviewed result. Verify content, policy, mode and the written revision,
    // then persist that observation before exposing a resolved state.
    for (auto &mutation : mutations) {
        if (mutation.completed || mutation.kind != QLatin1String("staging") || mutation.target.id() != file.id()
            || mutation.observation == QLatin1String("not-attempted") || mutation.observation == QLatin1String("unchanged")
            || !file.written() || file.written()->id != mutation.target.current().id) continue;
        const auto bytes = serialized(file, *file.written());
        const auto objectId = stagedIdentity(file, *file.written(), index.indexEntries, repository, executable);
        if (!bytes || !objectId || *bytes != disk.destination->bytes()
            || (((disk.destination->identity().mode & 0111U) != 0) != (captured->resultMode == "100755"))) continue;
        auto candidate = file;
        if (candidate.recordStage(file.written()->id, Outcome::Succeeded, objectId->toStdString(), captured->resultMode,
                "Recovered staging completion verified against disk and index") != Transition::Applied) continue;
        const auto committed = storage::RecoveryStore(directory).commit(recoveryTransaction(candidate,
            QStringLiteral("stage-outcome"), QUuid::createUuid().toString(QUuid::WithoutBraces)));
        if (!committed.succeeded)
            return QStringLiteral("Staging evidence matched, but recovery could not retain its completion: %1").arg(committed.error);
        file = std::move(candidate);
        captured = file.snapshots();
        completeMutations(mutations, file.id(), QStringLiteral("staging"), file.written()->id);
    }

    const auto live = std::find_if(observed.begin(), observed.end(), [&](const auto &entry) { return entry.id() == file.id(); });
    const bool sourcesMatch = live != observed.end() && live->snapshots()
        && live->snapshots()->conversion == captured->conversion
        && live->snapshots()->earlier == captured->earlier && live->snapshots()->stage2 == captured->stage2
        && live->snapshots()->stage3 == captured->stage3;
    const bool launchMatches = sourcesMatch && live->snapshots()->launchResult == captured->launchResult;
    bool policyMatches = live != observed.end() && live->snapshots()
        && live->snapshots()->conversion == captured->conversion;
    const auto &launch = captured->launchResult;
    const bool launchBytes = disk.destination->bytes() == QByteArray::fromStdString(launch.text.bytes);
    bool diskMatches = launchMatches && disk.destination->identity() == launch.identity && launchBytes;
    // The loaded capture must already show the copy, so nothing changed it
    // between that capture and this one.
    const bool copiedLaunch = !diskMatches && !file.written() && sourcesMatch && launchBytes
        && live->snapshots()->launchResult.text == launch.text
        && live->snapshots()->launchResult.identity == disk.destination->identity()
        && sameFileCopy(launch.identity, disk.destination->identity());
    if (file.written()) {
        const auto bytes = serialized(file, *file.written());
        diskMatches = bytes && *bytes == disk.destination->bytes();
    }
    bool indexMatches = originalIndex(index.indexEntries, *captured);
    if (file.staged()) {
        const auto &staged = *file.staged();
        const auto objectId = stagedIdentity(file, staged.revision, index.indexEntries, repository, executable);
        policyMatches = objectId.has_value();
        indexMatches = objectId && objectId->toStdString() == staged.objectId && staged.mode == captured->resultMode;
    }
    for (const auto &mutation : mutations) {
        if (mutation.completed || mutation.target.id() != file.id() || mutation.observation == QLatin1String("not-attempted")
            || mutation.observation == QLatin1String("unchanged")) continue;
        const auto bytes = serialized(mutation.target, mutation.target.current());
        const bool resultPresent = bytes && *bytes == disk.destination->bytes();
        return resultPresent
            ? QStringLiteral("Interrupted %1: result bytes are present; completion is uncertain. Inspect disk and index before retrying.").arg(mutation.kind)
            : QStringLiteral("Interrupted %1: current disk/index evidence cannot establish completion. Outcome uncertain; no retry was performed.").arg(mutation.kind);
    }
    const bool modeMatches = ((disk.destination->identity().mode & 0111U) != 0) == (captured->resultMode == "100755");
    const auto finalDisk = storage::captureDestination(repository.worktreeRoot, path);
    const auto finalIndex = inspect(executable, indexRequest);
    if (!finalDisk.destination || finalDisk.destination->identity() != disk.destination->identity()
        || finalDisk.destination->bytes() != disk.destination->bytes() || finalIndex.outcome != git::Outcome::Success
        || finalIndex.standardOutput != index.standardOutput)
        return QStringLiteral("Disk/index changed during recovery validation; work remains outstanding.");
    if (copiedLaunch && indexMatches && policyMatches && modeMatches) {
        copied = disk.destination->identity();
        return copiedLaunchFailure();
    }
    if (!diskMatches || !indexMatches || !policyMatches || !modeMatches)
        return QStringLiteral("Recovered draft retained, but the repository file, index or conversion policy changed. Work remains outstanding; no publication is authorized.");
    return {};
}
}

QString copiedLaunchFailure()
{
    return QStringLiteral("Recovered draft retained, but the repository file was replaced by a copy with the same bytes. "
        "Work remains outstanding unless git mergetool is waiting on this path with those bytes.");
}

QString revalidateCopiedLaunch(const FileSnapshots &captured, const SnapshotFileIdentity &identity,
    const git::RepositoryContext &repository, const QString &executable)
{
    const auto path = QByteArray::fromStdString(captured.path);
    const auto validatedFile = [&] {
        const auto disk = storage::captureDestination(repository.worktreeRoot, path);
        return disk.destination && disk.destination->identity() == identity
            && disk.destination->bytes() == QByteArray::fromStdString(captured.launchResult.text.bytes);
    };
    git::Request indexRequest;
    indexRequest.operation = git::Operation::InspectIndex;
    indexRequest.directory = repository.worktreeRoot;
    indexRequest.path = path;
    git::Request policyRequest = indexRequest;
    policyRequest.operation = git::Operation::ReadAttributes;
    const auto changed = QStringLiteral("Recovered draft retained, but the repository file, index or conversion policy "
        "changed after recovery validation. Work remains outstanding; no publication is authorized.");
    if (!validatedFile()) return changed;
    const auto index = inspect(executable, indexRequest);
    if (index.outcome != git::Outcome::Success || !originalIndex(index.indexEntries, captured)) return changed;
    const auto policy = inspect(executable, policyRequest);
    if (policy.outcome != git::Outcome::Success || conversionSnapshot(policy.conversionPolicy) != captured.conversion)
        return changed;
    // As in the restore, the file and index must not move while Git inspected them.
    const auto finalIndex = inspect(executable, indexRequest);
    if (!validatedFile() || finalIndex.outcome != git::Outcome::Success || finalIndex.standardOutput != index.standardOutput)
        return changed;
    return {};
}

storage::RecoveryTransaction mutationRecoveryTransaction(const FileSession &file, const QString &kind,
    const QString &id, const QString &observation)
{
    auto result = recoveryTransaction(file, observation.isEmpty() ? QStringLiteral("mutation-intent")
        : QStringLiteral("mutation-observation"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    result.state.insert(QStringLiteral("mutationId"), id);
    result.state.insert(QStringLiteral("mutationKind"), kind);
    if (!observation.isEmpty()) result.state.insert(QStringLiteral("observation"), observation);
    return result;
}

RecoveryReplay replayRecovery(const storage::RecoveryStore &store)
{
    RecoveryReplay result;
    const auto read = store.read();
    if (!read.succeeded) { result.error = read.error; return result; }
    result.incompleteTail = read.incompleteTail;
    Decoder decoder(store);
    std::map<std::string, FileSession> files;
    std::map<QString, Object> intents;
    try {
        for (auto state : read.transactions) {
            require(state.value("schema").toInt() == 1, "Unsupported recovery state schema.");
            const auto transactionId = state.value("transactionId").toString();
            require(!QUuid(transactionId).isNull(), "Invalid recovery transaction identity.");
            const auto action = state.value("action").toString();
            const auto id = decoder.bytes(state.value("fileId"));
            const auto path = decoder.bytes(state.value("path"));
            require(!id.empty() && !path.empty() && path.find('\0') == std::string::npos,
                    "Invalid recovery file identity.");
            auto found = files.find(id);
            if (action == QLatin1String("open")) {
                require(found == files.end() || (!found->second.snapshots() && found->second.current().id == 1
                    && found->second.current().draft == Draft{} && !found->second.checkpoint()),
                    "Recovery attempted to replace an acknowledged session.");
                const auto draft = decoder.draft(state.value("current").toObject().value("draft"));
                auto snapshots = decoder.snapshots(state.value("snapshots"));
                files.insert_or_assign(id, FileSession(id, path, draft,
                    decoder.enumeration<Support>(state.value("support"), 2),
                    decoder.bytes(state.value("supportDetail")), std::move(snapshots)));
                found = files.find(id);
            } else {
                require(found != files.end() && found->second.path() == path, "Recovery command has no matching open file.");
                result.userWork = true;
                result.editedFiles.insert(id);
                auto &file = found->second;
                Transition transition = Transition::InvalidOutcome;
                if (action == QLatin1String("replace-draft")) {
                    const auto current = state.value("current").toObject();
                    transition = file.replaceDraft(file.current().id, decoder.draft(current.value("draft")),
                        decoder.handEdited(current.value("handEdited")));
                } else if (action == QLatin1String("undo")) transition = file.undo(file.current().id);
                else if (action == QLatin1String("redo")) transition = file.redo(file.current().id);
                else if (action == QLatin1String("discard-to-checkpoint")) transition = file.discardToCheckpoint(file.current().id);
                else if (action == QLatin1String("discard-to-staged")) transition = file.discardToStaged(file.current().id);
                else if (action == QLatin1String("mark-resolved-review"))
                    transition = file.beginMarkResolved(file.current().id,
                        decoder.boolean(state.value("reviewed").toObject().value("confirmedUndecided")));
                else if (action == QLatin1String("manual-save")) {
                    const auto outcome = decoder.outcome(state.value("checkpointOutcome"));
                    transition = file.recordCheckpoint(outcome.revision, outcome.outcome, outcome.detail);
                } else if (action == QLatin1String("write-outcome")) {
                    const auto outcome = decoder.outcome(state.value("writeOutcome"));
                    transition = file.recordWrite(outcome.revision, outcome.outcome, outcome.detail);
                } else if (action == QLatin1String("stage-outcome")) {
                    const auto outcome = decoder.outcome(state.value("stageOutcome"));
                    const auto staged = state.value("staged").toObject();
                    transition = file.recordStage(outcome.revision, outcome.outcome,
                        outcome.outcome == Outcome::Succeeded ? decoder.bytes(staged.value("objectId")) : std::string{},
                        outcome.outcome == Outcome::Succeeded ? decoder.bytes(staged.value("mode")) : std::string{}, outcome.detail);
                } else if (action == QLatin1String("support"))
                    transition = file.setSupport(decoder.enumeration<Support>(state.value("support"), 2),
                        decoder.bytes(state.value("supportDetail")));
                else if (action == QLatin1String("mutation-intent") || action == QLatin1String("mutation-observation")) {
                    const auto mutationId = state.take("mutationId").toString();
                    const auto kind = state.take("mutationKind").toString();
                    require(!QUuid(mutationId).isNull() && (kind == QLatin1String("publication") || kind == QLatin1String("staging")),
                            "Invalid recovery mutation identity.");
                    if (action == QLatin1String("mutation-intent")) {
                        require(intents.emplace(mutationId, state).second, "Duplicate recovery mutation intent.");
                        result.mutations.push_back({mutationId, kind, {}, file});
                    } else {
                        const auto observation = state.take("observation").toString();
                        require(observation == QLatin1String("not-attempted") || observation == QLatin1String("unchanged")
                            || observation == QLatin1String("changed") || observation == QLatin1String("uncertain"),
                            "Invalid recovery mutation observation.");
                        const auto intent = intents.find(mutationId);
                        require(intent != intents.end(), "Recovery mutation observation has no intent.");
                        auto expected = intent->second;
                        expected.insert("action", action);
                        expected.insert("transactionId", transactionId);
                        require(expected == state, "Recovery mutation observation changed its original target.");
                        const auto mutation = std::find_if(result.mutations.begin(), result.mutations.end(),
                            [&](const auto &entry) { return entry.id == mutationId; });
                        require(mutation != result.mutations.end() && mutation->kind == kind && mutation->observation.isEmpty(),
                                "Duplicate or mismatched recovery mutation observation.");
                        mutation->observation = observation;
                        continue; // An old completion never replaces a newer draft or discard.
                    }
                    transition = Transition::Applied;
                }
                require(transition == Transition::Applied, "Unsupported or inconsistent recovery transition.");
            }
            const auto encoded = recoveryTransaction(found->second, action, transactionId).state;
            require(encoded.value("snapshots") == state.value("snapshots"), "Recovery session or immutable capture identity changed.");
            require(encoded == state,
                    "Recovery state schema or transition does not match the acknowledged record.");
            if (action == QLatin1String("write-outcome") || action == QLatin1String("stage-outcome")) {
                const auto kind = action == QLatin1String("write-outcome") ? QStringLiteral("publication") : QStringLiteral("staging");
                const auto &outcome = action == QLatin1String("write-outcome") ? found->second.writeOutcome() : found->second.stageOutcome();
                // Match completion in journal order before a later revision can
                // replace the file's latest outcome, or a new intent can reuse it.
                if (outcome && outcome->outcome == Outcome::Succeeded)
                    completeMutations(result.mutations, id, kind, outcome->revision);
            }
        }
        std::optional<SnapshotProvenance> provenance;
        for (auto &[id, file] : files) {
            (void)id;
            if (const auto *snapshots = file.snapshots()) {
                require(!provenance || *provenance == snapshots->provenance, "Recovery session identities disagree.");
                provenance = snapshots->provenance;
            }
            result.files.push_back(std::move(file));
        }
        result.succeeded = true;
    } catch (const std::exception &error) {
        result.error = QString::fromUtf8(error.what());
        result.files.clear();
        result.mutations.clear();
        result.editedFiles.clear();
    }
    return result;
}

namespace {
struct RecoveryCandidate {
    QString directory;
    std::shared_ptr<QLockFile> lease;
    RecoveryReplay replay;
    bool setAside = false;
};

QString setAsideMarker(const QString &directory)
{
    return directory + QStringLiteral("/set-aside");
}

QString lastChanged(const QString &directory)
{
    return QFileInfo(directory + QStringLiteral("/journal")).lastModified().toString(Qt::ISODate);
}

// Marks a store whose lease this launch holds. The journal and payloads stay
// complete and in place, so choosing the store later restores it.
QString setAside(const QString &directory)
{
    const auto path = setAsideMarker(directory);
    QSaveFile marker(path);
    const auto text = QStringLiteral("Set aside by Branch Truce on %1. Start Branch Truce with "
        "--recovery-session %2 to restore it.\n").arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate),
        QFileInfo(directory).fileName()).toUtf8();
    if (!marker.open(QIODevice::WriteOnly) || marker.write(text) != text.size() || !marker.commit())
        return marker.errorString();
    if (!QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return QStringLiteral("Could not restrict the set-aside marker permissions.");
    return {};
}
}

RecoveryRestore restoreRecovery(const QString &inventory, const QString &exclude,
    const git::RepositoryContext &repository, const std::vector<FileSession> &observed, const QString &executable,
    const QString &chosenSession)
{
    RecoveryRestore result;
    std::vector<RecoveryCandidate> candidates;
    bool chosenUnavailable = false;
    for (const auto &entry : QDir(inventory).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (entry.absoluteFilePath() == exclude) continue;
        if (entry.isSymLink() || entry.ownerId() != ::geteuid()) {
            result.diagnostics.push_back(QStringLiteral("Unsafe recovery directory rejected: %1").arg(entry.fileName()));
            continue;
        }
        auto lease = std::make_shared<QLockFile>(entry.absoluteFilePath() + QStringLiteral("/session.lock"));
        if (!lease->tryLock(0)) {
            result.diagnostics.push_back(QStringLiteral("Recovery session %1 is active or unavailable; it was not restored.").arg(entry.fileName()));
            chosenUnavailable = chosenUnavailable || entry.fileName() == chosenSession;
            continue;
        }
        // A set-aside store returns only when the user names it.
        const bool setAside = QFileInfo::exists(setAsideMarker(entry.absoluteFilePath()));
        if (setAside && entry.fileName() != chosenSession) continue;
        auto replay = replayRecovery(storage::RecoveryStore(entry.absoluteFilePath()));
        if (!replay.succeeded) {
            result.diagnostics.push_back(QStringLiteral("Recovery %1 rejected: %2").arg(entry.fileName(), replay.error));
            continue;
        }
        const auto captured = std::find_if(replay.files.begin(), replay.files.end(), [](const auto &file) { return file.snapshots(); });
        if (captured == replay.files.end()) continue;
        const auto &provenance = captured->snapshots()->provenance;
        if (provenance.worktreeRoot != repository.worktreeRoot.toStdString()) continue;
        if (!repository.supported || !sameOperation(provenance, repository)) {
            result.diagnostics.push_back(QStringLiteral("Recovery %1 rejected: worktree/operation session identity does not match.").arg(entry.fileName()));
            continue;
        }
        candidates.push_back({entry.absoluteFilePath(), std::move(lease), std::move(replay), setAside});
    }

    RecoveryCandidate *adopted = nullptr;
    bool chosen = false;
    if (!chosenSession.isEmpty()) {
        const auto match = std::find_if(candidates.begin(), candidates.end(),
            [&](const auto &candidate) { return QFileInfo(candidate.directory).fileName() == chosenSession; });
        if (match == candidates.end()) {
            if (!chosenUnavailable)
                result.diagnostics.push_back(QStringLiteral("Recovery session %1 does not match this operation; it was not restored.").arg(chosenSession));
        } else if (match->setAside && !QFile::remove(setAsideMarker(match->directory))) {
            result.diagnostics.push_back(QStringLiteral("Recovery session %1 is set aside and its marker could not be removed; it was not restored.").arg(chosenSession));
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
        if (candidate.replay.userWork) edited.push_back(&candidate);
    if (!adopted && candidates.size() == 1 && (!lostChoice || edited.empty())) adopted = &candidates.front();
    // Stores holding only launch captures carry no work, so they cannot make
    // the choice ambiguous. Several edited stores still block.
    if (!adopted && !lostChoice && edited.size() == 1) adopted = edited.front();
    if (!adopted && (edited.size() > 1 || (lostChoice && !edited.empty()))) {
        result.diagnostics.push_back((lostChoice
            ? QStringLiteral("Recovery session %1 was not restored, and no other recovery session with app edits is restored in its place; "
                "restoration is blocked rather than guessing. ").arg(chosenSession)
            : QStringLiteral("Multiple recovery sessions with app edits match this operation; restoration is blocked rather than guessing. "))
            + QStringLiteral("To restore one, choose it in this window, or close every Branch Truce window and start Branch Truce with "
                "--recovery-session followed by the session to keep. The other sessions are set aside, not deleted."));
        for (const auto *candidate : edited) {
            RecoveryChoice choice{QFileInfo(candidate->directory).fileName(), candidate->directory,
                                  lastChanged(candidate->directory), {}};
            for (const auto &file : candidate->replay.files)
                if (candidate->replay.editedFiles.contains(file.id())) choice.paths.push_back(file.path());
            result.diagnostics.push_back(QStringLiteral("Recovery session %1 was last changed %2.")
                .arg(choice.session, choice.lastChanged));
            result.choices.push_back(std::move(choice));
        }
    }
    int setAsideWithoutEdits = 0;
    if (candidates.size() > 1) {
        for (auto &candidate : candidates) {
            if (&candidate == adopted || (!chosen && candidate.replay.userWork)) continue;
            const auto name = QFileInfo(candidate.directory).fileName();
            if (const auto error = setAside(candidate.directory); !error.isEmpty())
                result.diagnostics.push_back(QStringLiteral("Recovery session %1 could not be set aside: %2").arg(name, error));
            else if (candidate.replay.userWork)
                result.diagnostics.push_back(QStringLiteral("Recovery session %1 was set aside, not deleted.").arg(name));
            else
                ++setAsideWithoutEdits;
        }
    }
    if (setAsideWithoutEdits > 0)
        result.diagnostics.push_back(QStringLiteral("Recovery sessions without app edits that were set aside: %1.").arg(setAsideWithoutEdits));

    if (adopted) {
        auto &replay = adopted->replay;
        const auto captured = std::find_if(replay.files.begin(), replay.files.end(), [](const auto &file) { return file.snapshots(); });
        const auto sessionId = captured->snapshots()->provenance.sessionId;
        result.found = true;
        result.directory = adopted->directory;
        result.lease = std::move(adopted->lease);
        if (replay.incompleteTail)
            result.diagnostics.push_back(QStringLiteral("An interrupted final recovery record was ignored. Pending input may need repeating."));
        for (auto &file : replay.files) {
            std::optional<SnapshotFileIdentity> copied;
            const auto failure = reconcile(file, replay.mutations, observed, repository, executable, adopted->directory, copied);
            if (!failure.isEmpty()) result.failures.emplace(file.id(), failure);
            if (copied) result.copiedLaunches.emplace(file.id(), *copied);
            result.files.push_back(std::move(file));
        }
        for (const auto &file : observed) {
            if (std::none_of(result.files.begin(), result.files.end(), [&](const auto &recovered) { return recovered.id() == file.id(); })) {
                result.newFiles.insert(file.id());
                auto snapshots = file.snapshots() ? std::optional<FileSnapshots>(*file.snapshots()) : std::nullopt;
                if (snapshots) snapshots->provenance.sessionId = sessionId;
                result.files.emplace_back(file.id(), file.path(), file.current().draft, file.support(),
                    file.supportDetail(), std::move(snapshots));
            }
        }
    }
    if (result.found) {
        git::Request request;
        request.operation = git::Operation::DiscoverRepository;
        request.directory = repository.worktreeRoot;
        const auto fresh = inspect(executable, request);
        for (const auto &file : result.files) {
            if (!file.snapshots()) continue;
            if (fresh.outcome != git::Outcome::Success || !fresh.repository.supported
                || !sameOperation(file.snapshots()->provenance, fresh.repository)) {
                result.failures.insert_or_assign(file.id(), QStringLiteral("Operation identity changed during recovery validation; work remains outstanding."));
                result.copiedLaunches.erase(file.id());
            }
        }
    }
    return result;
}
}
