// The session model presents each file's draft, decisions and resolution state to the window.
#pragma once
#include <QtQml/qqmlregistration.h>

#include "core/SessionState.h"
#include "core/EditingCommands.h"
#include "recovery/RecoveryJournal.h"
#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <cstddef>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace branch_truce {
struct RecoveryRestore;

// Lists session files and owns their result histories, recovery boundaries and observable status.
class SessionModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by ApplicationController")
    Q_PROPERTY(int totalFiles READ totalFiles NOTIFY summaryChanged)
    Q_PROPERTY(int resolvedFiles READ resolvedFiles NOTIFY summaryChanged)
    Q_PROPERTY(bool hasPendingRecovery READ hasPendingRecovery NOTIFY summaryChanged)
public:
    enum Role {
        FileIdRole = Qt::UserRole + 1, PathRole, DecisionsRole, CurrentRevisionRole,
        SupportRole, SupportDetailRole, UndecidedRole, UnsavedRole, ReviewNeededRole,
        ResolvedRole, StatusRole, FinishBlockersRole, CanUndoRole, CanRedoRole,
        PendingInputRole, PendingTextRole, RecoveryStateRole, RecoveryErrorRole,
        ProgressRole, StatusIconRole, StatusLineRole, ChipsRole, MimeIconRole,
        MimeDescriptionRole, SummaryRole, ConflictCountRole
    };
    Q_ENUM(Role)

    explicit SessionModel(QObject *parent = nullptr);
    explicit SessionModel(QString recoveryDirectory, QObject *parent = nullptr);
    // Roles the views read. PathRole is the escaped display path, never the path
    // bytes. StatusIconRole uses the handoff category's icon, StatusLineRole is
    // one short phrase, ChipsRole is a list of {icon, text, tone} and SummaryRole
    // is one sentence for a tooltip or an accessible description.
    // Holds every session's store. Built from GenericDataLocation and a fixed
    // segment, so setting an organization or application name never moves it.
    [[nodiscard]] static QString recoveryRoot();
    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] int totalFiles() const;
    [[nodiscard]] int resolvedFiles() const;
    [[nodiscard]] bool canFinish() const;
    [[nodiscard]] QVariantList finishBlockers() const;
    // Revision-derived handoff: one entry per file under exactly one category
    // (undecided, saved-draft, review-needed, newer-draft, written-unresolved,
    // published, failed, outside-app), each with its blockers and next action.
    [[nodiscard]] QVariantMap handoff() const;
    [[nodiscard]] const FileSession *file(const std::string &id) const;
    // The row of a file ID, or -1.
    [[nodiscard]] int rowOf(const std::string &id) const;
    // The file in a row, or null outside 0 to totalFiles().
    [[nodiscard]] const FileSession *fileAt(int row) const;
    // The progress facts of a row, with pending input and recovery errors applied.
    struct Progress {
        // Contribution decisions retained for review and handoff, even without markers.
        std::size_t undecided;
        bool saved;
        bool reviewed;
        bool resolved;
        bool earlierStaged;
        // Current conflict regions used by the file row and conflict stepper.
        std::size_t conflicts = 0;
    };
    // The progress rule the status, chips and progress role use, for a row
    // inside 0 to totalFiles().
    [[nodiscard]] Progress progress(int row) const;
    [[nodiscard]] int supportCount(Support support) const;
    // The escaped label of a file's path, empty for an unknown ID. A label only.
    [[nodiscard]] QString displayPath(const std::string &id) const;
    [[nodiscard]] const Draft *pendingDraft(const std::string &id) const;
    [[nodiscard]] bool hasPendingRecovery() const { return journal_.hasPending(); }
    [[nodiscard]] bool hasRecoveryFailures() const { return journal_.hasFailures(); }
    [[nodiscard]] bool hasRecoveryFailure(const std::string &id) const { return journal_.failure(id) != nullptr; }
    // Replaces a file's recovery error only while it is still exactly
    // expected; an empty replacement clears it. Returns whether it changed.
    bool settleRecoveryFailure(const std::string &id, const QString &expected, const QString &replacement = {});
    // Records a recovery error that blocks the file until it is settled.
    void recordRecoveryFailure(const std::string &id, const QString &error);
    [[nodiscard]] const QString &recoveryDirectory() const { return journal_.directory(); }
    // The store every acknowledged transaction of this model goes to.
    // Record per-file errors through recordRecoveryFailure, which notifies.
    [[nodiscard]] RecoveryJournal &recoveryJournal() { return journal_; }
    // Moves recovery to a new store beside the current one and releases the
    // current store's lease, so a repeated load never records over captures
    // already in it. Refused while pending input waits for this store.
    bool startNewRecoveryStore();
    Transition retryRecovery(const std::string &id);
    Q_INVOKABLE bool retryPendingRecovery(const QString &id);
    // Whether files may complete the current load: every row is still the
    // untouched placeholder for the same ID and path, and only its open
    // capture waits for recovery retry.
    [[nodiscard]] bool canCompleteLoad(const std::vector<FileSession> &files) const;
    [[nodiscard]] bool canRestoreLoad() const;
    // Resets every row and drops pending input and recovery errors. With a
    // restore, adopts its store and errors, and commits only its new files.
    // SessionCoordinator calls this after cancelling the old session's work.
    void replaceFiles(std::vector<FileSession> files, RecoveryRestore *restore = nullptr);

    bool appendFile(FileSession file);
    Transition replaceDraft(const std::string &id, RevisionId expected, Draft draft);
    Transition editResult(const std::string &id, RevisionId expected, ResultEdit edit);
    Transition applyChoice(const std::string &id, const ChoicePreview &preview, bool confirmReplacement = false);
    Transition applyNonConflicting(const std::string &id, const NonConflictingPreview &preview, bool confirm = false);
    Transition applyWholeFile(const std::string &id, const WholeFilePreview &preview, bool confirm = false);
    Transition undo(const std::string &id, RevisionId expected);
    Transition redo(const std::string &id, RevisionId expected);
    Transition discardToCheckpoint(const std::string &id, RevisionId expected);
    Transition discardToStaged(const std::string &id, RevisionId expected);
    Transition beginMarkResolved(const std::string &id, RevisionId expected, bool confirmUndecided = false);
    Transition recordCheckpoint(const std::string &id, RevisionId revision, Outcome outcome, std::string detail = {});
    Transition recordWrite(const std::string &id, RevisionId revision, Outcome outcome, std::string detail = {});
    Transition recordStage(const std::string &id, RevisionId revision, Outcome outcome,
                           std::string objectId = {}, std::string mode = {}, std::string detail = {});
    Transition setSupport(const std::string &id, Support support, std::string detail = {});

