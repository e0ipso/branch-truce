// SelectedFile exposes the selected row and caches independently notified comparison data.
#pragma once
#include "SessionModel.h"
#include "TextPositions.h"
#include <QtQml/qqmlregistration.h>
#include <map>

namespace branch_truce {
class SelectedFile : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by ApplicationController")
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
    Q_PROPERTY(QString resultText READ resultText NOTIFY resultTextChanged)
    Q_PROPERTY(QVariantList decisions READ decisions NOTIFY decisionsChanged)
    Q_PROPERTY(QVariantList passages READ passages NOTIFY passagesChanged)
    Q_PROPERTY(QVariantList handEditedLines READ handEditedLines NOTIFY handEditedLinesChanged)
    Q_PROPERTY(QVariantList choices READ choices NOTIFY choicesChanged)
    Q_PROPERTY(QString choiceNotice READ choiceNotice NOTIFY choicesChanged)
    Q_PROPERTY(QVariantList regions READ regions NOTIFY regionsChanged)
    Q_PROPERTY(QVariantList markerBlocks READ markerBlocks NOTIFY markerBlocksChanged)
    Q_PROPERTY(QVariantList markerOrphans READ markerOrphans NOTIFY markerOrphansChanged)
    Q_PROPERTY(QString support READ support NOTIFY statusChanged)
    Q_PROPERTY(QString supportDetail READ supportDetail NOTIFY statusChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool unsaved READ unsaved NOTIFY statusChanged)
    Q_PROPERTY(bool reviewNeeded READ reviewNeeded NOTIFY statusChanged)
    Q_PROPERTY(bool resolved READ resolved NOTIFY statusChanged)
    Q_PROPERTY(int undecided READ undecided NOTIFY statusChanged)
    Q_PROPERTY(bool pendingInput READ pendingInput NOTIFY statusChanged)
    Q_PROPERTY(QString pendingText READ pendingText NOTIFY statusChanged)
    Q_PROPERTY(QString recoveryState READ recoveryState NOTIFY statusChanged)
    Q_PROPERTY(QString recoveryError READ recoveryError NOTIFY statusChanged)
    Q_PROPERTY(QStringList finishBlockers READ finishBlockers NOTIFY statusChanged)
    Q_PROPERTY(QString currentRevision READ currentRevision NOTIFY statusChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY statusChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY statusChanged)
    Q_PROPERTY(QVariantMap progress READ progress NOTIFY statusChanged)
    Q_PROPERTY(QString statusIcon READ statusIcon NOTIFY statusChanged)
    Q_PROPERTY(QString statusLine READ statusLine NOTIFY statusChanged)
    Q_PROPERTY(QVariantList chips READ chips NOTIFY statusChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY statusChanged)
    Q_PROPERTY(bool canEdit READ canEdit NOTIFY statusChanged)
public:
    explicit SelectedFile(SessionModel &files, QObject *parent = nullptr);
    void sync(const QString &fileId);
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
    [[nodiscard]] QVariantList passages() const { return passages_; }
    [[nodiscard]] QVariantList handEditedLines() const { return handEditedLines_; }
    [[nodiscard]] QVariantList choices() const { return choices_; }
    [[nodiscard]] QString choiceNotice() const { return choiceNotice_; }
    [[nodiscard]] QVariantList regions() const { return regions_; }
    [[nodiscard]] QVariantList markerBlocks() const { return markerBlocks_; }
    [[nodiscard]] QVariantList markerOrphans() const { return markerOrphans_; }
    [[nodiscard]] QString support() const { return role(SessionModel::SupportRole).toString(); }
    [[nodiscard]] QString supportDetail() const { return role(SessionModel::SupportDetailRole).toString(); }
    [[nodiscard]] QString status() const { return role(SessionModel::StatusRole).toString(); }
    [[nodiscard]] bool unsaved() const { return role(SessionModel::UnsavedRole).toBool(); }
    [[nodiscard]] bool reviewNeeded() const { return role(SessionModel::ReviewNeededRole).toBool(); }
    [[nodiscard]] bool resolved() const { return role(SessionModel::ResolvedRole).toBool(); }
    [[nodiscard]] int undecided() const { return role(SessionModel::UndecidedRole).toInt(); }
    [[nodiscard]] bool pendingInput() const { return role(SessionModel::PendingInputRole).toBool(); }
    [[nodiscard]] QString pendingText() const { return role(SessionModel::PendingTextRole).toString(); }
    [[nodiscard]] QString recoveryState() const { return role(SessionModel::RecoveryStateRole).toString(); }
    [[nodiscard]] QString recoveryError() const { return role(SessionModel::RecoveryErrorRole).toString(); }
    [[nodiscard]] QStringList finishBlockers() const { return role(SessionModel::FinishBlockersRole).toStringList(); }
    [[nodiscard]] QString currentRevision() const { return role(SessionModel::CurrentRevisionRole).toString(); }
    [[nodiscard]] bool canUndo() const { return role(SessionModel::CanUndoRole).toBool(); }
    [[nodiscard]] bool canRedo() const { return role(SessionModel::CanRedoRole).toBool(); }
    [[nodiscard]] QVariantMap progress() const { return role(SessionModel::ProgressRole).toMap(); }
    [[nodiscard]] QString statusIcon() const { return role(SessionModel::StatusIconRole).toString(); }
    [[nodiscard]] QString statusLine() const { return role(SessionModel::StatusLineRole).toString(); }
    [[nodiscard]] QVariantList chips() const { return role(SessionModel::ChipsRole).toList(); }
    [[nodiscard]] QString summary() const { return role(SessionModel::SummaryRole).toString(); }
    [[nodiscard]] bool canEdit() const;
Q_SIGNALS:
    void identityChanged();
    void sourcesChanged();
    void resultTextChanged();
    void decisionsChanged();
    void passagesChanged();
    void handEditedLinesChanged();
    void choicesChanged();
    void regionsChanged();
    void markerBlocksChanged();
    void markerOrphansChanged();
    void statusChanged();
private:
    [[nodiscard]] QVariant role(int role) const;
    void project(const FileSession *file);
    template<typename T> void update(T &stored, const T &next, void (SelectedFile::*changed)())
    {
        if (stored == next) return;
        stored = next;
        Q_EMIT (this->*changed)();
    }
    SessionModel &files_;
    const FileSnapshots *sourceIdentity_ = nullptr;
    bool reset_ = true;
    QString projectedRevision_;
    Support projectedSupport_ = Support::Unreadable;
    bool projectedPending_ = false;
    std::map<SourceSide, std::string> sourceTexts_;
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
    QVariantList passages_;
    QVariantList handEditedLines_;
    QVariantList choices_;
    QString choiceNotice_;
    QVariantList regions_;
    QVariantList markerBlocks_;
    QVariantList markerOrphans_;
};
}
