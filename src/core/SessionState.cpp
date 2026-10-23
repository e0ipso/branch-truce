#include "SessionState.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace branch_truce {
namespace {
bool succeeded(const std::optional<OperationOutcome> &outcome, RevisionId revision)
{
    return outcome && outcome->revision == revision && outcome->outcome == Outcome::Succeeded;
}

std::size_t undecidedCount(const Draft &draft)
{
    return static_cast<std::size_t>(std::count_if(
        draft.decisions.begin(), draft.decisions.end(),
        [](const auto &entry) { return entry.second.value == DecisionValue::Undecided; }));
}
}

FileSession::FileSession(std::string id, std::string path, Draft initial,
                         Support support, std::string supportDetail)
    : id_(std::move(id)), path_(std::move(path)), support_(support),
      supportDetail_(std::move(supportDetail))
{
    if (id_.empty() || path_.empty()) {
        throw std::invalid_argument("A file session needs an identity and path");
    }
    revisions_.emplace(1, Revision{1, 1, 1, std::move(initial)});
}

RevisionId FileSession::advance(const Draft &draft)
{
    if (next_ == std::numeric_limits<RevisionId>::max()) {
        throw std::overflow_error("Session revision space exhausted");
    }
    const auto &previous = current();
    const auto id = next_++;
    const auto result = draft.text == previous.draft.text ? previous.result : id;
    const auto decisions = draft.decisions == previous.draft.decisions
            && draft.unresolvedMarkers == previous.draft.unresolvedMarkers
        ? previous.decisions : id;
    revisions_.emplace(id, Revision{id, result, decisions, draft});
    current_ = id;
    return id;
}

RevisionId FileSession::restore(const Revision &revision)
{
    const auto id = advance(revision.draft);
    // Restored content can still be the manually saved content. The transaction
    // identity is fresh, so restoring it never restores review.
    auto &restored = revisions_.at(id);
    restored.result = revision.result;
    restored.decisions = revision.decisions;
    return id;
}

void FileSession::appendHistory(RevisionId revision)
{
    history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(cursor_ + 1), history_.end());
    history_.push_back(revision);
    cursor_ = history_.size() - 1;
}

Transition FileSession::replaceDraft(RevisionId expected, Draft draft)
{
    if (expected != current_) return Transition::StaleRevision;
    if (support_ != Support::Supported) return Transition::Blocked;
    if (draft == current().draft) return Transition::NoChange;
    appendHistory(advance(draft));
    return Transition::Applied;
}

Transition FileSession::undo(RevisionId expected)
{
    if (expected != current_) return Transition::StaleRevision;
    if (support_ != Support::Supported) return Transition::Blocked;
    if (!canUndo()) return Transition::NoChange;
    restore(revisions_.at(history_[cursor_ - 1]));
    --cursor_;
    return Transition::Applied;
}

Transition FileSession::redo(RevisionId expected)
{
    if (expected != current_) return Transition::StaleRevision;
    if (support_ != Support::Supported) return Transition::Blocked;
    if (!canRedo()) return Transition::NoChange;
    restore(revisions_.at(history_[cursor_ + 1]));
    ++cursor_;
    return Transition::Applied;
}

Transition FileSession::discardToCheckpoint(RevisionId expected)
{
    if (expected != current_) return Transition::StaleRevision;
    if (support_ != Support::Supported) return Transition::Blocked;
    const auto &target = checkpoint_ ? *checkpoint_ : revisions_.at(1);
    // Matching text after undo can still have pending Save callbacks from a
    // discarded revision. Only the checkpoint's own successful transaction is
    // a no-op; otherwise restore its identity and fence those callbacks below.
    if (target.id == current_
        && (!checkpointOutcome_ || succeeded(checkpointOutcome_, target.id)))
        return Transition::NoChange;
    appendHistory(restore(target));
    outcomeFloor_ = current_;
    checkpointOutcome_ = checkpoint_
        ? std::optional<OperationOutcome>{{checkpoint_->id, Outcome::Succeeded, {}}}
        : std::nullopt;
    return Transition::Applied;
}

Transition FileSession::beginMarkResolved(RevisionId expected, bool confirmUndecided)
{
    if (expected != current_) return Transition::StaleRevision;
    if (support_ != Support::Supported || current().draft.unresolvedMarkers != 0)
        return Transition::Blocked;
    if (undecidedCount(current().draft) != 0 && !confirmUndecided)
        return Transition::ConfirmationRequired;
    reviewed_ = Review{current(), confirmUndecided};
    reviewCheckpointSucceeded_ = checkpoint_ && checkpoint_->id == expected
        && succeeded(checkpointOutcome_, expected);
    return Transition::Applied;
}

