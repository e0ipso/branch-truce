// The session store owns each file's history and the recovery journal that makes its edits durable.
#include "session/SessionStore.h"
#include "recovery/RecoveryRecord.h"
#include "recovery/RecoveryRestore.h"
#include "session/BackgroundAlignment.h"
#include "storage/OffThread.h"
#include <QPromise>
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
    alignment_ = std::make_unique<BackgroundAlignment>(*this);
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
    journal_.close();
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
    if (pending) return pending->action;
    const auto typing = typing_.find(id);
    return typing != typing_.end() && typing->second.refused ? std::optional<Action>(Action::ReplaceDraft)
                                                             : std::nullopt;
}

Problem SessionStore::pendingError(const std::string &id) const
{
    const auto *pending = journal_.pending(id);
    if (pending) return pending->problem;
    const auto typing = typing_.find(id);
    return typing != typing_.end() && typing->second.refused ? Problem{ProblemCode::RecoveryResourceLimit} : Problem{};
}

bool SessionStore::hasPendingRecovery() const
{
    return retrying_ || journal_.hasPending() || std::any_of(typing_.begin(), typing_.end(), [](const auto &entry) {
               return entry.second.refused.has_value();
           });
}

const Draft *SessionStore::pendingDraft(const std::string &id) const
{
    if (const auto entry = typing_.find(id); entry != typing_.end() && entry->second.refused)
        return &entry->second.refused->current().draft;
    const auto *candidate = journal_.pendingCandidate(id, retrying_);
    return candidate ? &candidate->current().draft : nullptr;
}

bool SessionStore::recordTyping()
{
    endBursts();
    return !recording() && !hasPendingRecovery();
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
    const auto &candidate = typing.refused ? *typing.refused : files_[static_cast<std::size_t>(row)];
    auto record = journal_.commitBehind(candidate, Action::ReplaceDraft, &typing.recorded,
                                        [this, id](const RecoveryJournal::Record &record, Transition result) {
                                            typedRecordWritten(id, record->candidate, result);
                                        });
    if (!record) {
        if (!typing.refused) typing.refused = candidate;
        notify(static_cast<std::size_t>(row), std::nullopt);
        return;
    }
    typing.recorded = candidate;
    typing.refused.reset();
    typing.open = false;
    ++typing.queued;
}

