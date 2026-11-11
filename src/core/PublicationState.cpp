// Publication state records Save, Mark resolved, write and stage outcomes for a file's revisions.
#include "PublicationState.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace branch_truce {
namespace {
bool succeeded(const std::optional<OperationOutcome> &outcome, RevisionId revision)
{
    return outcome && outcome->revision == revision && outcome->outcome == Outcome::Succeeded;
}

void addFailure(std::vector<Blocker> &blockers, const std::optional<OperationOutcome> &outcome, Blocker failure)
{
    if (!outcome) return;
    if (outcome->outcome == Outcome::Failed) blockers.push_back(failure);
    if (outcome->outcome == Outcome::Uncertain
        && std::find(blockers.begin(), blockers.end(), Blocker::UncertainOutcome) == blockers.end())
        blockers.push_back(Blocker::UncertainOutcome);
}

std::optional<FileStatus> supportStatus(Support support) noexcept
{
    switch (support) {
    case Support::OutsideApp: return FileStatus::OutsideApp;
    case Support::Unreadable: return FileStatus::Unreadable;
    case Support::LargeFile: return FileStatus::LargeFile;
    case Support::Supported: break;
    }
    return std::nullopt;
}

std::optional<Blocker> supportBlocker(Support support) noexcept
{
    switch (support) {
    case Support::OutsideApp: return Blocker::OutsideApp;
    case Support::Unreadable: return Blocker::Unreadable;
    case Support::LargeFile: return Blocker::LargeFile;
    case Support::Supported: break;
    }
    return std::nullopt;
}

// Recorded failures, most urgent first, ahead of the current revision's progress.
constexpr std::array failureStatuses{std::pair{Blocker::UncertainOutcome, FileStatus::OutcomeUncertain},
                                     std::pair{Blocker::CheckpointFailed, FileStatus::CheckpointFailed},
                                     std::pair{Blocker::WriteFailed, FileStatus::WriteFailed},
                                     std::pair{Blocker::StageFailed, FileStatus::StageFailed}};
}

PublicationState PublicationState::restore(PublicationCheckpoint checkpoint, const RevisionHistory &history)
{
    PublicationState restored;
    restored.state_ = std::move(checkpoint);
    const auto &state = restored.state_;
    const auto validId = [&](RevisionId id) { return id >= history.launchId() && id < history.nextId(); };
    bool valid = validId(history.launchId()) && state.floor >= history.launchId() && state.floor <= history.nextId();
    for (auto id : restored.held()) valid = valid && validId(id) && history.find(id);
    for (const auto *outcome : {&state.checkpointOutcome, &state.writeOutcome, &state.stageOutcome})
        if (*outcome)
            valid = valid && validId((*outcome)->revision)
                && ((*outcome)->outcome == Outcome::Succeeded || (*outcome)->outcome == Outcome::Failed
                    || (*outcome)->outcome == Outcome::Uncertain);
    valid = valid
        && (!state.reviewSaved || (state.reviewed && state.checkpoint && *state.checkpoint >= state.reviewed->revision))
        && (state.checkpoint.has_value() == state.checkpointOutcome.has_value()
            || (!state.checkpoint && state.checkpointOutcome && state.checkpointOutcome->outcome != Outcome::Succeeded))
        && (!state.checkpointOutcome || state.checkpointOutcome->outcome != Outcome::Succeeded
            || state.checkpoint == state.checkpointOutcome->revision)
        && (!state.written || state.writeOutcome) && (!state.writeOutcome || (state.reviewed && state.checkpoint))
        && (!state.writeOutcome || state.writeOutcome->outcome != Outcome::Succeeded
            || state.written == state.writeOutcome->revision)
        && (!state.staged || (state.stageOutcome && !state.staged->objectId.empty() && !state.staged->mode.empty()))
        && (!state.stageOutcome || state.written)
        && (!state.stageOutcome || state.stageOutcome->outcome != Outcome::Succeeded
            || (state.staged && state.staged->revision == state.stageOutcome->revision));
    if (!valid) throw std::invalid_argument("Invalid publication checkpoint");
    return restored;
}

