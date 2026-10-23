#pragma once

#include "core/SessionState.h"
#include <QAbstractListModel>
#include <QStringList>
#include <QVariantMap>
#include <utility>

namespace branch_truce {

class SessionModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int totalFiles READ totalFiles NOTIFY summaryChanged)
    Q_PROPERTY(int resolvedFiles READ resolvedFiles NOTIFY summaryChanged)
    Q_PROPERTY(bool canFinish READ canFinish NOTIFY summaryChanged)
    Q_PROPERTY(QVariantList finishBlockers READ finishBlockers NOTIFY summaryChanged)
public:
    enum Role {
        FileIdRole = Qt::UserRole + 1, PathRole, PathBytesRole, ResultTextRole, DecisionsRole,
        CurrentRevisionRole, ResultRevisionRole, DecisionRevisionRole,
        CheckpointRole, ReviewedRole, WrittenRole, StagedRole,
        CheckpointOutcomeRole, WriteOutcomeRole, StageOutcomeRole,
        SupportRole, SupportDetailRole, UndecidedRole, UnsavedRole, ReviewNeededRole,
        EarlierStagedRole, ResolvedRole, StatusRole, FinishBlockersRole, CanUndoRole, CanRedoRole
    };
    Q_ENUM(Role)

    explicit SessionModel(QObject *parent = nullptr);
    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] int totalFiles() const;
    [[nodiscard]] int resolvedFiles() const;
    [[nodiscard]] bool canFinish() const;
    [[nodiscard]] QVariantList finishBlockers() const;
    [[nodiscard]] const FileSession *file(const std::string &id) const;

    bool appendFile(FileSession file);
    Transition replaceDraft(const std::string &id, RevisionId expected, Draft draft);
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
    template<typename Command>
    Transition update(const std::string &id, Command command)
    {
        for (std::size_t row = 0; row < files_.size(); ++row) {
            if (files_[row].id() != id) continue;
            const auto result = command(files_[row]);
            if (result == Transition::Applied) {
                const auto changed = index(static_cast<int>(row), 0);
                Q_EMIT dataChanged(changed, changed);
                Q_EMIT summaryChanged();
            }
            return result;
        }
        return Transition::StaleRevision;
    }
    std::vector<FileSession> files_;
};

}
