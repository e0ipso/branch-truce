#pragma once

#include "core/SessionState.h"
#include "core/EditingCommands.h"
#include "storage/RecoveryStore.h"
#include <QAbstractListModel>
#include <QStringList>
#include <QVariantMap>
#include <utility>
#include <memory>
#include <set>

class QLockFile;

namespace branch_truce {
struct RecoveryRestore;

class SessionModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int totalFiles READ totalFiles NOTIFY summaryChanged)
    Q_PROPERTY(int resolvedFiles READ resolvedFiles NOTIFY summaryChanged)
    Q_PROPERTY(bool canFinish READ canFinish NOTIFY summaryChanged)
    Q_PROPERTY(QVariantList finishBlockers READ finishBlockers NOTIFY summaryChanged)
    Q_PROPERTY(bool hasPendingRecovery READ hasPendingRecovery NOTIFY summaryChanged)
    Q_PROPERTY(bool hasRecoveryFailures READ hasRecoveryFailures NOTIFY summaryChanged)
public:
    enum Role {
        FileIdRole = Qt::UserRole + 1, PathRole, PathBytesRole, ResultTextRole, DecisionsRole,
        CurrentRevisionRole, ResultRevisionRole, DecisionRevisionRole,
        CheckpointRole, ReviewedRole, WrittenRole, StagedRole,
        CheckpointOutcomeRole, WriteOutcomeRole, StageOutcomeRole,
        SupportRole, SupportDetailRole, UndecidedRole, UnsavedRole, ReviewNeededRole,
        EarlierStagedRole, ResolvedRole, StatusRole, FinishBlockersRole, CanUndoRole, CanRedoRole,
        PendingInputRole, PendingTextRole, PendingDecisionsRole, RecoveryStateRole, RecoveryErrorRole
    };
    Q_ENUM(Role)

    explicit SessionModel(QObject *parent = nullptr);
    explicit SessionModel(QString recoveryDirectory, QObject *parent = nullptr);
    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] int totalFiles() const;
    [[nodiscard]] int resolvedFiles() const;
    [[nodiscard]] bool canFinish() const;
    [[nodiscard]] QVariantList finishBlockers() const;
    [[nodiscard]] const FileSession *file(const std::string &id) const;
    [[nodiscard]] const Draft *pendingDraft(const std::string &id) const;
    [[nodiscard]] bool hasPendingRecovery() const { return !pending_.empty(); }
    [[nodiscard]] bool hasRecoveryFailures() const { return !recoveryFailures_.empty(); }
    [[nodiscard]] bool hasRecoveryFailure(const std::string &id) const { return recoveryFailures_.contains(id); }
    [[nodiscard]] const QString &recoveryDirectory() const { return recovery_.directory(); }
    Transition retryRecovery(const std::string &id);
    Q_INVOKABLE bool retryPendingRecovery(const QString &id);

    bool appendFile(FileSession file);
    Transition replaceDraft(const std::string &id, RevisionId expected, Draft draft);
    Transition editResult(const std::string &id, RevisionId expected, ResultEdit edit);
    Transition applyCommand(const std::string &id, const EditingCommand &command, bool confirmReplacement = false);
    Transition applyChoice(const std::string &id, const ChoicePreview &preview, bool confirmReplacement = false);
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
    friend class SessionCoordinator;
    [[nodiscard]] bool canCompleteLoad(const std::vector<FileSession> &files) const;
    void replaceFiles(std::vector<FileSession> files, RecoveryRestore *restore = nullptr);
    [[nodiscard]] bool canRestoreLoad() const;
    QString ensureRecoveryLease();
    void recordRecoveryFailure(const std::string &id, const QString &error);
    bool commitCandidate(std::size_t row, FileSession candidate, const QString &action);
    void notifyRow(std::size_t row);
    template<typename Command>
    Transition update(const std::string &id, const QString &action, Command command)
    {
        for (std::size_t row = 0; row < files_.size(); ++row) {
            if (files_[row].id() != id) continue;
            if (hasPendingRecovery() || hasRecoveryFailure(id)) return Transition::Blocked;
            auto candidate = files_[row];
            const auto result = command(candidate);
            if (result == Transition::Applied) {
                const auto committed = commitCandidate(row, std::move(candidate), action);
                notifyRow(row);
                return committed ? Transition::Applied : Transition::Blocked;
            }
            return result;
        }
        return Transition::StaleRevision;
    }
    struct PendingRecovery {
        FileSession candidate;
        QString action;
        QString transactionId;
        QString error;
    };
    std::vector<FileSession> files_;
    storage::RecoveryStore recovery_;
    std::map<std::string, PendingRecovery> pending_;
    std::map<std::string, QString> recoveryFailures_;
    std::set<std::string> restored_;
    std::shared_ptr<QLockFile> recoveryLease_;
};

}