bool PublicationState::untouched() const noexcept
{
    return !state_.checkpoint && !state_.reviewed && !state_.written && !state_.staged && !state_.checkpointOutcome
        && !state_.writeOutcome && !state_.stageOutcome;
}

bool PublicationState::checkpointed(RevisionId revision) const noexcept
{
    return state_.checkpoint == revision && succeeded(state_.checkpointOutcome, revision);
}

bool PublicationState::writtenAt(RevisionId revision) const noexcept
{
    return state_.written == revision && succeeded(state_.writeOutcome, revision);
}

std::vector<RevisionId> PublicationState::held() const
{
    std::vector<RevisionId> result;
    for (const auto &id :
         {state_.checkpoint, state_.written, state_.reviewed ? std::optional{state_.reviewed->revision} : std::nullopt,
          state_.staged ? std::optional{state_.staged->revision} : std::nullopt})
        if (id) result.push_back(*id);
    return result;
}

bool PublicationState::savedAt(RevisionId current) const noexcept
{
    return !state_.checkpointOutcome || succeeded(state_.checkpointOutcome, current);
}

bool PublicationState::stagedVerified() const noexcept
{
    return state_.staged && writtenAt(state_.staged->revision)
        && succeeded(state_.stageOutcome, state_.staged->revision);
}

void PublicationState::markResolved(RevisionId current, bool confirmedUndecided)
{
    state_.reviewed = Review{current, confirmedUndecided};
    state_.reviewSaved = checkpointed(current);
}

Transition PublicationState::recordCheckpoint(bool stored, RevisionId revision, RevisionId current, Outcome outcome,
                                              std::string detail)
{
    if (!stored || fenced(revision, current)
        || (state_.checkpointOutcome && revision < state_.checkpointOutcome->revision))
        return Transition::StaleRevision;
    state_.checkpointOutcome = OperationOutcome{revision, outcome, std::move(detail)};
    if (outcome == Outcome::Succeeded) state_.checkpoint = revision;
    if (state_.reviewed && state_.reviewed->revision == revision) state_.reviewSaved = outcome == Outcome::Succeeded;
    return Transition::Applied;
}

Transition PublicationState::recordWrite(RevisionId revision, RevisionId current, Outcome outcome, std::string detail)
{
    if (!state_.reviewed || state_.reviewed->revision != revision || fenced(revision, current))
        return Transition::StaleRevision;
    if (!state_.reviewSaved) return Transition::InvalidOutcome;
    state_.writeOutcome = OperationOutcome{revision, outcome, std::move(detail)};
    if (outcome == Outcome::Succeeded) state_.written = state_.reviewed->revision;
    return Transition::Applied;
}

Transition PublicationState::recordStage(RevisionId revision, RevisionId current, Outcome outcome, std::string objectId,
                                         std::string mode, std::string detail)
{
    if (!state_.reviewed || state_.reviewed->revision != revision || fenced(revision, current))
        return Transition::StaleRevision;
    if (!writtenAt(revision)) return Transition::InvalidOutcome;
    if (outcome == Outcome::Succeeded && (objectId.empty() || mode.empty())) return Transition::InvalidOutcome;
    state_.stageOutcome = OperationOutcome{revision, outcome, std::move(detail)};
    if (outcome == Outcome::Succeeded)
        state_.staged = StagedResult{state_.reviewed->revision, std::move(objectId), std::move(mode),
                                     state_.reviewed->confirmedUndecided};
    return Transition::Applied;
}

void PublicationState::discardedToCheckpoint(RevisionId current)
{
    state_.floor = current;
    state_.checkpointOutcome = state_.checkpoint
        ? std::optional<OperationOutcome>{{*state_.checkpoint, Outcome::Succeeded, {}}}
        : std::nullopt;
}

