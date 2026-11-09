// The session store owns each file's history and the recovery journal that makes its edits durable.
#include "session/SessionStore.h"
#include "recovery/RecoveryRestore.h"
#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace branch_truce {
bool SessionStore::repositions(Action action)
{
    switch (action) {
    case Action::Open:
    case Action::StageOutcome:
    case Action::DiscardToStaged:
    case Action::DiscardToCheckpoint:
    case Action::Support: return true;
    case Action::ReplaceDraft:
    case Action::Undo:
    case Action::Redo:
    case Action::ManualSave:
    case Action::MarkResolvedReview:
    case Action::WriteOutcome:
    case Action::LoadLarge:
    case Action::MutationIntent:
    case Action::MutationObservation: return false;
    }
    return false;
}

SessionStore::SessionStore(QString recoveryDirectory, std::shared_ptr<const storage::FileSystem> files, QObject *parent)
    : QObject(parent), journal_(std::move(recoveryDirectory), std::move(files))
{}

const FileSession *SessionStore::file(const std::string &id) const
{
    const auto row = rowOf(id);
    return row < 0 ? nullptr : &files_[static_cast<std::size_t>(row)];
}

int SessionStore::rowOf(const std::string &id) const
{
    const auto found = rows_.find(id);
    return found == rows_.end() ? -1 : static_cast<int>(found->second);
}

int SessionStore::supportCount(Support support) const
{
    return static_cast<int>(
        std::count_if(files_.begin(), files_.end(), [support](const auto &file) { return file.support() == support; }));
}

std::optional<SessionStore::Action> SessionStore::pendingAction(const std::string &id) const
{
    const auto *pending = journal_.pending(id);
    return pending ? std::optional<Action>(pending->action) : std::nullopt;
}

QString SessionStore::pendingError(const std::string &id) const
{
    const auto *pending = journal_.pending(id);
    return pending ? pending->error : QString{};
}

const Draft *SessionStore::pendingDraft(const std::string &id) const
{
    const auto *pending = journal_.pending(id);
    return pending ? &pending->candidate.current().draft : nullptr;
}

void SessionStore::recordRecoveryFailure(const std::string &id, const QString &error)
{
    journal_.recordFailure(id, error);
    if (const auto row = rowOf(id); row >= 0) notify(static_cast<std::size_t>(row), std::nullopt);
}

bool SessionStore::settleRecoveryFailure(const std::string &id, const QString &expected, const QString &replacement)
{
    if (!journal_.settleFailure(id, expected, replacement)) return false;
    if (const auto row = rowOf(id); row >= 0) notify(static_cast<std::size_t>(row), std::nullopt);
    return true;
}

storage::RecoveryCommit SessionStore::commitRecord(const storage::RecoveryTransaction &transaction)
{
    return journal_.commit(transaction);
}

Transition SessionStore::retryRecovery(const std::string &id)
{
    const auto action = pendingAction(id);
    if (!journal_.pending(id)) return file(id) ? Transition::NoChange : Transition::StaleRevision;
    const auto found = rowOf(id);
    if (found < 0 || !action) return Transition::StaleRevision;
    const auto row = static_cast<std::size_t>(found);
    auto committed = journal_.retryPending(id);
    if (committed) {
        files_[row] = std::move(*committed);
        facts_[row] = files_[row].facts();
    }
    notify(row, committed ? action : std::nullopt);
    return committed ? Transition::Applied : Transition::Blocked;
}

bool SessionStore::canCompleteLoad(const std::vector<FileSession> &files) const
{
    if (files.size() != files_.size()) return false;
    for (std::size_t row = 0; row < files_.size(); ++row) {
        const auto &file = files_[row];
        if (file.id() != files[row].id() || file.path() != files[row].path() || file.support() != Support::Unreadable
            || file.snapshots() || file.current().id != 1 || file.current().draft != Draft{} || file.checkpoint()
            || file.reviewed() || file.written() || file.staged() || file.checkpointOutcome() || file.writeOutcome()
            || file.stageOutcome())
            return false;
    }
    return journal_.onlyOpenCapturesPending();
}

void SessionStore::replaceFiles(std::vector<FileSession> files, RecoveryRestore *restore)
{
    Q_EMIT aboutToReplace();
    // The coordinator permits pending entries only for the placeholders of
    // this load. Retry must commit the captured files, never old placeholders.
    journal_.reset(restore ? std::move(restore->adopted) : std::nullopt);
    restored_.clear();
    files_ = std::move(files);
    facts_.clear();
    rows_.clear();
    for (std::size_t row = 0; row < files_.size(); ++row) {
        facts_.push_back(files_[row].facts());
        rows_.emplace(files_[row].id(), row);
    }
    for (std::size_t row = 0; row < files_.size(); ++row) {
        if (restore && !restore->newFiles.contains(files_[row].id()))
            restored_.insert(files_[row].id());
        else
            commitOpen(row);
    }
    Q_EMIT replaced();
}

