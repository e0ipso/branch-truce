#include "SessionModel.h"
#include "SessionRecovery.h"
#include "RecoveryRestore.h"
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>
#include <limits>
#include <unistd.h>

namespace branch_truce {
namespace {
QString text(const std::string &value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString revisionText(RevisionId revision)
{
    // Decimal strings retain all 64 bits across the QML/JavaScript boundary.
    return QString::number(static_cast<qulonglong>(revision));
}

QVariantList decisionData(const Draft &draft)
{
    QVariantList result;
    for (const auto &[id, decision] : draft.decisions) {
        QString value;
        switch (decision.value) {
        case DecisionValue::Undecided: value = QStringLiteral("undecided"); break;
        case DecisionValue::Accepted: value = QStringLiteral("accepted"); break;
        case DecisionValue::Ignored: value = QStringLiteral("ignored"); break;
        }
        QVariantMap entry{{QStringLiteral("id"), text(id)}, {QStringLiteral("value"), value}};
        if (decision.order) entry.insert(QStringLiteral("order"), revisionText(*decision.order));
        result.push_back(entry);
    }
    return result;
}

QVariantMap revisionData(const Revision &revision)
{
    return {{QStringLiteral("revision"), revisionText(revision.id)},
            {QStringLiteral("resultRevision"), revisionText(revision.result)},
            {QStringLiteral("decisionRevision"), revisionText(revision.decisions)},
            {QStringLiteral("text"), text(revision.draft.text)},
            {QStringLiteral("decisions"), decisionData(revision.draft)},
            {QStringLiteral("unresolvedMarkers"), QVariant::fromValue(
                static_cast<qulonglong>(revision.draft.unresolvedMarkers))}};
}

QVariantMap outcomeData(const std::optional<OperationOutcome> &outcome)
{
    if (!outcome) return {};
    QString status;
    switch (outcome->outcome) {
    case Outcome::Succeeded: status = QStringLiteral("succeeded"); break;
    case Outcome::Failed: status = QStringLiteral("failed"); break;
    case Outcome::Uncertain: status = QStringLiteral("uncertain"); break;
    }
    return {{QStringLiteral("revision"), revisionText(outcome->revision)},
            {QStringLiteral("outcome"), status}, {QStringLiteral("detail"), text(outcome->detail)}};
}

QStringList blockerData(const FileFacts &facts)
{
    QStringList result;
    for (const auto blocker : facts.blockers) result.push_back(QString::fromUtf8(blockerText(blocker)));
    return result;
}

// Dimensions come from the current revision and the saved, reviewed and staged
// revisions. They stay separate so one resolved flag cannot hide the others.
QVariantMap progressData(const FileSession &file, bool pendingInput, bool recoveryFailed)
{
    const auto facts = file.facts();
    const bool reviewed = file.reviewed() && file.reviewed()->revision.id == file.current().id;
    const bool saved = !pendingInput && !facts.unsaved;
    const bool resolved = !pendingInput && !recoveryFailed && facts.resolved;
    return {{QStringLiteral("undecided"), QVariant::fromValue(static_cast<qulonglong>(facts.undecided))},
        {QStringLiteral("saved"), saved},
        {QStringLiteral("reviewed"), reviewed},
        {QStringLiteral("resolved"), resolved},
        {QStringLiteral("earlierResolved"), facts.earlierStaged},
        {QStringLiteral("currentRevision"), revisionText(file.current().id)},
        {QStringLiteral("savedRevision"), saved && file.checkpoint() ? revisionText(file.checkpoint()->id) : QString{}},
        {QStringLiteral("reviewedRevision"), reviewed ? revisionText(file.reviewed()->revision.id) : QString{}},
        {QStringLiteral("stagedRevision"), file.staged() ? revisionText(file.staged()->revision.id) : QString{}}};
}
}

SessionModel::SessionModel(QObject *parent)
    : SessionModel(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/recovery/v1/") + QUuid::createUuid().toString(QUuid::WithoutBraces), parent) {}

SessionModel::SessionModel(QString recoveryDirectory, QObject *parent)
    : QAbstractListModel(parent), recovery_(std::move(recoveryDirectory)) {}

QString SessionModel::ensureRecoveryLease()
{
    if (recoveryLease_) return {};
    const auto &directory = recovery_.directory();
    const QFileInfo before(directory);
    const auto permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    if (!QDir::isAbsolutePath(directory) || before.isSymLink()
        || (before.exists() && (!before.isDir() || before.ownerId() != ::geteuid()))
        || !QDir().mkpath(directory, permissions)) return tr("Recovery storage is unavailable or unsafe.");
    auto lease = std::make_shared<QLockFile>(directory + QStringLiteral("/session.lock"));
    if (!lease->tryLock(0)) return tr("Recovery session is already open or unavailable; close its other window before retrying.");
    recoveryLease_ = std::move(lease);
    return {};
}

void SessionModel::recordRecoveryFailure(const std::string &id, const QString &error)
{
    recoveryFailures_.insert_or_assign(id, error);
    for (std::size_t row = 0; row < files_.size(); ++row)
        if (files_[row].id() == id) { notifyRow(row); return; }
}

bool SessionModel::commitCandidate(std::size_t row, FileSession candidate, const QString &action)
{
    const auto transactionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto leaseError = ensureRecoveryLease();
    const auto result = leaseError.isEmpty() ? recovery_.commit(recoveryTransaction(candidate, action, transactionId))
        : storage::RecoveryCommit{false, leaseError};
    if (result.succeeded) {
        files_[row] = std::move(candidate);
        return true;
    }
    const auto id = candidate.id();
    pending_.insert_or_assign(id, PendingRecovery{std::move(candidate), action, transactionId, result.error});
    return false;
}

void SessionModel::notifyRow(std::size_t row)
{
    const auto changed = index(static_cast<int>(row), 0);
    Q_EMIT dataChanged(changed, changed);
    Q_EMIT summaryChanged();
}

const Draft *SessionModel::pendingDraft(const std::string &id) const
{
    const auto found = pending_.find(id);
    return found == pending_.end() ? nullptr : &found->second.candidate.current().draft;
}

Transition SessionModel::retryRecovery(const std::string &id)
{
    const auto pending = pending_.find(id);
    if (pending == pending_.end()) return file(id) ? Transition::NoChange : Transition::StaleRevision;
    for (std::size_t row = 0; row < files_.size(); ++row) {
        if (files_[row].id() != id) continue;
        const auto leaseError = ensureRecoveryLease();
        const auto result = leaseError.isEmpty() ? recovery_.commit(recoveryTransaction(pending->second.candidate,
            pending->second.action, pending->second.transactionId)) : storage::RecoveryCommit{false, leaseError};
        if (!result.succeeded) {
            pending->second.error = result.error;
            notifyRow(row);
            return Transition::Blocked;
        }
        files_[row] = std::move(pending->second.candidate);
        pending_.erase(pending);
        notifyRow(row);
        return Transition::Applied;
    }
    return Transition::StaleRevision;
}

bool SessionModel::retryPendingRecovery(const QString &id)
{
    return retryRecovery(id.toStdString()) == Transition::Applied;
}

bool SessionModel::canCompleteLoad(const std::vector<FileSession> &files) const
{
    if (files.size() != files_.size()) return false;
    for (std::size_t row = 0; row < files_.size(); ++row) {
        const auto &current = files_[row];
        if (current.id() != files[row].id() || current.path() != files[row].path()
            || current.support() != Support::Unreadable || current.snapshots()
            || current.current().id != 1 || current.current().draft != Draft{}
            || current.checkpoint() || current.reviewed() || current.written() || current.staged()
            || current.checkpointOutcome() || current.writeOutcome() || current.stageOutcome()) return false;
    }
    return std::all_of(pending_.cbegin(), pending_.cend(), [](const auto &pending) {
        return pending.second.action == QLatin1String("open");
    });
}

bool SessionModel::canRestoreLoad() const
{
    return canCompleteLoad(files_);
}

void SessionModel::replaceFiles(std::vector<FileSession> files, RecoveryRestore *restore)
{
    beginResetModel();
    // The coordinator permits pending entries only for the placeholders of
    // this load. Retry must commit the captured files, never old placeholders.
    pending_.clear();
    recoveryFailures_.clear();
    restored_.clear();
    files_ = std::move(files);
    if (restore) {
        recoveryLease_ = std::move(restore->lease);
        recovery_ = storage::RecoveryStore(restore->directory);
        recoveryFailures_ = std::move(restore->failures);
    }
    for (std::size_t row = 0; row < files_.size(); ++row) {
        if (restore && !restore->newFiles.contains(files_[row].id())) restored_.insert(files_[row].id());
        else (void)commitCandidate(row, files_[row], QStringLiteral("open"));
    }
    endResetModel();
    Q_EMIT summaryChanged();
}

int SessionModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : totalFiles();
}

int SessionModel::totalFiles() const { return static_cast<int>(files_.size()); }

QVariant SessionModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.model() != this || index.column() != 0
        || index.row() < 0 || index.row() >= totalFiles()) return {};
    const auto &file = files_[static_cast<std::size_t>(index.row())];
    const auto facts = file.facts();
    const auto pending = pending_.find(file.id());
    const bool pendingInput = pending != pending_.end();
    const auto failure = recoveryFailures_.find(file.id());
    const bool recoveryFailed = failure != recoveryFailures_.end();
    switch (role) {
    case FileIdRole: return text(file.id());
    case Qt::DisplayRole:
    case PathRole: return text(file.path());
    case PathBytesRole: return QByteArray(file.path().data(), static_cast<qsizetype>(file.path().size()));
    case ResultTextRole: return text(file.current().draft.text);
    case DecisionsRole: return decisionData(file.current().draft);
    case CurrentRevisionRole: return revisionText(file.current().id);
    case ResultRevisionRole: return revisionText(file.current().result);
    case DecisionRevisionRole: return revisionText(file.current().decisions);
    case CheckpointRole: return file.checkpoint() ? revisionData(*file.checkpoint()) : QVariantMap{};
    case ReviewedRole: {
        if (!file.reviewed()) return QVariantMap{};
        auto result = revisionData(file.reviewed()->revision);
        result.insert(QStringLiteral("confirmedUndecided"), file.reviewed()->confirmedUndecided);
        return result;
    }
    case WrittenRole: return file.written() ? revisionData(*file.written()) : QVariantMap{};
    case StagedRole: {
        if (!file.staged()) return QVariantMap{};
        auto result = revisionData(file.staged()->revision);
        result.insert(QStringLiteral("objectId"), text(file.staged()->objectId));
        result.insert(QStringLiteral("mode"), text(file.staged()->mode));
        return result;
    }
    case CheckpointOutcomeRole: return outcomeData(file.checkpointOutcome());
    case WriteOutcomeRole: return outcomeData(file.writeOutcome());
    case StageOutcomeRole: return outcomeData(file.stageOutcome());
    case SupportRole:
        switch (file.support()) {
        case Support::Supported: return QStringLiteral("supported");
        case Support::OutsideApp: return QStringLiteral("outside-app");
        case Support::Unreadable: return QStringLiteral("unreadable");
        }
        return {};
    case SupportDetailRole: return text(file.supportDetail());
    case UndecidedRole: return QVariant::fromValue(static_cast<qulonglong>(facts.undecided));
    case UnsavedRole: return pendingInput || facts.unsaved;
    case ReviewNeededRole: return facts.needsReview;
    case EarlierStagedRole: return facts.earlierStaged;
    case ResolvedRole: return !pendingInput && !recoveryFailed && facts.resolved;
    case StatusRole:
        if (recoveryFailed) return tr("Recovery requires attention; work remains outstanding");
        if (pendingInput && pending->second.action == QLatin1String("manual-save"))
            return QString::fromUtf8(statusText(FileStatus::CheckpointFailed));
        if (pendingInput) return tr("Recovery failed; pending input retained");
        return QString::fromUtf8(statusText(facts.status));
    case FinishBlockersRole: return blockersFor(file);
    case CanUndoRole: return !pendingInput && !recoveryFailed && file.canUndo();
    case CanRedoRole: return !pendingInput && !recoveryFailed && file.canRedo();
    case PendingInputRole: return pendingInput;
    case PendingTextRole: return pendingInput ? text(pending->second.candidate.current().draft.text) : QString{};
    case PendingDecisionsRole: return pendingInput ? decisionData(pending->second.candidate.current().draft) : QVariantList{};
    case RecoveryStateRole:
        if (recoveryFailed) return QStringLiteral("blocked");
        if (pendingInput && pending->second.action == QLatin1String("manual-save"))
            return QStringLiteral("save-failed");
        if (pendingInput) return QStringLiteral("failed");
        return restored_.contains(file.id()) ? QStringLiteral("restored") : QStringLiteral("durable");
    case RecoveryErrorRole: return recoveryFailed ? failure->second : pendingInput ? pending->second.error : QString{};
    case ProgressRole: return progressData(file, pendingInput, recoveryFailed);
    default: return {};
    }
}

