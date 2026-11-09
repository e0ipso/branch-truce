// Recovery replay rebuilds file sessions from a journal by repeating each recorded domain transition.
#include "recovery/RecoveryReplay.h"
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
    return !file.snapshots() && file.current().id == 1 && file.current().draft == Draft{} && !file.checkpoint();
}

// Repeats one file command on file. Mutation facts change no file.
Transition apply(RecoveryAction action, FileSession &file, const RecoveryRecordReader &record,
                 std::string_view previousText)
{
    const auto revision = file.current().id;
    switch (action) {
    case RecoveryAction::ReplaceDraft:
        return file.replaceDraft(revision, record.draft(previousText), record.handEdited(), record.handEditedRanges());
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
    case RecoveryAction::Support: return file.setSupport(record.support(), record.supportDetail());
    case RecoveryAction::LoadLarge: return file.loadLargeFile();
    case RecoveryAction::MutationIntent:
    case RecoveryAction::MutationObservation: return Transition::Applied;
    case RecoveryAction::Open: break;
    }
    return Transition::InvalidOutcome;
}
}

storage::RecoveryTransaction mutationRecoveryTransaction(const FileSession &file, const MutationFact &fact)
{
    return recoveryTransaction(file,
                               fact.observation ? RecoveryAction::MutationObservation : RecoveryAction::MutationIntent,
                               QUuid::createUuid().toString(QUuid::WithoutBraces), file.current().draft.text, fact);
}

RecoveryReplay replayRecovery(const storage::RecoveryStore &store,
                              std::shared_ptr<const CorrespondenceAlgorithm> correspondence)
{
    RecoveryReplay result;
    const auto read = store.read();
    if (!read.succeeded) {
        result.error = read.error;
        result.newerFormat = read.newerFormat;
        return result;
    }
    result.incompleteTail = read.incompleteTail;
    std::map<std::string, FileSession> files;
    try {
        for (const auto &stored : read.transactions) {
            const RecoveryRecordReader record(store, stored.state);
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
                                              FileSession(id, path, record.draft({}), record.support(),
                                                          record.supportDetail(), record.snapshots(), false,
                                                          correspondence))
                            .first;
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
                    // The observation repeats its intent's target, whatever the
                    // file became since.
                    const auto entry =
                        std::find_if(result.mutations.begin(), result.mutations.end(),
                                     [&](const auto &candidate) { return candidate.id == mutation->id; });
                    requireRecord(entry != result.mutations.end() && entry->kind == mutation->kind
                                      && !entry->observation,
                                  "Recovery mutation observation has no intent or repeats one.");
                    requireRecord(recoveryTransaction(entry->target, action, stored.id,
                                                      entry->target.current().draft.text, mutation)
                                          .state
                                      == stored.state,
                                  "Recovery mutation observation changed its original target.");
                    entry->observation = mutation->observation;
                    continue; // An old completion never replaces a newer draft or discard.
                }
                requireRecord(apply(action, found->second, record, previousText) == Transition::Applied,
                              "Unsupported or inconsistent recovery transition.");
                if (action == RecoveryAction::MutationIntent) {
                    requireRecord(std::none_of(result.mutations.begin(), result.mutations.end(),
                                               [&](const auto &entry) { return entry.id == mutation->id; }),
                                  "Duplicate recovery mutation intent.");
                    result.mutations.push_back({mutation->id, mutation->kind, {}, found->second});
                }
            }
            const auto encoded = recoveryTransaction(found->second, action, stored.id, previousText, mutation).state;
            requireRecord(encoded == stored.state,
                          "Recovery state schema or transition does not match the acknowledged record.");
            if (action == RecoveryAction::WriteOutcome || action == RecoveryAction::StageOutcome) {
                const auto kind =
                    action == RecoveryAction::WriteOutcome ? MutationKind::Publication : MutationKind::Staging;
                const auto &outcome = action == RecoveryAction::WriteOutcome ? found->second.writeOutcome()
                                                                             : found->second.stageOutcome();
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
                requireRecord(!provenance || *provenance == snapshots->provenance,
                              "Recovery session identities disagree.");
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
}