Q_SIGNALS:
    void summaryChanged();

private:
    // A handoff category with the next action and its label.
    struct Route {
        QString category;
        QString action;
        QString actionLabel;
    };
    bool commitCandidate(std::size_t row, FileSession candidate, const QString &action);
    // Internal rows address discovery-order storage; public rows address presentation.
    [[nodiscard]] int storageRowOf(const std::string &id) const;
    void setFile(std::size_t row, FileSession file, const QString &action);
    [[nodiscard]] std::vector<std::size_t> presentationOrder() const;
    void refreshPresentation();
    void notifyRow(std::size_t row);
    void notifyFile(const std::string &id);
    [[nodiscard]] bool resolvedAt(std::size_t row) const;
    [[nodiscard]] Progress progressAt(std::size_t row) const;
    [[nodiscard]] QVariantMap progressData(std::size_t row) const;
    // The one status rule for the status role and the handoff.
    [[nodiscard]] QString statusAt(std::size_t row) const;
    [[nodiscard]] QString statusLineAt(std::size_t row) const;
    [[nodiscard]] QVariantList chipsAt(std::size_t row) const;
    [[nodiscard]] QString summaryAt(std::size_t row) const;
    [[nodiscard]] Route routeAt(std::size_t row) const;
    [[nodiscard]] QStringList blockersAt(std::size_t row) const;
    [[nodiscard]] QVariantMap handoffEntry(std::size_t row) const;
    template<typename Command>
    Transition update(const std::string &id, const QString &action, Command command)
    {
        const auto found = storageRowOf(id);
        if (found < 0) return Transition::StaleRevision;
        const auto row = static_cast<std::size_t>(found);
        if (hasPendingRecovery() || hasRecoveryFailure(id)) return Transition::Blocked;
        auto candidate = files_[row];
        const auto result = command(candidate);
        if (result != Transition::Applied) return result;
        const auto committed = commitCandidate(row, std::move(candidate), action);
        notifyRow(row);
        return committed ? Transition::Applied : Transition::Blocked;
    }
    std::vector<FileSession> files_;
    // facts_[row] is files_[row].facts(), recomputed whenever setFile stores the row.
    std::vector<FileFacts> facts_;
    std::unordered_map<std::string, std::size_t> rows_;
    std::vector<std::size_t> presentationRows_;
    // Placement follows explicit publication/restore, not every draft revision.
    std::vector<bool> presentationResolved_;
    RecoveryJournal journal_;
    std::set<std::string> restored_;
};

}