QHash<int, QByteArray> SessionModel::roleNames() const
{
    return {{FileIdRole, "fileId"}, {PathRole, "path"}, {PathBytesRole, "pathBytes"},
            {ResultTextRole, "resultText"}, {DecisionsRole, "decisions"},
            {CurrentRevisionRole, "currentRevision"}, {ResultRevisionRole, "resultRevision"},
            {DecisionRevisionRole, "decisionRevision"}, {CheckpointRole, "manualCheckpoint"},
            {ReviewedRole, "reviewed"}, {WrittenRole, "written"}, {StagedRole, "staged"},
            {CheckpointOutcomeRole, "checkpointOutcome"}, {WriteOutcomeRole, "writeOutcome"},
            {StageOutcomeRole, "stageOutcome"}, {SupportRole, "support"},
            {SupportDetailRole, "supportDetail"}, {UndecidedRole, "undecidedCount"},
            {UnsavedRole, "unsaved"}, {ReviewNeededRole, "reviewNeeded"},
            {EarlierStagedRole, "earlierStaged"}, {ResolvedRole, "resolved"},
            {StatusRole, "status"}, {FinishBlockersRole, "finishBlockers"},
            {CanUndoRole, "canUndo"}, {CanRedoRole, "canRedo"},
            {PendingInputRole, "pendingInput"}, {PendingTextRole, "pendingText"},
            {PendingDecisionsRole, "pendingDecisions"}, {RecoveryStateRole, "recoveryState"},
            {RecoveryErrorRole, "recoveryError"}, {ProgressRole, "progress"}};
}

