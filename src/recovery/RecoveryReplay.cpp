// Recovery replay rebuilds file sessions from a journal by repeating each recorded domain transition.
#include "recovery/RecoveryReplay.h"
#include <QJsonArray>
#include <QUuid>
#include <algorithm>
#include <map>

namespace branch_truce {
namespace {

void completeMutations(std::vector<RecoveryMutation> &mutations, const std::string &fileId, MutationKind kind,
                       RevisionId revision)
{
    for (auto &mutation : mutations) {
        if (mutation.kind == kind && mutation.target.id() == fileId && mutation.target.current().id == revision)
            mutation.completed = true;
    }
}

bool placeholder(const FileSession &file)
{
    return !file.snapshots() && file.current().id == 1 && file.current().draft == Draft{}
    && !file.publication().checkpoint();
}

// Repeats one file command on file. Mutation facts change no file.
Transition apply(RecoveryAction action, FileSession &file, const RecoveryRecordReader &record,
                 std::string_view previousText)
{
    const auto revision = file.current().id;
    switch (action) {
    case RecoveryAction::ReplaceDraft: return file.replayDraft(revision, record.revision(previousText));
    case RecoveryAction::Undo: return file.undo(revision);
    case RecoveryAction::Redo: return file.redo(revision);
    case RecoveryAction::DiscardToCheckpoint: return file.discardToCheckpoint(revision);
    case RecoveryAction::DiscardToStaged: return file.discardToStaged(revision);
    case RecoveryAction::MarkResolvedReview: return file.beginMarkResolved(revision, record.reviewConfirmedUndecided());
    case RecoveryAction::ManualSave: {
        const auto outcome = record.checkpointOutcome();
        return file.recordCheckpoint(outcome.revision, outcome.outcome, outcome.detail);
    }
    case RecoveryAction::WriteOutcome: {
        const auto outcome = record.writeOutcome();
        return file.recordWrite(outcome.revision, outcome.outcome, outcome.detail);
    }
    case RecoveryAction::StageOutcome: {
        const auto outcome = record.stageOutcome();
        auto [objectId, mode] =
            outcome.outcome == Outcome::Succeeded ? record.stagedObject() : std::pair<std::string, std::string>{};
        return file.recordStage(outcome.revision, outcome.outcome, std::move(objectId), std::move(mode),
                                outcome.detail);
    }
    case RecoveryAction::LoadLarge: return file.loadLargeFile();
    case RecoveryAction::MutationIntent:
    case RecoveryAction::MutationObservation: return Transition::Applied;
    case RecoveryAction::Open: break;
    }
    return Transition::InvalidOutcome;
}
struct ReplayState {
    const storage::RecoveryStore &store;
    const std::shared_ptr<const CorrespondenceAlgorithm> correspondence;
    RecoveryReplay &result;
    std::map<std::string, FileSession> files;
    std::set<QByteArray> references;

