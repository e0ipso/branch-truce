// The session model lists the store's files for the window, unresolved files first.
#include "SessionModel.h"
#include "EngineText.h"
#include "git/GitAdapter.h"
#include <QMimeDatabase>
#include <algorithm>
#include <numeric>

namespace branch_truce {
namespace {
QString text(const std::string &value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }

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

QString pathText(const std::string &path)
{
    return git::displayPath(QByteArray(path.data(), static_cast<qsizetype>(path.size())));
}
}

SessionModel::SessionModel(const SessionStore &store, QObject *parent)
    : QAbstractListModel(parent), store_(store), presentation_(store)
{
    connect(&store_, &SessionStore::aboutToReplace, this, [this] { beginResetModel(); });
    connect(&store_, &SessionStore::replaced, this, &SessionModel::storeReplaced);
    connect(&store_, &SessionStore::aboutToAppend, this, [this](int row) { beginInsertRows({}, row, row); });
    connect(&store_, &SessionStore::appended, this, &SessionModel::storeAppended);
    connect(&store_, &SessionStore::fileStored, this, &SessionModel::storeStored);
    connect(&store_, &SessionStore::fileChanged, this, &SessionModel::storeChanged);
    rebuild();
}

void SessionModel::rebuild()
{
    presentationResolved_.assign(store_.size(), false);
    for (std::size_t row = 0; row < store_.size(); ++row) presentationResolved_[row] = store_.facts(row).resolved;
    presentationRows_ = presentationOrder();
    indexPresentation();
}

void SessionModel::storeReplaced()
{
    rebuild();
    endResetModel();
    Q_EMIT summaryChanged();
}

void SessionModel::storeAppended(int row)
{
    const auto storage = static_cast<std::size_t>(row);
    presentationRows_.push_back(storage);
    presentationResolved_.push_back(store_.facts(storage).resolved);
    indexPresentation();
    endInsertRows();
    refreshPresentation();
    Q_EMIT summaryChanged();
}

void SessionModel::storeStored(int row, SessionStore::Action action)
{
    const auto storage = static_cast<std::size_t>(row);
    if (SessionStore::repositions(action)) presentationResolved_[storage] = store_.facts(storage).resolved;
}

void SessionModel::storeChanged(int row)
{
    const auto storage = static_cast<std::size_t>(row);
    refreshPresentation();
    const auto changed = index(rowsByStorage_[storage], 0);
    Q_EMIT dataChanged(changed, changed);
    Q_EMIT summaryChanged();
}

std::vector<std::size_t> SessionModel::presentationOrder() const
{
    std::vector<std::size_t> order(store_.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    const auto group = [this](std::size_t row) {
        if (store_.files()[row].support() != Support::Supported) return 2;
        return presentationResolved_[row] ? 1 : 0;
    };
    std::stable_sort(order.begin(), order.end(), [&](auto left, auto right) { return group(left) < group(right); });
    return order;
}

void SessionModel::indexPresentation()
{
    rowsByStorage_.assign(presentationRows_.size(), -1);
    for (std::size_t row = 0; row < presentationRows_.size(); ++row)
        rowsByStorage_[presentationRows_[row]] = static_cast<int>(row);
}

void SessionModel::refreshPresentation()
{
    const auto order = presentationOrder();
    for (std::size_t target = 0; target < order.size(); ++target) {
        const auto found = std::find(presentationRows_.begin() + static_cast<std::ptrdiff_t>(target),
                                     presentationRows_.end(), order[target]);
        const auto source = static_cast<int>(found - presentationRows_.begin());
        if (source == static_cast<int>(target)) continue;
        // Move rows, keeping Qt's persistent indexes and delegate identities intact.
        beginMoveRows({}, source, source, {}, static_cast<int>(target));
        presentationRows_.erase(found);
        presentationRows_.insert(presentationRows_.begin() + static_cast<std::ptrdiff_t>(target), order[target]);
        indexPresentation();
        endMoveRows();
    }
}

int SessionModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : totalFiles(); }

int SessionModel::totalFiles() const { return static_cast<int>(presentationRows_.size()); }

QVariant SessionModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.model() != this || index.column() != 0 || index.row() < 0
        || index.row() >= totalFiles())
        return {};
    const auto row = presentationRows_[static_cast<std::size_t>(index.row())];
    const auto &file = store_.files()[row];
    const auto &facts = store_.facts(row);
    const auto *pendingDraft = store_.pendingDraft(file.id());
    const bool pendingInput = pendingDraft != nullptr;
    const auto *failure = store_.recoveryFailure(file.id());
    const bool recoveryFailed = failure != nullptr;
    switch (role) {
    case FileIdRole: return text(file.id());
    case Qt::DisplayRole:
    case PathRole: return pathText(file.path());
    case DecisionsRole: return decisionData(file.current().draft);
    case CurrentRevisionRole: return revisionText(file.current().id);
    case SupportRole: return QVariant::fromValue(FilePresentation::supportKind(file.support()));
    case SupportDetailRole: return engine_text::supportDetail(file);
    case UndecidedRole: return QVariant::fromValue(static_cast<qulonglong>(facts.undecided));
    case ConflictCountRole: return QVariant::fromValue(static_cast<qulonglong>(file.conflictCount()));
    case UnsavedRole: return presentation_.unsaved(row);
    case ReviewNeededRole: return facts.needsReview;
    case ResolvedRole: return presentation_.resolved(row);
    case StatusRole: return presentation_.status(row);
    case StatusKindRole: return QVariant::fromValue(presentation_.statusKind(row));
    case FinishBlockersRole: return presentation_.blockers(row);
    case CanUndoRole: return !pendingInput && !recoveryFailed && file.canUndo();
    case CanRedoRole: return !pendingInput && !recoveryFailed && file.canRedo();
    case PendingInputRole: return pendingInput;
    case PendingTextRole: return pendingInput ? text(pendingDraft->text) : QString{};
    case RecoveryStateRole: return QVariant::fromValue(presentation_.recoveryState(row));
    case RecoveryErrorRole: return recoveryFailed ? *failure : store_.pendingError(file.id());
    case ProgressRole: return presentation_.progressData(row);
    case StatusIconRole: return FilePresentation::categoryIcon(presentation_.route(row).category);
    case StatusLineRole: return presentation_.statusLine(row);
    case ChipsRole: return presentation_.chips(row);
    case MimeIconRole:
    case MimeDescriptionRole: {
        // Use the captured path, never a later read of the mutable worktree or draft.
        const auto mime = QMimeDatabase().mimeTypeForFile(text(file.path()), QMimeDatabase::MatchExtension);
        if (role == MimeDescriptionRole) return mime.isDefault() ? QString{} : mime.comment();
        const auto icon = mime.iconName();
        return mime.isDefault() || icon.isEmpty() ? QStringLiteral("unknown") : icon;
    }
    case SummaryRole: return presentation_.summary(row);
    default: return {};
    }
}