void PublicationState::discardedToStaged(RevisionId floor)
{
    // Selecting this previously saved, verified state deliberately discards
    // the newer draft and checkpoint. No external state changes or new review occur.
    if (!state_.staged) return;
    const auto revision = state_.staged->revision;
    state_.floor = floor;
    state_.checkpoint = revision;
    state_.checkpointOutcome = OperationOutcome{revision, Outcome::Succeeded, {}};
    state_.reviewed = Review{revision, state_.staged->confirmedUndecided};
    state_.reviewSaved = true;
}

FileFacts PublicationState::facts(const Revision &current, const Revision *checkpoint, Support support,
                                  std::size_t undecided) const
{
    const bool reviewed = state_.reviewed && state_.reviewed->revision == current.id;
    const bool saved = checkpoint && checkpoint->result == current.result && checkpoint->decisions == current.decisions;
    const bool written = writtenAt(current.id);
    const bool staged =
        state_.staged && state_.staged->revision == current.id && succeeded(state_.stageOutcome, current.id);
    const bool unresolved = current.draft.unresolvedMarkers != 0;
    FileFacts facts{.undecided = undecided,
                    .unsaved = !saved,
                    .needsReview = !reviewed,
                    .earlierStaged = state_.staged && state_.staged->revision != current.id,
                    .resolved = support == Support::Supported && reviewed && saved && written && staged
                        && succeeded(state_.checkpointOutcome, current.id) && !unresolved,
                    .status = FileStatus::ReviewNeeded,
                    .blockers = {},
                    .settled = false};
    facts.blockers = blockers(current, support, facts, written, staged);
    // Failures remain outstanding even if an earlier matching snapshot exists.
    facts.resolved = facts.resolved && facts.blockers.empty();
    facts.status = status(facts, support, saved);
    facts.settled = support == Support::Supported && undecided == 0 && !unresolved
        && (facts.status == FileStatus::ReviewNeeded || facts.status == FileStatus::SavedDraft);
    return facts;
}

std::vector<Blocker> PublicationState::blockers(const Revision &current, Support support, const FileFacts &facts,
                                                bool written, bool staged) const
{
    std::vector<Blocker> result;
    // A file the app cannot edit has no draft to Save, mark resolved, write
    // or stage, so only its support blocker and recorded failures apply. It
    // stays unresolved because resolution requires Support::Supported.
    if (const auto blocker = supportBlocker(support)) {
        result.push_back(*blocker);
    } else {
        const bool confirmed =
            state_.reviewed && state_.reviewed->revision == current.id && state_.reviewed->confirmedUndecided;
        const std::array<std::pair<bool, Blocker>, 7> progress{
            {{current.draft.unresolvedMarkers != 0, Blocker::UnresolvedMarkers},
             {facts.undecided != 0 && !confirmed, Blocker::Undecided},
             {facts.unsaved, Blocker::UnsavedDraft},
             {facts.needsReview, Blocker::ReviewNeeded},
             {!written, Blocker::WriteRequired},
             {!staged, Blocker::StageRequired},
             {facts.earlierStaged, Blocker::EarlierStagedResult}}};
        for (const auto &[applies, blocker] : progress)
            if (applies) result.push_back(blocker);
    }
    addFailure(result, state_.checkpointOutcome, Blocker::CheckpointFailed);
    addFailure(result, state_.writeOutcome, Blocker::WriteFailed);
    addFailure(result, state_.stageOutcome, Blocker::StageFailed);
    return result;
}

FileStatus PublicationState::status(const FileFacts &facts, Support support, bool checkpoint) const
{
    if (const auto status = supportStatus(support)) return *status;
    for (const auto &[blocker, status] : failureStatuses)
        if (std::find(facts.blockers.begin(), facts.blockers.end(), blocker) != facts.blockers.end()) return status;
    if (facts.earlierStaged) return FileStatus::EarlierStaged;
    if (facts.resolved) return FileStatus::Resolved;
    if (state_.written && writtenAt(*state_.written)) return FileStatus::WrittenUnresolved;
    if (checkpoint) return FileStatus::SavedDraft;
    if (facts.undecided != 0) return FileStatus::Undecided;
    return FileStatus::ReviewNeeded;
}

}
