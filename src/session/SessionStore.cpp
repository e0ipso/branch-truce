// The session store owns each file's history and the recovery journal that makes its edits durable.
#include "session/SessionStore.h"
#include "recovery/RecoveryRestore.h"
#include <QSignalBlocker>
#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <utility>

namespace branch_truce {
namespace {
// Whether `after` holds nothing a journal record could tell from `before`:
// the same text, decisions and hand edits.
bool recordsAlike(const FileSession &before, const FileSession &after)
{
    const auto &earlier = before.current();
    const auto &later = after.current();
    if (earlier.draft.text != later.draft.text || earlier.draft.decisions != later.draft.decisions) return false;
    const auto *from = earlier.correspondence.get();
    const auto *to = later.correspondence.get();
    if (!from || !to) return from == to;
    return from->handEditedRanges() == to->handEditedRanges() && from->handEditedAnchors() == to->handEditedAnchors();
}
}

bool SessionStore::repositions(Action action)
{
    switch (action) {
    case Action::Open:
    case Action::StageOutcome:
    case Action::DiscardToStaged:
    case Action::DiscardToCheckpoint: return true;
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
{
    pause_.setSingleShot(true);
    pause_.setInterval(kTypingPause);
    limit_.setSingleShot(true);
    limit_.setInterval(kTypingBurst);
    connect(&pause_, &QTimer::timeout, this, &SessionStore::endBursts);
    connect(&limit_, &QTimer::timeout, this, &SessionStore::endBursts);
}

SessionStore::~SessionStore()
{
    // Nothing listens any more; the records still have to reach the journal.
    const QSignalBlocker quiet(this);
    recordTyping();
}

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

Problem SessionStore::pendingError(const std::string &id) const
{
    const auto *pending = journal_.pending(id);
    return pending ? pending->problem : Problem{};
}

const Draft *SessionStore::pendingDraft(const std::string &id) const
{
    const auto *candidate = journal_.pendingCandidate(id);
    return candidate ? &candidate->current().draft : nullptr;
}

bool SessionStore::recordTyping()
{
    endBursts();
    journal_.finishBehind();
    return !hasPendingRecovery();
}

void SessionStore::endBursts()
{
    pause_.stop();
    limit_.stop();
    std::vector<std::string> open;
    for (const auto &[id, typing] : typing_)
        if (typing.open) open.push_back(id);
    for (const auto &id : open) endBurst(id);
}

void SessionStore::endBurst(const std::string &id)
{
    const auto entry = typing_.find(id);
    const auto row = rowOf(id);
    if (entry == typing_.end() || !entry->second.open || row < 0) return;
    auto &typing = entry->second;
    typing.open = false;
    const auto candidate = files_[static_cast<std::size_t>(row)];
    const auto previous = std::exchange(typing.recorded, candidate);
    ++typing.queued;
    // The outcome can arrive before this returns, so nothing touches `typing` after it.
    journal_.commitBehind(
        candidate, Action::ReplaceDraft, previous.current().draft.text,
        [this, id, candidate](RecoveryJournal::Written written) { typedRecordWritten(id, candidate, written); });
}

void SessionStore::typedRecordWritten(const std::string &id, const FileSession &candidate,
                                      RecoveryJournal::Written written)
{
    const auto entry = typing_.find(id);
    const auto found = rowOf(id);
    // A record held behind a failed one finds its file already rolled back.
    if (entry == typing_.end() || found < 0) return;
    const auto row = static_cast<std::size_t>(found);
    auto &typing = entry->second;
    --typing.queued;
    if (written == RecoveryJournal::Written::Committed) {
        typing.acknowledged = candidate;
        if (!typing.open && typing.queued == 0) typing_.erase(entry);
    } else {
        // Typing after the failed record joins it as pending input, so retry
        // records every typed edit in order. The file shows its acknowledged
        // state, and the editor its newest pending draft.
        auto acknowledged = typing.acknowledged;
        endBurst(id);
        typing_.erase(id);
        files_[row] = std::move(acknowledged);
        facts_[row] = files_[row].facts();
    }
    notify(row, std::nullopt);
}

void SessionStore::recordRecoveryFailure(const std::string &id, const Problem &problem)
{
    journal_.recordFailure(id, problem);
    if (const auto row = rowOf(id); row >= 0) notify(static_cast<std::size_t>(row), std::nullopt);
}

bool SessionStore::settleRecoveryFailure(const std::string &id, const Problem &expected, const Problem &replacement)
{
    if (!journal_.settleFailure(id, expected, replacement)) return false;
    if (const auto row = rowOf(id); row >= 0) notify(static_cast<std::size_t>(row), std::nullopt);
    return true;
}

storage::RecoveryCommit SessionStore::commitRecord(const storage::RecoveryTransaction &transaction)
{
    recordTyping();
    return journal_.commit(transaction);
}

bool SessionStore::discardRecoveryStore()
{
    recordTyping();
    return journal_.discardStore();
}

bool SessionStore::startNewRecoveryStore()
{
    recordTyping();
    return journal_.startNewStore();
}

Transition SessionStore::retryRecovery(const std::string &id)
{
    recordTyping();
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
    return journal_.pending(id) ? Transition::Blocked : Transition::Applied;
}

bool SessionStore::canRestoreLoad() const
{
    for (const auto &file : files_) {
        if (file.support() != Support::Unreadable || file.snapshots() || file.current().id != 1
            || file.current().draft != Draft{} || !file.publication().untouched())
            return false;
    }
    return journal_.onlyOpenCapturesPending();
}

void SessionStore::replaceFiles(std::vector<FileSession> files, RecoveryRestore *restore)
{
    // Typed edits belong in the store being left; SessionCoordinator refuses a
    // replacement while any of them is pending.
    recordTyping();
    typing_.clear();
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
    // Outcomes of typed records must not arrive inside the row insertion.
    recordTyping();
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
    // The command's record follows the typed edits before it, and starts a new undo entry.
    recordTyping();
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
Transition SessionStore::typeResult(const std::string &id, RevisionId expected, ResultEdit edit)
{
    const auto found = rowOf(id);
    if (found < 0) return Transition::StaleRevision;
    const auto row = static_cast<std::size_t>(found);
    if (hasPendingRecovery() || hasRecoveryFailure(id)) return Transition::Blocked;
    // A typed line break ends the burst before it and starts the next, so one
    // undo never takes back more than one line of typing.
    if (edit.replacement.find('\n') != std::string::npos) endBurst(id);
    if (hasPendingRecovery()) return Transition::Blocked;
    auto entry = typing_.find(id);
    // The file as it is before its first typed edit, which is acknowledged.
    std::optional<FileSession> before;
    if (entry == typing_.end()) before = files_[row];
    const bool join = entry != typing_.end() && entry->second.open;
    // A refused or failed edit leaves the file as it was.
    const auto result = files_[row].editResult(expected, edit, {.realignment = Realignment::Deferred, .join = join});
    if (result != Transition::Applied) return result;
    if (before) entry = typing_.emplace(id, Typing{*before, *before}).first;
    auto &typing = entry->second;
    typing.open = true;
    if (recordsAlike(typing.recorded, files_[row])) {
        // Typing that put back everything a record holds, such as a letter
        // and Backspace inside earlier hand edits, leaves nothing to record,
        // and replay could not make its undo entry. The file goes back to the
        // revision before the burst, before anyone can name the dropped one.
        files_[row] = typing.recorded;
        typing.open = false;
        if (typing.queued == 0) typing_.erase(entry);
    }
    facts_[row] = files_[row].facts();
    pause_.start();
    if (!limit_.isActive()) limit_.start();
    notify(row, std::nullopt);
    return Transition::Applied;
}
Transition SessionStore::installAlignment(const std::string &id, RevisionId revision,
                                          std::shared_ptr<const CorrespondenceMap> aligned)
{
    const auto found = rowOf(id);
    if (found < 0) return Transition::StaleRevision;
    const auto row = static_cast<std::size_t>(found);
    const auto result = files_[row].installAlignment(revision, std::move(aligned));
    if (result != Transition::Applied) return result;
    // The marker scan was counted again.
    facts_[row] = files_[row].facts();
    notify(row, std::nullopt);
    return result;
}
Transition SessionStore::settleAlignment(const std::string &id)
{
    const auto *settled = file(id);
    if (!settled) return Transition::StaleRevision;
    const auto pending = settled->pendingAlignment();
    if (!pending) return Transition::NoChange;
    std::shared_ptr<const CorrespondenceMap> aligned;
    try {
        aligned = std::make_shared<const CorrespondenceMap>(pending->realigned());
    } catch (const std::bad_alloc &) {
        return Transition::OutOfMemory;
    }
    return installAlignment(id, settled->current().id, std::move(aligned));
}
Transition SessionStore::applyChoice(const std::string &id, const ChoicePreview &preview, bool confirmReplacement)
{
    // A preview is made from exact passages, so it is compared with exact ones.
    (void)settleAlignment(id);
    // Recovery replays the complete text-and-decision transaction as a draft
    // replacement. Undo/redo then retain exactly the same action order.
    return update(id, Action::ReplaceDraft,
                  [&](auto &file) { return branch_truce::applyChoice(file, preview, confirmReplacement); });
}
Transition SessionStore::applyNonConflicting(const std::string &id, const NonConflictingPreview &preview, bool confirm)
{
    (void)settleAlignment(id);
    return update(id, Action::ReplaceDraft,
                  [&](auto &file) { return branch_truce::applyNonConflicting(file, preview, confirm); });
}
Transition SessionStore::applyWholeFile(const std::string &id, const WholeFilePreview &preview, bool confirm)
{
    (void)settleAlignment(id);
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
    // Mark resolved counts markers against exact passages. Settling them here
    // keeps them even when the review is refused.
    (void)settleAlignment(id);
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
Transition SessionStore::loadLargeFile(const std::string &id)
{
    return update(id, Action::LoadLarge, [](auto &file) { return file.loadLargeFile(); });
}
}
