// The session model lists the store's files for the window, unresolved files first.
#pragma once
#include <QtQml/qqmlregistration.h>

#include "FilePresentation.h"
#include "FileStates.h"
#include "core/ResourceLimits.h"
#include "session/SessionStore.h"
#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVariant>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace branch_truce {

// A list model over one SessionStore. Rows are in presentation order:
// unresolved files, then resolved files, then files outside the app. A file
// changes group only when an action that repositions it stores a new state,
// so editing a resolved file does not move it under the pointer.
class SessionModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by the application")
    Q_PROPERTY(int totalFiles READ totalFiles NOTIFY summaryChanged)
    Q_PROPERTY(int resolvedFiles READ resolvedFiles NOTIFY summaryChanged)
    Q_PROPERTY(bool hasPendingRecovery READ hasPendingRecovery NOTIFY summaryChanged)
    Q_PROPERTY(int outsideAppFiles READ outsideAppFiles NOTIFY summaryChanged)
    Q_PROPERTY(int loadErrorFiles READ loadErrorFiles NOTIFY summaryChanged)
    // A file with more lines than this opens only after the user confirms it.
    Q_PROPERTY(int largeFileLines READ largeFileLines CONSTANT)
public:
    // PathRole is the escaped display path, never the path bytes. SupportRole,
    // StatusKindRole and RecoveryStateRole hold FileStates enums. StatusIconRole
    // uses the handoff category's icon, StatusLineRole is one short phrase,
    // ChipsRole is a list of {icon, text, tone} and SummaryRole is one sentence
    // for a tooltip or an accessible description.
    enum Role {
        FileIdRole = Qt::UserRole + 1,
        PathRole,
        DecisionsRole,
        CurrentRevisionRole,
        SupportRole,
        SupportDetailRole,
        UndecidedRole,
        UnsavedRole,
        ReviewNeededRole,
        ResolvedRole,
        StatusRole,
        FinishBlockersRole,
        CanUndoRole,
        CanRedoRole,
        PendingInputRole,
        PendingTextRole,
        RecoveryStateRole,
        RecoveryErrorRole,
        ProgressRole,
        StatusIconRole,
        StatusLineRole,
        ChipsRole,
        MimeIconRole,
        MimeDescriptionRole,
        SummaryRole,
        ConflictCountRole,
        StatusKindRole
    };
    Q_ENUM(Role)

    explicit SessionModel(const SessionStore &store, QObject *parent = nullptr);
    [[nodiscard]] const SessionStore &store() const { return store_; }
    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] int totalFiles() const;
    [[nodiscard]] int resolvedFiles() const;
    [[nodiscard]] bool hasPendingRecovery() const { return store_.hasPendingRecovery(); }
    [[nodiscard]] int outsideAppFiles() const { return store_.supportCount(Support::OutsideApp); }
    [[nodiscard]] int loadErrorFiles() const { return store_.supportCount(Support::Unreadable); }
    [[nodiscard]] static int largeFileLines() { return static_cast<int>(kLargeFileLines); }
    // The presentation row of a file ID, or -1.
    [[nodiscard]] int rowOf(const std::string &id) const;
    // The file in a presentation row, or null outside 0 to totalFiles().
    [[nodiscard]] const FileSession *fileAt(int row) const;
    // The file ID in a presentation row, or empty.
    [[nodiscard]] QString fileIdAt(int row) const;
    // The progress rule the status, chips and progress role use, for a row
    // inside 0 to totalFiles().
    [[nodiscard]] FilePresentation::Progress progress(int row) const;
    // The escaped label of a file's path, empty for an unknown ID. A label only.
    [[nodiscard]] QString displayPath(const std::string &id) const;

Q_SIGNALS:
    // Any file, count or recovery state changed.
    void summaryChanged();

private:
    [[nodiscard]] std::vector<std::size_t> presentationOrder() const;
    // Places every file by its current facts.
    void rebuild();
    void indexPresentation();
    void refreshPresentation();
    void storeReplaced();
    void storeAppended(int row);
    void storeStored(int row, SessionStore::Action action);
    void storeChanged(int row);

    const SessionStore &store_;
    FilePresentation presentation_;
    // presentationRows_[row] is the storage row shown in presentation row `row`.
    std::vector<std::size_t> presentationRows_;
    // The inverse of presentationRows_.
    std::vector<int> rowsByStorage_;
    // Placement follows explicit publication/restore, not every draft revision.
    std::vector<bool> presentationResolved_;
};

}
