#include "SessionModel.h"
#include "recovery/RecoveryRestore.h"
#include "git/GitAdapter.h"
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>
#include <limits>

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

QStringList blockerData(const FileFacts &facts)
{
    QStringList result;
    for (const auto blocker : facts.blockers) result.push_back(QString::fromUtf8(blockerText(blocker)));
    return result;
}

QString pathText(const std::string &path)
{
    return git::displayPath(QByteArray(path.data(), static_cast<qsizetype>(path.size())));
}

// Icon names are written as designated initializers so the theme icon check
// collects them from this file.
struct CategoryIcon {
    const char *category;
    const char *icon;
};
constexpr CategoryIcon categoryIcons[] = {
    {.category = "undecided", .icon = "vcs-conflicting"},
    {.category = "saved-draft", .icon = "document-save"},
    {.category = "review-needed", .icon = "vcs-locally-modified"},
    {.category = "newer-draft", .icon = "vcs-update-required"},
    {.category = "written-unresolved", .icon = "vcs-locally-modified"},
    {.category = "failed", .icon = "dialog-error"},
    {.category = "outside-app", .icon = "unknown"},
    {.category = "published", .icon = "checkmark"},
};

QString categoryIcon(const QString &category)
{
    for (const auto &entry : categoryIcons)
        if (category == QLatin1String(entry.category)) return QString::fromLatin1(entry.icon);
    return {};
}

// One status fact. tone is neutral, positive, negative or warning.
struct Chip {
    const char *icon;
    QString text;
    const char *tone;
};

QVariantMap chipData(const Chip &chip)
{
    return {{QStringLiteral("icon"), QString::fromLatin1(chip.icon)}, {QStringLiteral("text"), chip.text},
            {QStringLiteral("tone"), QString::fromLatin1(chip.tone)}};
}
}

SessionModel::SessionModel(QObject *parent)
    : SessionModel(recoveryRoot() + QLatin1Char('/') + QUuid::createUuid().toString(QUuid::WithoutBraces), parent) {}

QString SessionModel::recoveryRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/Branch Truce/recovery/v1");
}

SessionModel::SessionModel(QString recoveryDirectory, QObject *parent)
    : QAbstractListModel(parent), journal_(std::move(recoveryDirectory)) {}

bool SessionModel::startNewRecoveryStore()
{
    return journal_.startNewStore();
}

void SessionModel::recordRecoveryFailure(const std::string &id, const QString &error)
{
    journal_.recordFailure(id, error);
    notifyFile(id);
}

bool SessionModel::settleRecoveryFailure(const std::string &id, const QString &expected, const QString &replacement)
{
    if (!journal_.settleFailure(id, expected, replacement)) return false;
    notifyFile(id);
    return true;
}

bool SessionModel::commitCandidate(std::size_t row, FileSession candidate, const QString &action)
{
    if (!journal_.commitCandidate(candidate, action)) return false;
    setFile(row, std::move(candidate));
    return true;
}

void SessionModel::setFile(std::size_t row, FileSession file)
{
    files_[row] = std::move(file);
    facts_[row] = files_[row].facts();
}

void SessionModel::notifyFile(const std::string &id)
{
    if (const auto row = rowOf(id); row >= 0) notifyRow(static_cast<std::size_t>(row));
}

void SessionModel::notifyRow(std::size_t row)
{
    const auto changed = index(static_cast<int>(row), 0);
    Q_EMIT dataChanged(changed, changed);
    Q_EMIT summaryChanged();
}

const Draft *SessionModel::pendingDraft(const std::string &id) const
{
    const auto *pending = journal_.pending(id);
    return pending ? &pending->candidate.current().draft : nullptr;
}

Transition SessionModel::retryRecovery(const std::string &id)
{
    if (!journal_.pending(id)) return file(id) ? Transition::NoChange : Transition::StaleRevision;
    const auto found = rowOf(id);
    if (found < 0) return Transition::StaleRevision;
    const auto row = static_cast<std::size_t>(found);
    auto committed = journal_.retryPending(id);
    if (committed) setFile(row, std::move(*committed));
    notifyRow(row);
    return committed ? Transition::Applied : Transition::Blocked;
}

bool SessionModel::retryPendingRecovery(const QString &id)
{
    return retryRecovery(id.toStdString()) == Transition::Applied;
}