Transition FileSession::recordCheckpoint(RevisionId revision, Outcome outcome, std::string detail)
{
    const auto found = revisions_.find(revision);
    if (found == revisions_.end()
        || (revision < outcomeFloor_ && revision != current_)
        || (checkpointOutcome_ && revision < checkpointOutcome_->revision))
        return Transition::StaleRevision;
    checkpointOutcome_ = OperationOutcome{revision, outcome, std::move(detail)};
    if (outcome == Outcome::Succeeded) checkpoint_ = found->second;
    if (reviewed_ && reviewed_->revision.id == revision)
        reviewCheckpointSucceeded_ = outcome == Outcome::Succeeded;
    return Transition::Applied;
}

Transition FileSession::recordWrite(RevisionId revision, Outcome outcome, std::string detail)
{
    if (!reviewed_ || reviewed_->revision.id != revision
        || (revision < outcomeFloor_ && revision != current_)) return Transition::StaleRevision;
    if (!reviewCheckpointSucceeded_) return Transition::InvalidOutcome;
    writeOutcome_ = OperationOutcome{revision, outcome, std::move(detail)};
    if (outcome == Outcome::Succeeded) written_ = reviewed_->revision;
    return Transition::Applied;
}

Transition FileSession::recordStage(RevisionId revision, Outcome outcome, std::string objectId,
                                    std::string mode, std::string detail)
{
    if (!reviewed_ || reviewed_->revision.id != revision
        || (revision < outcomeFloor_ && revision != current_)) return Transition::StaleRevision;
    if (!written_ || written_->id != revision || !succeeded(writeOutcome_, revision))
        return Transition::InvalidOutcome;
    if (outcome == Outcome::Succeeded && (objectId.empty() || mode.empty()))
        return Transition::InvalidOutcome;
    stageOutcome_ = OperationOutcome{revision, outcome, std::move(detail)};
    if (outcome == Outcome::Succeeded) {
        staged_ = StagedResult{reviewed_->revision, std::move(objectId), std::move(mode),
                               reviewed_->confirmedUndecided};
    }
    return Transition::Applied;
}

Transition FileSession::discardToStaged(RevisionId expected)
{
    if (expected != current_) return Transition::StaleRevision;
    if (support_ != Support::Supported || !staged_ || !written_
        || written_->id != staged_->revision.id
        || !succeeded(writeOutcome_, staged_->revision.id)
        || !succeeded(stageOutcome_, staged_->revision.id))
        return Transition::Blocked;
    if (current_ == staged_->revision.id) return Transition::NoChange;
    // Selecting this previously saved, verified state deliberately discards the
    // newer draft/checkpoint. No external state changes or new review occur.
    current_ = staged_->revision.id;
    outcomeFloor_ = next_;
    checkpoint_ = staged_->revision;
    checkpointOutcome_ = OperationOutcome{current_, Outcome::Succeeded, {}};
    reviewed_ = Review{staged_->revision, staged_->confirmedUndecided};
    reviewCheckpointSucceeded_ = true;
    appendHistory(current_);
    return Transition::Applied;
}

Transition FileSession::setSupport(Support support, std::string detail)
{
    if (support_ == support && supportDetail_ == detail) return Transition::NoChange;
    support_ = support;
    supportDetail_ = std::move(detail);
    return Transition::Applied;
}