    ReplayState(const storage::RecoveryStore &source, std::shared_ptr<const CorrespondenceAlgorithm> algorithm,
                RecoveryReplay &output)
        : store(source), correspondence(std::move(algorithm)), result(output)
    {}
    void referenced(const storage::RecoveryTransaction &transaction)
    {
        for (auto it = transaction.payloads.cbegin(); it != transaction.payloads.cend(); ++it)
            references.insert(it.key());
    }
    void checkpoint(const storage::StoredTransaction &stored, bool first)
    {
        requireRecord(files.empty() && result.mutations.empty() && first
                          && stored.state.value("checkpointSchema") == QJsonValue(2)
                          && stored.state.size() == (stored.state.contains("receipts") ? 5 : 4)
                          && stored.state.value("files").isArray() && stored.state.value("mutations").isArray()
                          && stored.state.value("edited").isArray(),
                      "Invalid recovery checkpoint envelope.");
        qint64 retained = 0;
        const auto decode = [&](const QJsonValue &value) {
            const auto object = value.toObject();
            bool ok = false;
            const auto bytes = object.value("retained").toString().toLongLong(&ok);
            requireRecord(ok && bytes >= 0 && bytes <= stored.retainedBytes - retained
                              && QString::number(bytes) == object.value("retained").toString(),
                          "Invalid checkpoint retained size.");
            retained += bytes;
            const auto state = object.value("file").toObject();
            auto result = RecoveryRecordReader(store, state, bytes).checkpointFile(correspondence);
            const auto encoded = recoveryFileCheckpoint(result);
            requireRecord(encoded.retainedBytes == bytes, "Invalid checkpoint decoded accounting.");
            referenced(encoded);
            return result;
        };
        for (const auto &value : stored.state.value("files").toArray()) {
            auto file = decode(value);
            requireRecord(value.toObject().size() == 2 && files.emplace(file.id(), std::move(file)).second,
                          "Duplicate checkpoint file.");
        }
        for (const auto &value : stored.state.value("mutations").toArray()) {
            const auto object = value.toObject();
            auto target = decode(value);
            const auto id = object.value("id").toString();
            const auto kind = valueNamed(mutationKindNames, object.value("kind").toString());
            if (!kind) throw RecoveryRecordError("Invalid checkpoint mutation evidence.");
            const auto observation = object.value("observation");
            const auto observed = valueNamed(mutationObservationNames, observation.toString());
            requireRecord(!QUuid(id).isNull() && kind && (observation.isNull() || observed) && object.size() == 5
                              && files.contains(target.id()) && files.at(target.id()).path() == target.path()
                              && ((!files.at(target.id()).snapshots() && !target.snapshots())
                                  || (files.at(target.id()).snapshots() && target.snapshots()
                                      && *files.at(target.id()).snapshots() == *target.snapshots()))
                              && std::none_of(result.mutations.begin(), result.mutations.end(),
                                              [&](const auto &item) { return item.id == id; }),
                          "Invalid checkpoint mutation evidence.");
            result.mutations.push_back({id, *kind, observed, std::move(target), false});
        }
        for (const auto &value : stored.state.value("edited").toArray())
            requireRecord(value.isString() && files.contains(value.toString().toStdString())
                              && result.editedFiles.insert(value.toString().toStdString()).second,
                          "Invalid checkpoint edited file.");
        requireRecord(retained == stored.retainedBytes, "Checkpoint retained size mismatch.");
        result.userWork = !result.editedFiles.empty();
    }
    void transaction(const storage::StoredTransaction &stored)
    {
        const RecoveryRecordReader record(store, stored.state, stored.retainedBytes);
        const auto action = record.action();
        const auto id = record.fileId();
        const auto path = record.path();
        requireRecord(!id.empty() && !path.empty() && path.find('\0') == std::string::npos,
                      "Invalid recovery file identity.");
        const auto mutation = record.mutation();
        requireRecord(
            mutation.has_value()
                == (action == RecoveryAction::MutationIntent || action == RecoveryAction::MutationObservation),
            "Invalid recovery mutation record.");
        auto found = files.find(id);
        std::string previousText;
        if (action == RecoveryAction::Open) {
            requireRecord(found == files.end() || placeholder(found->second),
                          "Recovery attempted to replace an acknowledged session.");
            found = files
                        .insert_or_assign(id,
                                          FileSession(id, path, record.draft({}),
                                                      {.support = record.support(),
                                                       .supportDetail = record.supportDetail(),
                                                       .snapshots = record.snapshots(),
                                                       .algorithm = correspondence}))
                        .first;
            requireRecord(found->second.algorithm()->identity() == record.alignment(),
                          "Recovery journal was aligned with another correspondence algorithm.");
        } else {
            requireRecord(found != files.end() && found->second.path() == path,
                          "Recovery command has no matching open file.");
            // Loading a large file is not an edit; reopening it asks again.
            if (action != RecoveryAction::LoadLarge) {
                result.userWork = true;
                result.editedFiles.insert(id);
            }
            previousText = found->second.current().draft.text;
            if (action == RecoveryAction::MutationObservation) {
                if (!mutation) throw RecoveryRecordError("Invalid recovery mutation record.");
                // The observation repeats its intent's target, whatever the
                // file became since.
                const auto entry = std::find_if(result.mutations.begin(), result.mutations.end(),
                                                [&](const auto &candidate) { return candidate.id == mutation->id; });
                requireRecord(entry != result.mutations.end() && entry->kind == mutation->kind && !entry->observation,
                              "Recovery mutation observation has no intent or repeats one.");
                const auto expected =
                    recoveryTransaction(entry->target, action, stored.id, entry->target.current().draft.text, mutation);
                requireRecord(expected.state == stored.state && expected.retainedBytes == stored.retainedBytes,
                              "Recovery mutation observation changed its original target.");
                referenced(expected);
                entry->observation = mutation->observation;
                return; // An old completion never replaces a newer draft or discard.
            }
            requireRecord(apply(action, found->second, record, previousText) == Transition::Applied,
                          "Recovery transition does not match the acknowledged record.");
            if (action == RecoveryAction::MutationIntent) {
                if (!mutation) throw RecoveryRecordError("Invalid recovery mutation record.");
                requireRecord(std::none_of(result.mutations.begin(), result.mutations.end(),
                                           [&](const auto &entry) { return entry.id == mutation->id; }),
                              "Duplicate recovery mutation intent.");
                result.mutations.push_back({mutation->id, mutation->kind, {}, found->second, false});
            }
        }
        const auto encoded = recoveryTransaction(found->second, action, stored.id, previousText, mutation);
        requireRecord(encoded.state == stored.state && encoded.retainedBytes == stored.retainedBytes,
                      "Recovery state schema or transition does not match the acknowledged record.");
        referenced(encoded);
        if (action == RecoveryAction::WriteOutcome || action == RecoveryAction::StageOutcome) {
            const auto kind =
                action == RecoveryAction::WriteOutcome ? MutationKind::Publication : MutationKind::Staging;
            const auto &publication = found->second.publication();
            const auto &outcome =
                action == RecoveryAction::WriteOutcome ? publication.writeOutcome() : publication.stageOutcome();
            // Match completion in journal order before a later revision can
            // replace the file's latest outcome, or a new intent can reuse it.
            if (outcome && outcome->outcome == Outcome::Succeeded)
                completeMutations(result.mutations, id, kind, outcome->revision);
        }
    }
};

}

storage::RecoveryTransaction mutationRecoveryTransaction(const FileSession &file, const MutationFact &fact)
{
    return recoveryTransaction(file,
                               fact.observation ? RecoveryAction::MutationObservation : RecoveryAction::MutationIntent,
                               QUuid::createUuid().toString(QUuid::WithoutBraces), file.current().draft.text, fact);
}

storage::RecoveryTransaction recoveryCheckpoint(const storage::RecoveryStore &store,
                                                std::shared_ptr<const CorrespondenceAlgorithm> correspondence)
{
    const auto replay = replayRecovery(store, correspondence);
    if (!replay.succeeded) throw RecoveryRecordError(replay.problem);
    storage::RecoveryTransaction result;
    result.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    result.continuation = [correspondence](const storage::RecoveryStore &source) {
        return recoveryCheckpoint(source, correspondence);
    };
    QJsonArray files, mutations, edited;
    const auto encode = [&](const FileSession &file) {
        auto checkpoint = recoveryFileCheckpoint(file);
        // Reject an unimportable encoding before any authoritative replacement.
        (void)RecoveryRecordReader(store, checkpoint.state, checkpoint.retainedBytes, &checkpoint.payloads)
            .checkpointFile(correspondence);
        if (checkpoint.retainedBytes > 256LL * 1024 * 1024 - result.retainedBytes)
            throw std::length_error("Live recovery checkpoint exceeds 256 MiB.");
        result.retainedBytes += checkpoint.retainedBytes;
        for (auto it = checkpoint.payloads.cbegin(); it != checkpoint.payloads.cend(); ++it)
            result.payloads.insert(it.key(), it.value());
        return QJsonObject{{"file", checkpoint.state}, {"retained", QString::number(checkpoint.retainedBytes)}};
    };
    for (const auto &file : replay.files) files.push_back(encode(file));
    for (const auto &mutation : replay.mutations) {
        if (mutation.completed) continue;
        auto value = encode(mutation.target);
        value.insert("id", mutation.id);
        value.insert("kind", nameOf(mutationKindNames, mutation.kind));
        value.insert("observation",
                     mutation.observation ? QJsonValue(nameOf(mutationObservationNames, *mutation.observation))
                                          : QJsonValue::Null);
        mutations.push_back(value);
    }
    for (const auto &id : replay.editedFiles) edited.push_back(QString::fromStdString(id));
    result.state = {{"checkpointSchema", 2}, {"files", files}, {"mutations", mutations}, {"edited", edited}};
    return result;
}

RecoveryReplay replayRecovery(const storage::RecoveryStore &store,
                              std::shared_ptr<const CorrespondenceAlgorithm> correspondence)
{
    RecoveryReplay result;
    const auto read = store.read();
    if (!read.succeeded) {
        result.problem = read.problem;
        result.newerFormat = read.newerFormat;
        return result;
    }
    result.incompleteTail = read.incompleteTail;
    ReplayState state(store, std::move(correspondence), result);
    try {
        for (const auto &stored : read.transactions) {
            if (stored.state.contains("checkpointSchema"))
                state.checkpoint(stored, &stored == &read.transactions.front());
            else
                state.transaction(stored);
        }
        requireRecord(state.references == read.payloads,
                      "Recovery payload declarations disagree with domain references.");
        std::optional<SnapshotProvenance> provenance;
        for (auto &[id, file] : state.files) {
            (void)id;
            if (const auto *snapshots = file.snapshots()) {
                requireRecord(!provenance || *provenance == snapshots->provenance,
                              "Recovery session identities disagree.");
                provenance = snapshots->provenance;
            }
            result.files.push_back(std::move(file));
        }
        result.succeeded = true;
    } catch (const RecoveryRecordError &error) {
        result.problem = error.problem.isEmpty()
            ? Problem{ProblemCode::RecoveryRecordInvalid, {QString::fromUtf8(error.what())}}
            : error.problem;
    } catch (const std::exception &error) {
        result.problem = {ProblemCode::RecoveryRecordInvalid, {QString::fromUtf8(error.what())}};
    }
    if (!result.succeeded) {
        result.files.clear();
        result.mutations.clear();
        result.editedFiles.clear();
    }
    return result;
}
}