bool SessionStore::appendFile(FileSession file)
{
    if (this->file(file.id()) || files_.size() >= static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return false;
    const auto row = static_cast<int>(files_.size());
    Q_EMIT aboutToAppend(row);
    facts_.push_back(file.facts());
    rows_.emplace(file.id(), files_.size());
    files_.push_back(std::move(file));
    commitOpen(files_.size() - 1);
    Q_EMIT appended(row);
    return true;
}

void SessionStore::commitOpen(std::size_t row)
{
    // A launch capture has no earlier text; a failure leaves it pending.
    (void)journal_.commitCandidate(files_[row], Action::Open, {});
}

bool SessionStore::commitCandidate(std::size_t row, FileSession candidate, Action action)
{
    if (!journal_.commitCandidate(candidate, action, files_[row].current().draft.text)) return false;
    files_[row] = std::move(candidate);
    facts_[row] = files_[row].facts();
    return true;
}

void SessionStore::notify(std::size_t row, std::optional<Action> action)
{
    if (action) Q_EMIT fileStored(static_cast<int>(row), *action);
    Q_EMIT fileChanged(static_cast<int>(row));
}

template <typename Command> Transition SessionStore::update(const std::string &id, Action action, Command command)
{
    const auto found = rowOf(id);
    if (found < 0) return Transition::StaleRevision;
    const auto row = static_cast<std::size_t>(found);
    if (hasPendingRecovery() || hasRecoveryFailure(id)) return Transition::Blocked;
    // Copies share the file's immutable revisions, so this costs no text copy.
    auto candidate = files_[row];
    const auto result = command(candidate);
    if (result != Transition::Applied) return result;
    const auto committed = commitCandidate(row, std::move(candidate), action);
    notify(row, committed ? std::optional<Action>(action) : std::nullopt);
    return committed ? Transition::Applied : Transition::Blocked;
}

Transition SessionStore::replaceDraft(const std::string &id, RevisionId expected, Draft draft)
{
    return update(id, Action::ReplaceDraft, [&](auto &file) { return file.replaceDraft(expected, std::move(draft)); });
}
Transition SessionStore::editResult(const std::string &id, RevisionId expected, ResultEdit edit)
{
    return update(id, Action::ReplaceDraft, [&](auto &file) { return file.editResult(expected, std::move(edit)); });
}
Transition SessionStore::applyChoice(const std::string &id, const ChoicePreview &preview, bool confirmReplacement)
{
    // Recovery replays the complete text-and-decision transaction as a draft
    // replacement. Undo/redo then retain exactly the same action order.
    return update(id, Action::ReplaceDraft,
                  [&](auto &file) { return branch_truce::applyChoice(file, preview, confirmReplacement); });
}
Transition SessionStore::applyNonConflicting(const std::string &id, const NonConflictingPreview &preview, bool confirm)
{
    return update(id, Action::ReplaceDraft,
                  [&](auto &file) { return branch_truce::applyNonConflicting(file, preview, confirm); });
}
Transition SessionStore::applyWholeFile(const std::string &id, const WholeFilePreview &preview, bool confirm)
{
    return update(id, Action::ReplaceDraft,
                  [&](auto &file) { return branch_truce::applyWholeFile(file, preview, confirm); });
}
Transition SessionStore::undo(const std::string &id, RevisionId expected)
{
    return update(id, Action::Undo, [&](auto &file) { return file.undo(expected); });
}
Transition SessionStore::redo(const std::string &id, RevisionId expected)
{
    return update(id, Action::Redo, [&](auto &file) { return file.redo(expected); });
}
Transition SessionStore::discardToCheckpoint(const std::string &id, RevisionId expected)
{
    return update(id, Action::DiscardToCheckpoint, [&](auto &file) { return file.discardToCheckpoint(expected); });
}
Transition SessionStore::discardToStaged(const std::string &id, RevisionId expected)
{
    return update(id, Action::DiscardToStaged, [&](auto &file) { return file.discardToStaged(expected); });
}
Transition SessionStore::beginMarkResolved(const std::string &id, RevisionId expected, bool confirmUndecided)
{
    return update(id, Action::MarkResolvedReview,
                  [&](auto &file) { return file.beginMarkResolved(expected, confirmUndecided); });
}
Transition SessionStore::recordCheckpoint(const std::string &id, RevisionId revision, Outcome outcome,
                                          std::string detail)
{
    return update(id, Action::ManualSave,
                  [&](auto &file) { return file.recordCheckpoint(revision, outcome, std::move(detail)); });
}
Transition SessionStore::recordWrite(const std::string &id, RevisionId revision, Outcome outcome, std::string detail)
{
    return update(id, Action::WriteOutcome,
                  [&](auto &file) { return file.recordWrite(revision, outcome, std::move(detail)); });
}
Transition SessionStore::recordStage(const std::string &id, RevisionId revision, Outcome outcome, std::string objectId,
                                     std::string mode, std::string detail)
{
    return update(id, Action::StageOutcome, [&](auto &file) {
        return file.recordStage(revision, outcome, std::move(objectId), std::move(mode), std::move(detail));
    });
}
Transition SessionStore::setSupport(const std::string &id, Support support, std::string detail)
{
    return update(id, Action::Support, [&](auto &file) { return file.setSupport(support, std::move(detail)); });
}
Transition SessionStore::loadLargeFile(const std::string &id)
{
    return update(id, Action::LoadLarge, [](auto &file) { return file.loadLargeFile(); });
}
}