const FileSession *SessionModel::file(const std::string &id) const
{
    const auto found = std::find_if(files_.begin(), files_.end(),
                                    [&](const auto &file) { return file.id() == id; });
    return found == files_.end() ? nullptr : &*found;
}

bool SessionModel::appendFile(FileSession file)
{
    if (this->file(file.id()) || files_.size() >= static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return false;
    const auto row = totalFiles();
    beginInsertRows({}, row, row);
    files_.push_back(std::move(file));
    (void)commitCandidate(static_cast<std::size_t>(row), files_.back(), QStringLiteral("open"));
    endInsertRows();
    Q_EMIT summaryChanged();
    return true;
}

int SessionModel::resolvedFiles() const
{
    return static_cast<int>(std::count_if(files_.begin(), files_.end(),
                                          [this](const auto &file) { return !pending_.contains(file.id())
                                              && !recoveryFailures_.contains(file.id()) && file.facts().resolved; }));
}

bool SessionModel::canFinish() const
{
    return !hasPendingRecovery() && !hasRecoveryFailures() && !files_.empty() && resolvedFiles() == totalFiles();
}

QVariantList SessionModel::finishBlockers() const
{
    QVariantList result;
    if (files_.empty()) {
        result.push_back(QVariantMap{{QStringLiteral("fileId"), QString{}},
            {QStringLiteral("blockers"), QStringList{QStringLiteral("No files loaded")}}});
    }
    for (const auto &file : files_) {
        const auto blockers = blockersFor(file);
        if (blockers.isEmpty()) continue;
        result.push_back(QVariantMap{{QStringLiteral("fileId"), text(file.id())},
            {QStringLiteral("path"), text(file.path())},
            {QStringLiteral("blockers"), blockers}});
    }
    return result;
}

QStringList SessionModel::blockersFor(const FileSession &file) const
{
    auto blockers = blockerData(file.facts());
    const auto pending = pending_.find(file.id());
    if (pending != pending_.end()) blockers.push_back(pending->second.action == QLatin1String("manual-save")
        ? tr("Draft save failed; the edited result is still available")
        : tr("Pending input needs recovery retry"));
    const auto failure = recoveryFailures_.find(file.id());
    if (failure != recoveryFailures_.end()) blockers.push_back(failure->second);
    return blockers;
}

QVariantMap SessionModel::handoffEntry(const FileSession &file) const
{
    const auto facts = file.facts();
    const auto pending = pending_.find(file.id());
    const bool pendingInput = pending != pending_.end();
    const bool recoveryFailed = recoveryFailures_.contains(file.id());
    QString category;
    QString action;
    QString actionLabel;
    // Recovery problems outrank every revision fact: until they clear, the
    // recorded state may not be the state the user last saw.
    if (recoveryFailed) {
        category = QStringLiteral("failed");
        action = QStringLiteral("recovery");
        actionLabel = tr("Open the recovery problem");
    } else if (pendingInput) {
        const bool save = pending->second.action == QLatin1String("manual-save");
        category = QStringLiteral("failed");
        action = save ? QStringLiteral("retry-save") : QStringLiteral("retry-recovery");
        actionLabel = save ? tr("Retry Save") : tr("Retry recovery");
    } else {
        switch (facts.status) {
        case FileStatus::OutsideApp:
            category = QStringLiteral("outside-app");
            action = QStringLiteral("outside-app");
            actionLabel = tr("See why it needs work outside Branch Truce");
            break;
        case FileStatus::Unreadable:
            category = QStringLiteral("failed");
            action = QStringLiteral("inspect");
            actionLabel = tr("See why it could not be read");
            break;
        case FileStatus::OutcomeUncertain:
            category = QStringLiteral("failed");
            action = QStringLiteral("inspect");
            actionLabel = tr("Inspect the uncertain outcome");
            break;
        case FileStatus::CheckpointFailed:
            category = QStringLiteral("failed");
            action = QStringLiteral("retry-save");
            actionLabel = tr("Retry Save");
            break;
        case FileStatus::WriteFailed:
        case FileStatus::StageFailed:
            category = QStringLiteral("failed");
            action = QStringLiteral("mark-resolved");
            actionLabel = tr("Retry Mark resolved");
            break;
        case FileStatus::EarlierStaged:
            category = QStringLiteral("newer-draft");
            action = QStringLiteral("apply-or-discard");
            actionLabel = tr("Mark resolved or discard back to the staged result");
            break;
        case FileStatus::Resolved:
            category = QStringLiteral("published");
            break;
        case FileStatus::WrittenUnresolved:
            category = QStringLiteral("written-unresolved");
            action = QStringLiteral("check-git");
            actionLabel = tr("Check Git resolution");
            break;
        case FileStatus::SavedDraft:
            category = QStringLiteral("saved-draft");
            action = QStringLiteral("mark-resolved");
            actionLabel = tr("Review and Mark resolved");
            break;
        case FileStatus::Undecided:
            category = QStringLiteral("undecided");
            action = QStringLiteral("decide");
            actionLabel = tr("Decide source choices");
            break;
        case FileStatus::ReviewNeeded:
            category = QStringLiteral("review-needed");
            action = QStringLiteral("mark-resolved");
            actionLabel = facts.unsaved ? tr("Save or Mark resolved") : tr("Review and Mark resolved");
            break;
        }
    }
    const bool outstanding = pendingInput || recoveryFailed || !facts.resolved;
    QString status;
    if (recoveryFailed) status = tr("Recovery requires attention; work remains outstanding");
    else if (pendingInput && pending->second.action == QLatin1String("manual-save"))
        status = QString::fromUtf8(statusText(FileStatus::CheckpointFailed));
    else if (pendingInput) status = tr("Recovery failed; pending input retained");
    else status = QString::fromUtf8(statusText(facts.status));
    return {{QStringLiteral("fileId"), text(file.id())}, {QStringLiteral("path"), text(file.path())},
            {QStringLiteral("category"), category}, {QStringLiteral("status"), status},
            {QStringLiteral("blockers"), blockersFor(file)},
            {QStringLiteral("unsaved"), pendingInput || facts.unsaved},
            {QStringLiteral("undecided"), QVariant::fromValue(static_cast<qulonglong>(facts.undecided))},
            {QStringLiteral("outstanding"), outstanding},
            {QStringLiteral("action"), action}, {QStringLiteral("actionLabel"), actionLabel}};
}

QVariantMap SessionModel::handoff() const
{
    struct Group { const char *category; QString title; };
    const Group order[] = {
        {"undecided", tr("Unresolved decisions")},
        {"saved-draft", tr("Saved drafts in Branch Truce; repository files unchanged")},
        {"review-needed", tr("Review needed")},
        {"newer-draft", tr("Newer drafts than the result staged in Git")},
        {"written-unresolved", tr("Written, not yet resolved in Git")},
        {"failed", tr("Failed or uncertain work")},
        {"outside-app", tr("Files to handle outside Branch Truce")},
        {"published", tr("Written and resolved in Git")},
    };
    std::map<QString, QVariantList> entries;
    int outstanding = 0;
    for (const auto &file : files_) {
        auto entry = handoffEntry(file);
        if (entry.value(QStringLiteral("outstanding")).toBool()) ++outstanding;
        entries[entry.value(QStringLiteral("category")).toString()].push_back(std::move(entry));
    }
    QVariantList groups;
    QVariantMap counts;
    for (const auto &group : order) {
        const auto category = QString::fromLatin1(group.category);
        const auto &list = entries[category];
        counts.insert(category, static_cast<int>(list.size()));
        groups.push_back(QVariantMap{{QStringLiteral("category"), category},
            {QStringLiteral("title"), group.title}, {QStringLiteral("entries"), list}});
    }
    const bool clean = canFinish();
    QString outcome;
    if (files_.empty()) outcome = tr("No files are loaded in this session.");
    else if (clean) outcome = tr("All %1 files in this session are written and resolved in Git. "
        "Continue and commit in your usual workflow; Branch Truce has not checked the code.").arg(totalFiles());
    else outcome = tr("%1 of %2 files in this session remain outstanding. This session is not finished.")
        .arg(outstanding).arg(totalFiles());
    return {{QStringLiteral("clean"), clean}, {QStringLiteral("totalFiles"), totalFiles()},
            {QStringLiteral("outstandingFiles"), outstanding}, {QStringLiteral("counts"), counts},
            {QStringLiteral("groups"), groups}, {QStringLiteral("outcome"), outcome}};
}

Transition SessionModel::replaceDraft(const std::string &id, RevisionId expected, Draft draft)
{
    return update(id, QStringLiteral("replace-draft"), [&](auto &file) { return file.replaceDraft(expected, std::move(draft)); });
}
Transition SessionModel::applyWholeFile(const std::string &id, const WholeFilePreview &preview, bool confirm)
{
    return update(id, QStringLiteral("replace-draft"), [&](auto &file) {
        return branch_truce::applyWholeFile(file, preview, confirm);
    });
}

Transition SessionModel::undo(const std::string &id, RevisionId expected)
{
    return update(id, QStringLiteral("undo"), [&](auto &file) { return file.undo(expected); });
}
Transition SessionModel::editResult(const std::string &id, RevisionId expected, ResultEdit edit)
{
    return update(id, QStringLiteral("replace-draft"), [&](auto &file) { return file.editResult(expected, std::move(edit)); });
}
Transition SessionModel::applyCommand(const std::string &id, const EditingCommand &command, bool confirmReplacement)
{
    return update(id, QStringLiteral("replace-draft"), [&](auto &file) { return file.applyCommand(command, confirmReplacement); });
}
Transition SessionModel::applyChoice(const std::string &id, const ChoicePreview &preview, bool confirmReplacement)
{
    // Recovery replays the complete text-and-decision transaction as a draft
    // replacement. Undo/redo then retain exactly the same action order.
    return update(id, QStringLiteral("replace-draft"), [&](auto &file) {
        return branch_truce::applyChoice(file, preview, confirmReplacement);
    });
}
Transition SessionModel::redo(const std::string &id, RevisionId expected)
{
    return update(id, QStringLiteral("redo"), [&](auto &file) { return file.redo(expected); });
}
Transition SessionModel::applyNonConflicting(const std::string &id, const NonConflictingPreview &preview, bool confirm)
{
    return update(id, QStringLiteral("replace-draft"), [&](auto &file) {
        return branch_truce::applyNonConflicting(file, preview, confirm);
    });
}
Transition SessionModel::discardToCheckpoint(const std::string &id, RevisionId expected)
{
    return update(id, QStringLiteral("discard-to-checkpoint"), [&](auto &file) { return file.discardToCheckpoint(expected); });
}
Transition SessionModel::discardToStaged(const std::string &id, RevisionId expected)
{
    return update(id, QStringLiteral("discard-to-staged"), [&](auto &file) { return file.discardToStaged(expected); });
}
Transition SessionModel::beginMarkResolved(const std::string &id, RevisionId expected, bool confirmUndecided)
{
    return update(id, QStringLiteral("mark-resolved-review"), [&](auto &file) { return file.beginMarkResolved(expected, confirmUndecided); });
}
Transition SessionModel::recordCheckpoint(const std::string &id, RevisionId revision, Outcome outcome, std::string detail)
{
    return update(id, QStringLiteral("manual-save"), [&](auto &file) { return file.recordCheckpoint(revision, outcome, std::move(detail)); });
}
Transition SessionModel::recordWrite(const std::string &id, RevisionId revision, Outcome outcome, std::string detail)
{
    return update(id, QStringLiteral("write-outcome"), [&](auto &file) { return file.recordWrite(revision, outcome, std::move(detail)); });
}
Transition SessionModel::recordStage(const std::string &id, RevisionId revision, Outcome outcome,
                                    std::string objectId, std::string mode, std::string detail)
{
    return update(id, QStringLiteral("stage-outcome"), [&](auto &file) {
        return file.recordStage(revision, outcome, std::move(objectId), std::move(mode), std::move(detail));
    });
}
Transition SessionModel::setSupport(const std::string &id, Support support, std::string detail)
{
    return update(id, QStringLiteral("support"), [&](auto &file) { return file.setSupport(support, std::move(detail)); });
}
}
