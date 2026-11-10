// SelectedFile exposes the selected row and publishes coherent comparison projections.
#pragma once
#include "ComparisonPresentation.h"
#include "FileStates.h"
#include "SessionModel.h"
#include "TextPositions.h"
#include <QtQml/qqmlregistration.h>
#include <map>
#include <memory>

namespace branch_truce {
class Selection;

// The selected file's row and comparison, kept in step with the selection and
// the session. Each change of the comparison is published as one complete
// projection, so a binding reading several of its lists sees one revision.
class SelectedFile : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by the application")
    Q_PROPERTY(QString fileId READ fileId NOTIFY identityChanged)
    Q_PROPERTY(QString path READ path NOTIFY identityChanged)
    Q_PROPERTY(QByteArray pathBytes READ pathBytes NOTIFY identityChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY identityChanged)
    Q_PROPERTY(QString mimeIcon READ mimeIcon NOTIFY identityChanged)
    Q_PROPERTY(QString mimeDescription READ mimeDescription NOTIFY identityChanged)
    Q_PROPERTY(QString formatDescription READ formatDescription NOTIFY sourcesChanged)
    Q_PROPERTY(QVariantMap stage2 READ stage2 NOTIFY sourcesChanged)
    Q_PROPERTY(QVariantMap stage3 READ stage3 NOTIFY sourcesChanged)
    Q_PROPERTY(QVariantMap earlier READ earlier NOTIFY sourcesChanged)
    Q_PROPERTY(QVariantMap initialResult READ initialResult NOTIFY sourcesChanged)
    Q_PROPERTY(branch_truce::ComparisonPresentation *comparison READ comparison CONSTANT)
    Q_PROPERTY(QString resultText READ resultText NOTIFY resultTextChanged)
    Q_PROPERTY(QVariantList decisions READ decisions NOTIFY decisionsChanged)
    Q_PROPERTY(branch_truce::FileStates::SupportKind support READ support NOTIFY statusChanged)
    // Shorthand for support == Supported.
    Q_PROPERTY(bool supported READ supported NOTIFY statusChanged)
    Q_PROPERTY(QString supportDetail READ supportDetail NOTIFY statusChanged)
    Q_PROPERTY(bool binaryExcluded READ binaryExcluded NOTIFY statusChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(branch_truce::FileStates::StatusKind statusKind READ statusKind NOTIFY statusChanged)
    Q_PROPERTY(bool unsaved READ unsaved NOTIFY statusChanged)
    Q_PROPERTY(bool resolved READ resolved NOTIFY statusChanged)
    Q_PROPERTY(int undecided READ undecided NOTIFY statusChanged)
    Q_PROPERTY(bool pendingInput READ pendingInput NOTIFY statusChanged)
    Q_PROPERTY(QString pendingText READ pendingText NOTIFY statusChanged)
    Q_PROPERTY(branch_truce::FileStates::RecoveryState recoveryState READ recoveryState NOTIFY statusChanged)
    Q_PROPERTY(QString recoveryError READ recoveryError NOTIFY statusChanged)
    Q_PROPERTY(QString currentRevision READ currentRevision NOTIFY statusChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY statusChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY statusChanged)
    Q_PROPERTY(QVariantMap progress READ progress NOTIFY statusChanged)
    Q_PROPERTY(QString statusIcon READ statusIcon NOTIFY statusChanged)
    Q_PROPERTY(bool canEdit READ canEdit NOTIFY statusChanged)
public:
    SelectedFile(const SessionModel &files, const Selection &selection, QObject *parent = nullptr);
    // Reads the selected file again; the selection and the session call it.
    void sync();
    [[nodiscard]] QString fileId() const { return fileId_; }
    [[nodiscard]] QString path() const { return path_; }
    [[nodiscard]] QByteArray pathBytes() const { return pathBytes_; }
    [[nodiscard]] QString fileName() const { return fileName_; }
    [[nodiscard]] QString mimeIcon() const;
    [[nodiscard]] QString mimeDescription() const;
    [[nodiscard]] QString formatDescription() const;
    [[nodiscard]] QVariantMap stage2() const { return stage2_; }
    [[nodiscard]] QVariantMap stage3() const { return stage3_; }
    [[nodiscard]] QVariantMap earlier() const { return earlier_; }
    [[nodiscard]] QVariantMap initialResult() const { return initialResult_; }
    [[nodiscard]] QString resultText() const { return resultText_; }
    [[nodiscard]] QVariantList decisions() const { return decisions_; }
    ComparisonPresentation *comparison() { return &comparison_; }
    const ComparisonPresentation *comparison() const { return &comparison_; }
    // Unreadable without a selected file.
    [[nodiscard]] FileStates::SupportKind support() const
    {
        const auto kind = role(SessionModel::SupportRole);
        return kind.isValid() ? kind.value<FileStates::SupportKind>() : FileStates::SupportKind::Unreadable;
    }
    [[nodiscard]] bool supported() const { return support() == FileStates::SupportKind::Supported; }
    [[nodiscard]] QString supportDetail() const { return role(SessionModel::SupportDetailRole).toString(); }
    [[nodiscard]] bool binaryExcluded() const { return role(SessionModel::BinaryExcludedRole).toBool(); }
    [[nodiscard]] QString status() const { return role(SessionModel::StatusRole).toString(); }
    // Unreadable without a selected file.
    [[nodiscard]] FileStates::StatusKind statusKind() const
    {
        const auto kind = role(SessionModel::StatusKindRole);
        return kind.isValid() ? kind.value<FileStates::StatusKind>() : FileStates::StatusKind::Unreadable;
    }
    [[nodiscard]] bool unsaved() const { return role(SessionModel::UnsavedRole).toBool(); }
    [[nodiscard]] bool resolved() const { return role(SessionModel::ResolvedRole).toBool(); }
    [[nodiscard]] int undecided() const { return role(SessionModel::UndecidedRole).toInt(); }
    [[nodiscard]] bool pendingInput() const { return role(SessionModel::PendingInputRole).toBool(); }
    [[nodiscard]] QString pendingText() const { return role(SessionModel::PendingTextRole).toString(); }
    // Durable without a selected file.
    [[nodiscard]] FileStates::RecoveryState recoveryState() const
    {
        const auto state = role(SessionModel::RecoveryStateRole);
        return state.isValid() ? state.value<FileStates::RecoveryState>() : FileStates::RecoveryState::Durable;
    }
    [[nodiscard]] QString recoveryError() const { return role(SessionModel::RecoveryErrorRole).toString(); }
    [[nodiscard]] QString currentRevision() const { return role(SessionModel::CurrentRevisionRole).toString(); }
    [[nodiscard]] bool canUndo() const { return role(SessionModel::CanUndoRole).toBool(); }
    [[nodiscard]] bool canRedo() const { return role(SessionModel::CanRedoRole).toBool(); }
    [[nodiscard]] QVariantMap progress() const { return role(SessionModel::ProgressRole).toMap(); }
    [[nodiscard]] QString statusIcon() const { return role(SessionModel::StatusIconRole).toString(); }
    [[nodiscard]] bool canEdit() const;
Q_SIGNALS:
    void identityChanged();
    void sourcesChanged();
    void resultTextChanged();
    void decisionsChanged();
    void statusChanged();

private:
    [[nodiscard]] QVariant role(int role) const;
    void project(const FileSession *file);
    template <typename T> void update(T &stored, const T &next, void (SelectedFile::*changed)())
    {
        if (stored == next) return;
        stored = next;
        Q_EMIT(this->*changed)();
    }
    ComparisonPresentation comparison_;
    const SessionModel &files_;
    const Selection &selection_;
    const FileSnapshots *sourceIdentity_ = nullptr;
    // The status roles last published, so statusChanged fires only on a change.
    QVariantList status_;
    bool reset_ = true;
    QString projectedRevision_;
    std::shared_ptr<const CorrespondenceMap> projectedCorrespondence_;
    Support projectedSupport_ = Support::Unreadable;
    bool projectedPending_ = false;
    std::map<SourceSide, TextPositions> sourcePositions_;
    QString fileId_;
    QString path_;
    QByteArray pathBytes_;
    QString fileName_;
    QVariantMap stage2_;
    QVariantMap stage3_;
    QVariantMap earlier_;
    QVariantMap initialResult_;
    QString resultText_;
    QVariantList decisions_;
};
}