FileFacts FileSession::facts() const
{
    const auto undecided = undecidedCount(current().draft);
    const bool reviewed = reviewed_ && reviewed_->revision.id == current_;
    const bool checkpoint = checkpoint_
        && checkpoint_->result == current().result
        && checkpoint_->decisions == current().decisions;
    const bool written = written_ && written_->id == current_ && succeeded(writeOutcome_, current_);
    const bool staged = staged_ && staged_->revision.id == current_ && succeeded(stageOutcome_, current_);
    const bool resolved = support_ == Support::Supported && reviewed && checkpoint && written
        && staged && succeeded(checkpointOutcome_, current_) && current().draft.unresolvedMarkers == 0;
    FileFacts facts{undecided, !checkpoint, !reviewed, staged_ && staged_->revision.id != current_,
                    resolved, FileStatus::ReviewNeeded, {}};
    auto &blockers = facts.blockers;
    if (support_ == Support::OutsideApp) blockers.push_back(Blocker::OutsideApp);
    if (support_ == Support::Unreadable) blockers.push_back(Blocker::Unreadable);
    if (current().draft.unresolvedMarkers != 0) blockers.push_back(Blocker::UnresolvedMarkers);
    if (undecided != 0 && !(reviewed && reviewed_->confirmedUndecided))
        blockers.push_back(Blocker::Undecided);
    if (facts.unsaved) blockers.push_back(Blocker::UnsavedDraft);
    if (facts.needsReview) blockers.push_back(Blocker::ReviewNeeded);
    if (!written) blockers.push_back(Blocker::WriteRequired);
    if (!staged) blockers.push_back(Blocker::StageRequired);
    if (facts.earlierStaged) blockers.push_back(Blocker::EarlierStagedResult);
    const auto addFailure = [&](const auto &outcome, Blocker failure) {
        if (!outcome) return;
        if (outcome->outcome == Outcome::Failed) blockers.push_back(failure);
        if (outcome->outcome == Outcome::Uncertain
            && std::find(blockers.begin(), blockers.end(), Blocker::UncertainOutcome) == blockers.end())
            blockers.push_back(Blocker::UncertainOutcome);
    };
    addFailure(checkpointOutcome_, Blocker::CheckpointFailed);
    addFailure(writeOutcome_, Blocker::WriteFailed);
    addFailure(stageOutcome_, Blocker::StageFailed);
    // Failures remain outstanding even if an earlier matching snapshot exists.
    facts.resolved = facts.resolved && blockers.empty();
    const auto has = [&](Blocker blocker) {
        return std::find(blockers.begin(), blockers.end(), blocker) != blockers.end();
    };
    if (support_ == Support::OutsideApp) facts.status = FileStatus::OutsideApp;
    else if (support_ == Support::Unreadable) facts.status = FileStatus::Unreadable;
    else if (has(Blocker::UncertainOutcome)) facts.status = FileStatus::OutcomeUncertain;
    else if (has(Blocker::CheckpointFailed)) facts.status = FileStatus::CheckpointFailed;
    else if (has(Blocker::WriteFailed)) facts.status = FileStatus::WriteFailed;
    else if (has(Blocker::StageFailed)) facts.status = FileStatus::StageFailed;
    else if (facts.earlierStaged) facts.status = FileStatus::EarlierStaged;
    else if (facts.resolved) facts.status = FileStatus::Resolved;
    else if (written_ && succeeded(writeOutcome_, written_->id))
        facts.status = FileStatus::WrittenUnresolved;
    else if (checkpoint) facts.status = FileStatus::SavedDraft;
    else if (undecided != 0) facts.status = FileStatus::Undecided;
    return facts;
}

const char *statusText(FileStatus status) noexcept
{
    switch (status) {
    case FileStatus::OutsideApp: return "Needs work outside Branch Truce";
    case FileStatus::Unreadable: return "File could not be read";
    case FileStatus::OutcomeUncertain: return "Outcome uncertain; verification required";
    case FileStatus::CheckpointFailed: return "Draft save failed";
    case FileStatus::WriteFailed: return "Repository write failed";
    case FileStatus::StageFailed: return "Written, unresolved";
    case FileStatus::EarlierStaged: return "Earlier result staged; current draft not applied";
    case FileStatus::Resolved: return "Written and resolved";
    case FileStatus::WrittenUnresolved: return "Written, unresolved";
    case FileStatus::SavedDraft: return "Draft saved in Branch Truce; repository file unchanged";
    case FileStatus::Undecided: return "Source choices remain undecided";
    case FileStatus::ReviewNeeded: return "Current result needs review";
    }
    return "";
}

const char *blockerText(Blocker blocker) noexcept
{
    switch (blocker) {
    case Blocker::OutsideApp: return "Handle this file outside Branch Truce";
    case Blocker::Unreadable: return "File content unavailable";
    case Blocker::UnresolvedMarkers: return "Known conflict markers remain";
    case Blocker::Undecided: return "Undecided source choices need confirmation";
    case Blocker::UnsavedDraft: return "Current draft has no manual Save checkpoint";
    case Blocker::ReviewNeeded: return "Current result needs Mark resolved";
    case Blocker::WriteRequired: return "Current result has not been verified written";
    case Blocker::StageRequired: return "Current result has not been verified staged";
    case Blocker::EarlierStagedResult: return "Earlier result staged; current draft not applied";
    case Blocker::CheckpointFailed: return "Draft save failed; retry or discard explicitly";
    case Blocker::WriteFailed: return "Repository write failed";
    case Blocker::StageFailed: return "Git resolution failed";
    case Blocker::UncertainOutcome: return "Inspect uncertain mutation before finishing";
    }
    return "";
}
}