bool SessionModel::canCompleteLoad(const std::vector<FileSession> &files) const
{
    return journal_.canCompleteLoad(files_, files);
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
    journal_.reset(restore);
    restored_.clear();
    files_ = std::move(files);
    facts_.clear();
    rows_.clear();
    for (std::size_t row = 0; row < files_.size(); ++row) {
        facts_.push_back(files_[row].facts());
        rows_.emplace(files_[row].id(), row);
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
    const auto row = static_cast<std::size_t>(index.row());
    const auto &file = files_[row];
    const auto &facts = facts_[row];
    const auto *pending = journal_.pending(file.id());
    const bool pendingInput = pending != nullptr;
    const auto *failure = journal_.failure(file.id());
    const bool recoveryFailed = failure != nullptr;
    switch (role) {
    case FileIdRole: return text(file.id());
    case Qt::DisplayRole:
    case PathRole: return pathText(file.path());
    case DecisionsRole: return decisionData(file.current().draft);
    case CurrentRevisionRole: return revisionText(file.current().id);
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
    case ResolvedRole: return resolvedAt(row);
    case StatusRole: return statusAt(row);
    case FinishBlockersRole: return blockersAt(row);
    case CanUndoRole: return !pendingInput && !recoveryFailed && file.canUndo();
    case CanRedoRole: return !pendingInput && !recoveryFailed && file.canRedo();
    case PendingInputRole: return pendingInput;
    case PendingTextRole: return pendingInput ? text(pending->candidate.current().draft.text) : QString{};
    case RecoveryStateRole:
        if (recoveryFailed) return QStringLiteral("blocked");
        if (pendingInput && pending->action == QLatin1String("manual-save"))
            return QStringLiteral("save-failed");
        if (pendingInput) return QStringLiteral("failed");
        return restored_.contains(file.id()) ? QStringLiteral("restored") : QStringLiteral("durable");
    case RecoveryErrorRole: return recoveryFailed ? *failure : pendingInput ? pending->error : QString{};
    case ProgressRole: return progressData(row);
    case StatusIconRole: return categoryIcon(routeAt(row).category);
    case StatusLineRole: return statusLineAt(row);
    case ChipsRole: return chipsAt(row);
    case SummaryRole: return summaryAt(row);
    default: return {};
    }
}

QHash<int, QByteArray> SessionModel::roleNames() const
{
    return {{FileIdRole, "fileId"}, {PathRole, "path"}, {DecisionsRole, "decisions"},
            {CurrentRevisionRole, "currentRevision"}, {SupportRole, "support"},
            {SupportDetailRole, "supportDetail"}, {UndecidedRole, "undecidedCount"},
            {UnsavedRole, "unsaved"}, {ReviewNeededRole, "reviewNeeded"}, {ResolvedRole, "resolved"},
            {StatusRole, "status"}, {FinishBlockersRole, "finishBlockers"},
            {CanUndoRole, "canUndo"}, {CanRedoRole, "canRedo"}, {PendingInputRole, "pendingInput"},
            {PendingTextRole, "pendingText"}, {RecoveryStateRole, "recoveryState"},
            {RecoveryErrorRole, "recoveryError"}, {ProgressRole, "progress"},
            {StatusIconRole, "statusIcon"}, {StatusLineRole, "statusLine"}, {ChipsRole, "chips"},
            {SummaryRole, "summary"}};
}

const FileSession *SessionModel::file(const std::string &id) const
{
    const auto row = rowOf(id);
    return row < 0 ? nullptr : &files_[static_cast<std::size_t>(row)];
}

int SessionModel::rowOf(const std::string &id) const
{
    const auto found = rows_.find(id);
    return found == rows_.end() ? -1 : static_cast<int>(found->second);
}

const FileSession *SessionModel::fileAt(int row) const
{
    return row < 0 || static_cast<std::size_t>(row) >= files_.size() ? nullptr : &files_[static_cast<std::size_t>(row)];
}

SessionModel::Progress SessionModel::progress(int row) const
{
    return progressAt(static_cast<std::size_t>(row));
}

int SessionModel::supportCount(Support support) const
{
    return static_cast<int>(std::count_if(files_.begin(), files_.end(),
                                          [support](const auto &file) { return file.support() == support; }));
}

QString SessionModel::displayPath(const std::string &id) const
{
    const auto *found = file(id);
    return found ? pathText(found->path()) : QString{};
}

bool SessionModel::appendFile(FileSession file)
{
    if (this->file(file.id()) || files_.size() >= static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return false;
    const auto row = totalFiles();
    beginInsertRows({}, row, row);
    facts_.push_back(file.facts());
    rows_.emplace(file.id(), files_.size());
    files_.push_back(std::move(file));
    (void)commitCandidate(static_cast<std::size_t>(row), files_.back(), QStringLiteral("open"));
    endInsertRows();
    Q_EMIT summaryChanged();
    return true;
}

int SessionModel::resolvedFiles() const
{
    int count = 0;
    for (std::size_t row = 0; row < files_.size(); ++row)
        if (resolvedAt(row)) ++count;
    return count;
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
    for (std::size_t row = 0; row < files_.size(); ++row) {
        const auto blockers = blockersAt(row);
        if (blockers.isEmpty()) continue;
        result.push_back(QVariantMap{{QStringLiteral("fileId"), text(files_[row].id())},
            {QStringLiteral("path"), pathText(files_[row].path())},
            {QStringLiteral("blockers"), blockers}});
    }
    return result;
}

bool SessionModel::resolvedAt(std::size_t row) const
{
    const auto &id = files_[row].id();
    return !journal_.pending(id) && !journal_.failure(id) && facts_[row].resolved;
}

SessionModel::Progress SessionModel::progressAt(std::size_t row) const
{
    const auto &facts = facts_[row];
    return {facts.undecided, !journal_.pending(files_[row].id()) && !facts.unsaved, !facts.needsReview,
            resolvedAt(row), facts.earlierStaged};
}

// Dimensions come from the current revision and the saved, reviewed and staged
// revisions. They stay separate so one resolved flag cannot hide the others.
QVariantMap SessionModel::progressData(std::size_t row) const
{
    const auto &file = files_[row];
    const auto progress = progressAt(row);
    return {{QStringLiteral("undecided"), QVariant::fromValue(static_cast<qulonglong>(progress.undecided))},
        {QStringLiteral("saved"), progress.saved},
        {QStringLiteral("reviewed"), progress.reviewed},
        {QStringLiteral("resolved"), progress.resolved},
        {QStringLiteral("earlierResolved"), progress.earlierStaged},
        {QStringLiteral("currentRevision"), revisionText(file.current().id)},
        {QStringLiteral("savedRevision"), progress.saved && file.checkpoint() ? revisionText(file.checkpoint()->id) : QString{}},
        {QStringLiteral("reviewedRevision"), progress.reviewed ? revisionText(file.reviewed()->revision.id) : QString{}},
        {QStringLiteral("stagedRevision"), file.staged() ? revisionText(file.staged()->revision.id) : QString{}}};
}

QString SessionModel::statusAt(std::size_t row) const
{
    const auto &id = files_[row].id();
    if (journal_.failure(id)) return tr("Recovery requires attention; work remains outstanding");
    if (const auto *pending = journal_.pending(id)) {
        return pending->action == QLatin1String("manual-save")
            ? QString::fromUtf8(statusText(FileStatus::CheckpointFailed)) : tr("Recovery failed; pending input retained");
    }
    return QString::fromUtf8(statusText(facts_[row].status));
}

// A file outside the app names its reason and an unresolved file its undecided
// count; otherwise the status.
QString SessionModel::statusLineAt(std::size_t row) const
{
    const auto &file = files_[row];
    if (file.support() != Support::Supported)
        return file.supportDetail().empty() ? statusAt(row) : text(file.supportDetail());
    const auto progress = progressAt(row);
    if (progress.undecided > 0 && !progress.resolved)
        return tr("%1 undecided").arg(static_cast<qulonglong>(progress.undecided));
    return statusAt(row);
}

// The status sentence, the draft state, then one chip per progress fact.
QVariantList SessionModel::chipsAt(std::size_t row) const
{
    const auto progress = progressAt(row);
    QVariantList chips{chipData({.icon = "", .text = statusAt(row), .tone = "neutral"})};
    // Outside-app and unreadable files have no app draft to describe.
    if (files_[row].support() == Support::Supported) {
        chips.push_back(chipData(journal_.pending(files_[row].id()) || facts_[row].unsaved
            ? Chip{.icon = "document-edit", .text = tr("Unsaved draft"), .tone = "warning"}
            : Chip{.icon = "document-save", .text = tr("Manual Save checkpoint retained"), .tone = "neutral"}));
    }
    const bool undecided = progress.undecided > 0;
    chips.push_back(chipData({.icon = undecided ? "dialog-question" : "checkmark",
        .text = tr("Undecided: %1").arg(static_cast<qulonglong>(progress.undecided)),
        .tone = undecided ? "negative" : "positive"}));
    chips.push_back(chipData(progress.saved ? Chip{.icon = "document-save", .text = tr("Saved"), .tone = "positive"}
        : Chip{.icon = "vcs-locally-modified", .text = tr("Not saved"), .tone = "warning"}));
    chips.push_back(chipData(progress.reviewed ? Chip{.icon = "view-visible", .text = tr("Reviewed"), .tone = "positive"}
        : Chip{.icon = "view-hidden", .text = tr("Not reviewed"), .tone = "neutral"}));
    chips.push_back(chipData(progress.resolved ? Chip{.icon = "checkmark", .text = tr("Resolved"), .tone = "positive"}
        : Chip{.icon = "vcs-conflicting", .text = tr("Not resolved"), .tone = "neutral"}));
    if (progress.earlierStaged) {
        chips.push_back(chipData({.icon = "vcs-update-required",
            .text = tr("Earlier result staged; current draft not applied"), .tone = "warning"}));
    }
    return chips;
}

QString SessionModel::summaryAt(std::size_t row) const
{
    const auto progress = progressAt(row);
    return tr("%1. Undecided: %2. %3. %4. %5.").arg(statusAt(row),
        QString::number(static_cast<qulonglong>(progress.undecided)),
        progress.saved ? tr("Saved") : tr("Not saved"),
        progress.reviewed ? tr("Reviewed") : tr("Not reviewed"),
        progress.resolved ? tr("Resolved") : tr("Not resolved"));
}

QStringList SessionModel::blockersAt(std::size_t row) const
{
    const auto &file = files_[row];
    auto blockers = blockerData(facts_[row]);
    const auto *pending = journal_.pending(file.id());
    if (pending) blockers.push_back(pending->action == QLatin1String("manual-save")
        ? tr("Draft save failed; the edited result is still available")
        : tr("Pending input needs recovery retry"));
    if (const auto *failure = journal_.failure(file.id())) blockers.push_back(*failure);
    return blockers;
}

SessionModel::Route SessionModel::routeAt(std::size_t row) const
{
    const auto &facts = facts_[row];
    const auto *pending = journal_.pending(files_[row].id());
    const bool pendingInput = pending != nullptr;
    const bool recoveryFailed = journal_.failure(files_[row].id()) != nullptr;
    Route route;
    auto &[category, action, actionLabel] = route;
    // Recovery problems outrank every revision fact: until they clear, the
    // recorded state may not be the state the user last saw.
    if (recoveryFailed) {
        category = QStringLiteral("failed");
        action = QStringLiteral("recovery");
        actionLabel = tr("Open the recovery problem");
    } else if (pendingInput) {
        const bool save = pending->action == QLatin1String("manual-save");
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
    return route;
}

QVariantMap SessionModel::handoffEntry(std::size_t row) const
{
    const auto &file = files_[row];
    const auto &facts = facts_[row];
    const bool pendingInput = journal_.pending(file.id()) != nullptr;
    const auto route = routeAt(row);
    return {{QStringLiteral("fileId"), text(file.id())}, {QStringLiteral("path"), pathText(file.path())},
            {QStringLiteral("category"), route.category}, {QStringLiteral("icon"), categoryIcon(route.category)},
            {QStringLiteral("status"), statusAt(row)},
            {QStringLiteral("blockers"), blockersAt(row)},
            {QStringLiteral("unsaved"), pendingInput || facts.unsaved},
            {QStringLiteral("undecided"), QVariant::fromValue(static_cast<qulonglong>(facts.undecided))},
            {QStringLiteral("outstanding"), !resolvedAt(row)},
            {QStringLiteral("action"), route.action}, {QStringLiteral("actionLabel"), route.actionLabel}};
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
    for (std::size_t row = 0; row < files_.size(); ++row) {
        auto entry = handoffEntry(row);
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