QHash<int, QByteArray> SessionModel::roleNames() const
{
    return {{FileIdRole, "fileId"},
            {PathRole, "path"},
            {DecisionsRole, "decisions"},
            {CurrentRevisionRole, "currentRevision"},
            {SupportRole, "support"},
            {SupportDetailRole, "supportDetail"},
            {UndecidedRole, "undecidedCount"},
            {UnsavedRole, "unsaved"},
            {ReviewNeededRole, "reviewNeeded"},
            {ResolvedRole, "resolved"},
            {StatusRole, "status"},
            {FinishBlockersRole, "finishBlockers"},
            {CanUndoRole, "canUndo"},
            {CanRedoRole, "canRedo"},
            {PendingInputRole, "pendingInput"},
            {PendingTextRole, "pendingText"},
            {RecoveryStateRole, "recoveryState"},
            {RecoveryErrorRole, "recoveryError"},
            {ProgressRole, "progress"},
            {StatusIconRole, "statusIcon"},
            {StatusLineRole, "statusLine"},
            {ChipsRole, "chips"},
            {MimeIconRole, "mimeIcon"},
            {MimeDescriptionRole, "mimeDescription"},
            {SummaryRole, "summary"},
            {ConflictCountRole, "conflictCount"},
            {StatusKindRole, "statusKind"}};
}

int SessionModel::rowOf(const std::string &id) const
{
    const auto row = store_.rowOf(id);
    return row < 0 || static_cast<std::size_t>(row) >= rowsByStorage_.size()
        ? -1
        : rowsByStorage_[static_cast<std::size_t>(row)];
}

const FileSession *SessionModel::fileAt(int row) const
{
    return row < 0 || static_cast<std::size_t>(row) >= presentationRows_.size()
        ? nullptr
        : &store_.files()[presentationRows_[static_cast<std::size_t>(row)]];
}

QString SessionModel::fileIdAt(int row) const
{
    const auto *file = fileAt(row);
    return file ? text(file->id()) : QString{};
}

FilePresentation::Progress SessionModel::progress(int row) const
{
    return presentation_.progress(presentationRows_[static_cast<std::size_t>(row)]);
}

QString SessionModel::displayPath(const std::string &id) const
{
    const auto *found = store_.file(id);
    return found ? pathText(found->path()) : QString{};
}

int SessionModel::resolvedFiles() const
{
    int count = 0;
    for (std::size_t row = 0; row < store_.size(); ++row)
        if (presentation_.resolved(row)) ++count;
    return count;
}
}