void SessionStore::typedRecordWritten(const std::string &id, const FileSession &candidate, Transition written)
{
    const auto entry = typing_.find(id);
    const auto found = rowOf(id);
    if (entry == typing_.end() || found < 0) return;
    const auto row = static_cast<std::size_t>(found);
    auto &typing = entry->second;
    --typing.queued;
    if (written == Transition::Applied) {
        typing.acknowledged = candidate;
        if (files_[row].current().id < candidate.current().id) {
            files_[row] = candidate;
            facts_[row] = files_[row].facts();
        }
        if (typing.refused) endBurst(id);
        if (!typing.open && typing.queued == 0) typing_.erase(entry);
    } else {
        // Queue later typing before rolling back. A capacity refusal keeps its
        // single snapshot in Typing until the original FIFO has been retried.
        auto acknowledged = typing.acknowledged;
        endBurst(id);
        if (!typing.refused) typing_.erase(id);
        files_[row] = std::move(acknowledged);
        facts_[row] = files_[row].facts();
    }
    admitOpenCaptures();
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

QFuture<RecoveryJournal::Receipt> SessionStore::commitRecord(const storage::RecoveryTransaction &transaction)
{
    recordTyping();
    if (!journal_.canAdmit())
        return completedFuture(RecoveryJournal::Receipt{{false, ProblemCode::RecoveryResourceLimit}, journal_.lease()});
    auto promise = std::make_shared<QPromise<RecoveryJournal::Receipt>>();
    promise->start();
    auto future = promise->future();
    journal_.commit([transaction] { return transaction; },
                    [promise](RecoveryJournal::Receipt result) {
                        promise->addResult(result);
                        promise->finish();
                    });
    return future;
}

QFuture<RecoveryJournal::Receipt> SessionStore::commitMutation(const FileSession &file, MutationFact fact)
{
    recordTyping();
    if (!journal_.canAdmit())
        return completedFuture(RecoveryJournal::Receipt{{false, ProblemCode::RecoveryResourceLimit}, journal_.lease()});
    auto promise = std::make_shared<QPromise<RecoveryJournal::Receipt>>();
    promise->start();
    journal_.commit([file, fact = std::move(fact)] { return mutationRecoveryTransaction(file, fact); },
                    [promise](RecoveryJournal::Receipt result) {
                        promise->addResult(result);
                        promise->finish();
                    });
    return promise->future();
}

QFuture<bool> SessionStore::discardRecoveryStore()
{
    recordTyping();
    return journal_.discardStore();
}

bool SessionStore::startNewRecoveryStore()
{
    recordTyping();
    return journal_.startNewStore();
}

QFuture<Transition> SessionStore::retryRecovery(const std::string &id)
{
    recordTyping();
    auto promise = std::make_shared<QPromise<Transition>>();
    promise->start();
    auto future = promise->future();
    if (journal_.busy() || !journal_.pending(id)) {
        promise->addResult(journal_.busy() ? Transition::Blocked
                               : file(id)  ? Transition::NoChange
                                           : Transition::StaleRevision);
        promise->finish();
        return future;
    }
    retrying_ = true;
    journal_.retryPending(
        id,
        [this](FileSession candidate, Action action) {
            const auto row = rowOf(candidate.id());
            if (row < 0) return;
            files_[static_cast<std::size_t>(row)] = std::move(candidate);
            facts_[static_cast<std::size_t>(row)] = files_[static_cast<std::size_t>(row)].facts();
            notify(static_cast<std::size_t>(row), action);
        },
        [this, promise] {
            retrying_ = false;
            endBursts();
            admitOpenCaptures();
            for (std::size_t row = 0; row < files_.size(); ++row) notify(row, std::nullopt);
            auto *waiter = new QObject(this);
            const auto settled = [this, promise, waiter] {
                if (journal_.busy()) return;
                promise->addResult(hasPendingRecovery() || recording() ? Transition::Blocked : Transition::Applied);
                promise->finish();
                delete waiter;
            };
            connect(this, &SessionStore::fileChanged, waiter, [settled](int) { settled(); });
            connect(this, &QObject::destroyed, waiter, [promise] {
                if (promise->future().isFinished()) return;
                promise->addResult(Transition::Blocked);
                promise->finish();
            });
            settled();
        });
    return future;
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
    Q_ASSERT(!journal_.busy());
    ++generation_;
    typing_.clear();
    commands_.clear();
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
    nextOpen_ = 0;
    for (std::size_t row = 0; row < files_.size(); ++row) {
        if (restore && !restore->newFiles.contains(files_[row].id())) restored_.insert(files_[row].id());
    }
    admitOpenCaptures();
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
    admitOpenCaptures();
    Q_EMIT appended(row);
    return true;
}

void SessionStore::admitOpenCaptures()
{
    if (!journal_.hasPending()) {
        std::vector<std::string> refused;
        for (const auto &[id, typing] : typing_)
            if (typing.refused) refused.push_back(id);
        for (const auto &id : refused)
            if (journal_.canAdmit()) endBurst(id);
    }
    while (nextOpen_ < files_.size() && journal_.canAdmit() && !journal_.hasPending()) {
        const auto row = nextOpen_++;
        if (!restored_.contains(files_[row].id())) commitOpen(row);
    }
}

void SessionStore::commitOpen(std::size_t row)
{
    journal_.commitBehind(files_[row], Action::Open, nullptr, [this, row](const auto &, Transition) {
        admitOpenCaptures();
        notify(row, std::nullopt);
    });
}

void SessionStore::notify(std::size_t row, std::optional<Action> action)
{
    if (action) Q_EMIT fileStored(static_cast<int>(row), *action);
    Q_EMIT fileChanged(static_cast<int>(row));
}

template <typename Command>
QFuture<Transition> SessionStore::update(const std::string &id, Action action, Command command)
{
    const auto found = rowOf(id);
    if (found < 0) return completedFuture(Transition::StaleRevision);
    const auto row = static_cast<std::size_t>(found);
    if (row >= nextOpen_) return completedFuture(Transition::Blocked);
    recordTyping();
    if (hasPendingRecovery() || hasRecoveryFailure(id)) return completedFuture(Transition::Blocked);
    auto promise = std::make_shared<QPromise<Transition>>();
    promise->start();
    auto future = promise->future();
    const auto generation = generation_;
    auto token = journal_.commitBehind(
        files_[row], action, nullptr,
        [this, id, row, action, promise, generation](const RecoveryJournal::Record &record, Transition result) {
            if (generation != generation_)
                result = Transition::StaleRevision;
            else {
                if (result == Transition::Applied && files_[row].current().id != record->inputRevision)
                    result = Transition::StaleRevision;
                if (result == Transition::Applied) {
                    files_[row] = record->candidate;
                    facts_[row] = files_[row].facts();
                }
                if (const auto next = commands_.find(id); next != commands_.end() && next->second == record)
                    commands_.erase(next);
                admitOpenCaptures();
                notify(row, result == Transition::Applied ? std::optional<Action>{action} : std::nullopt);
            }
            promise->addResult(result);
            promise->finish();
        },
        std::move(command), commands_.contains(id) ? commands_.at(id) : nullptr);
    if (!token) return completedFuture(Transition::Blocked);
    commands_.insert_or_assign(id, std::move(token));
    notify(row, std::nullopt);
    return future;
}

QFuture<Transition> SessionStore::replaceDraft(const std::string &id, RevisionId expected, Draft draft)
{
    return update(id, Action::ReplaceDraft, [expected, draft = std::move(draft)](auto &file) mutable {
        return file.replaceDraft(expected, draft);
    });
}
QFuture<Transition> SessionStore::editResult(const std::string &id, RevisionId expected, ResultEdit edit)
{
    return update(id, Action::ReplaceDraft, [expected, edit = std::move(edit)](auto &file) {
        return file.editResult(expected, edit, {.realignment = Realignment::Deferred});
    });
}
Transition SessionStore::typeResult(const std::string &id, RevisionId expected, ResultEdit edit)
{
    const auto found = rowOf(id);
    if (found < 0) return Transition::StaleRevision;
    const auto row = static_cast<std::size_t>(found);
    if (row >= nextOpen_) return Transition::Blocked;
    if (hasPendingRecovery() || hasRecoveryFailure(id) || commands_.contains(id)) return Transition::Blocked;
    // A typed line break ends the burst before it and starts the next, so one
    // undo never takes back more than one line of typing.
    if (edit.replacement.find('\n') != std::string::npos) endBurst(id);
    if (hasPendingRecovery()) return Transition::Blocked;
    auto entry = typing_.find(id);
    // The file as it is before its first typed edit, which is acknowledged.
    std::optional<FileSession> before;
    if (entry == typing_.end()) {
        if (!journal_.canAdmit(typing_.size())) return Transition::Blocked;
        before = files_[row];
    }
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
QFuture<Transition> SessionStore::awaitAlignment(const std::string &id, RevisionId revision)
{
    return alignment_->waitFor(id, revision);
}
QFuture<Transition> SessionStore::applyChoice(const std::string &id, const ChoicePreview &preview,
                                              bool confirmReplacement)
{
    if (!journal_.canAdmit()) return completedFuture(Transition::Blocked);
    // A preview is made from exact passages, so it is compared with exact ones.
    // Recovery replays the complete text-and-decision transaction as a draft
    // replacement. Undo/redo then retain exactly the same action order.
    return update(id, Action::ReplaceDraft,
                  [=](auto &file) mutable { return branch_truce::applyChoice(file, preview, confirmReplacement); });
}
QFuture<Transition> SessionStore::applyNonConflicting(const std::string &id, const NonConflictingPreview &preview,
                                                      bool confirm)
{
    if (!journal_.canAdmit()) return completedFuture(Transition::Blocked);
    return update(id, Action::ReplaceDraft,
                  [=](auto &file) mutable { return branch_truce::applyNonConflicting(file, preview, confirm); });
}
QFuture<Transition> SessionStore::applyWholeFile(const std::string &id, const WholeFilePreview &preview, bool confirm)
{
    if (!journal_.canAdmit()) return completedFuture(Transition::Blocked);
    return update(id, Action::ReplaceDraft,
                  [=](auto &file) mutable { return branch_truce::applyWholeFile(file, preview, confirm); });
}
QFuture<Transition> SessionStore::undo(const std::string &id, RevisionId expected)
{
    return update(id, Action::Undo, [=](auto &file) mutable { return file.undo(expected); });
}
QFuture<Transition> SessionStore::redo(const std::string &id, RevisionId expected)
{
    return update(id, Action::Redo, [=](auto &file) mutable { return file.redo(expected); });
}
QFuture<Transition> SessionStore::discardToCheckpoint(const std::string &id, RevisionId expected)
{
    return update(id, Action::DiscardToCheckpoint,
                  [=](auto &file) mutable { return file.discardToCheckpoint(expected); });
}
QFuture<Transition> SessionStore::discardToStaged(const std::string &id, RevisionId expected)
{
    return update(id, Action::DiscardToStaged, [=](auto &file) mutable { return file.discardToStaged(expected); });
}
QFuture<Transition> SessionStore::beginMarkResolved(const std::string &id, RevisionId expected, bool confirmUndecided)
{
    if (const auto *current = file(id); current && current->pendingAlignment())
        return awaitAlignment(id, expected)
            .then(this,
                  [this, id, expected, confirmUndecided](Transition aligned) {
                      if (aligned != Transition::Applied && aligned != Transition::NoChange)
                          return completedFuture(aligned);
                      return beginMarkResolved(id, expected, confirmUndecided);
                  })
            .unwrap();
    return update(id, Action::MarkResolvedReview,
                  [=](auto &file) mutable { return file.beginMarkResolved(expected, confirmUndecided); });
}
QFuture<Transition> SessionStore::recordCheckpoint(const std::string &id, RevisionId revision, Outcome outcome,
                                                   std::string detail)
{
    return update(id, Action::ManualSave,
                  [=](auto &file) mutable { return file.recordCheckpoint(revision, outcome, std::move(detail)); });
}
QFuture<Transition> SessionStore::recordWrite(const std::string &id, RevisionId revision, Outcome outcome,
                                              std::string detail)
{
    return update(id, Action::WriteOutcome,
                  [=](auto &file) mutable { return file.recordWrite(revision, outcome, std::move(detail)); });
}
QFuture<Transition> SessionStore::recordStage(const std::string &id, RevisionId revision, Outcome outcome,
                                              std::string objectId, std::string mode, std::string detail)
{
    return update(id, Action::StageOutcome, [=](auto &file) mutable {
        return file.recordStage(revision, outcome, std::move(objectId), std::move(mode), std::move(detail));
    });
}
QFuture<Transition> SessionStore::loadLargeFile(const std::string &id)
{
    return update(id, Action::LoadLarge, [](auto &file) { return file.loadLargeFile(); });
}
}
