// Publication state records Save, Mark resolved, write and stage outcomes for a file's revisions.
#include "PublicationState.h"

#include <algorithm>
#include <array>
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

bool PublicationState::untouched() const noexcept
{
    return !checkpoint_ && !reviewed_ && !written_ && !staged_ && !checkpointOutcome_ && !writeOutcome_
        && !stageOutcome_;
}

bool PublicationState::savedAt(RevisionId current) const noexcept
{
    return !checkpointOutcome_ || succeeded(checkpointOutcome_, current);
}

bool PublicationState::stagedVerified() const noexcept
{
    return staged_ && written_ && written_->id == staged_->revision.id && succeeded(writeOutcome_, staged_->revision.id)
        && succeeded(stageOutcome_, staged_->revision.id);
}

void PublicationState::markResolved(const Revision &current, bool confirmedUndecided)
{
    reviewed_ = Review{current, confirmedUndecided};
    reviewCheckpointSucceeded_ =
        checkpoint_ && checkpoint_->id == current.id && succeeded(checkpointOutcome_, current.id);
}

Transition PublicationState::recordCheckpoint(const Revision *stored, RevisionId revision, RevisionId current,
                                              Outcome outcome, std::string detail)
{
    if (!stored || fenced(revision, current) || (checkpointOutcome_ && revision < checkpointOutcome_->revision))
        return Transition::StaleRevision;
    checkpointOutcome_ = OperationOutcome{revision, outcome, std::move(detail)};
    if (outcome == Outcome::Succeeded) checkpoint_ = *stored;
    if (reviewed_ && reviewed_->revision.id == revision) reviewCheckpointSucceeded_ = outcome == Outcome::Succeeded;
    return Transition::Applied;
}

Transition PublicationState::recordWrite(RevisionId revision, RevisionId current, Outcome outcome, std::string detail)
{
    if (!reviewed_ || reviewed_->revision.id != revision || fenced(revision, current)) return Transition::StaleRevision;
    if (!reviewCheckpointSucceeded_) return Transition::InvalidOutcome;
    writeOutcome_ = OperationOutcome{revision, outcome, std::move(detail)};
    if (outcome == Outcome::Succeeded) written_ = reviewed_->revision;
    return Transition::Applied;
}

Transition PublicationState::recordStage(RevisionId revision, RevisionId current, Outcome outcome, std::string objectId,
                                         std::string mode, std::string detail)
{
    if (!reviewed_ || reviewed_->revision.id != revision || fenced(revision, current)) return Transition::StaleRevision;
    if (!written_ || written_->id != revision || !succeeded(writeOutcome_, revision)) return Transition::InvalidOutcome;
    if (outcome == Outcome::Succeeded && (objectId.empty() || mode.empty())) return Transition::InvalidOutcome;
    stageOutcome_ = OperationOutcome{revision, outcome, std::move(detail)};
    if (outcome == Outcome::Succeeded)
        staged_ =
            StagedResult{reviewed_->revision, std::move(objectId), std::move(mode), reviewed_->confirmedUndecided};
    return Transition::Applied;
}

void PublicationState::discardedToCheckpoint(RevisionId current)
{
    outcomeFloor_ = current;
    checkpointOutcome_ =
        checkpoint_ ? std::optional<OperationOutcome>{{checkpoint_->id, Outcome::Succeeded, {}}} : std::nullopt;
}

void PublicationState::discardedToStaged(RevisionId floor)
{
    // Selecting this previously saved, verified state deliberately discards
    // the newer draft and checkpoint. No external state changes or new review occur.
    if (!staged_) return;
    const auto &revision = staged_->revision;
    outcomeFloor_ = floor;
    checkpoint_ = revision;
    checkpointOutcome_ = OperationOutcome{revision.id, Outcome::Succeeded, {}};
    reviewed_ = Review{revision, staged_->confirmedUndecided};
    reviewCheckpointSucceeded_ = true;
}

FileFacts PublicationState::facts(const Revision &current, Support support, std::size_t undecided) const
{
    const bool reviewed = reviewed_ && reviewed_->revision.id == current.id;
    const bool checkpoint =
        checkpoint_ && checkpoint_->result == current.result && checkpoint_->decisions == current.decisions;
    const bool written = written_ && written_->id == current.id && succeeded(writeOutcome_, current.id);
    const bool staged = staged_ && staged_->revision.id == current.id && succeeded(stageOutcome_, current.id);
    const bool unresolved = current.draft.unresolvedMarkers != 0;
    FileFacts facts{.undecided = undecided,
                    .unsaved = !checkpoint,
                    .needsReview = !reviewed,
                    .earlierStaged = staged_ && staged_->revision.id != current.id,
                    .resolved = support == Support::Supported && reviewed && checkpoint && written && staged
                        && succeeded(checkpointOutcome_, current.id) && !unresolved,
                    .status = FileStatus::ReviewNeeded,
                    .blockers = {},
                    .settled = false};
    facts.blockers = blockers(current, support, facts, written, staged);
    // Failures remain outstanding even if an earlier matching snapshot exists.
    facts.resolved = facts.resolved && facts.blockers.empty();
    facts.status = status(facts, support, checkpoint);
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
        const bool confirmed = reviewed_ && reviewed_->revision.id == current.id && reviewed_->confirmedUndecided;
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
    addFailure(result, checkpointOutcome_, Blocker::CheckpointFailed);
    addFailure(result, writeOutcome_, Blocker::WriteFailed);
    addFailure(result, stageOutcome_, Blocker::StageFailed);
    return result;
}

FileStatus PublicationState::status(const FileFacts &facts, Support support, bool checkpoint) const
{
    if (const auto status = supportStatus(support)) return *status;
    for (const auto &[blocker, status] : failureStatuses)
        if (std::find(facts.blockers.begin(), facts.blockers.end(), blocker) != facts.blockers.end()) return status;
    if (facts.earlierStaged) return FileStatus::EarlierStaged;
    if (facts.resolved) return FileStatus::Resolved;
    if (written_ && succeeded(writeOutcome_, written_->id)) return FileStatus::WrittenUnresolved;
    if (checkpoint) return FileStatus::SavedDraft;
    if (facts.undecided != 0) return FileStatus::Undecided;
    return FileStatus::ReviewNeeded;
}

}
