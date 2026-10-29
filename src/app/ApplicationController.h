#pragma once
#include <QtQml/qqmlregistration.h>
#include "ChoicePreviewFlow.h"
#include "CommandGuard.h"
#include "FileNavigator.h"
#include "mergetool/MergetoolSession.h"
#include "recovery/RecoveryRestore.h"
#include "publication/ResolutionPublisher.h"
#include "ResultEditorAdapter.h"
#include "SessionModel.h"
#include "SelectedFile.h"
#include "session/SessionCoordinator.h"
#include "SessionLoader.h"
#include <QByteArray>
#include <QColor>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <optional>
#include <string>

namespace branch_truce {
class ApplicationController final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by ApplicationController")
    Q_PROPERTY(branch_truce::SessionModel *files READ files CONSTANT)
    Q_PROPERTY(branch_truce::SessionLoader *loader READ loader CONSTANT)
    Q_PROPERTY(QString selectedFileId READ selectedFileId NOTIFY selectedFileChanged)
    Q_PROPERTY(branch_truce::SelectedFile *selectedFile READ selectedFile CONSTANT)
    Q_PROPERTY(int outsideAppFiles READ outsideAppFiles NOTIFY inventoryChanged)
    Q_PROPERTY(int loadErrorFiles READ loadErrorFiles NOTIFY inventoryChanged)
    Q_PROPERTY(QVariantMap commandPreview READ commandPreview NOTIFY commandPreviewChanged)
    Q_PROPERTY(QString commandMessage READ commandMessage NOTIFY messageChanged)
    Q_PROPERTY(QVariantMap markerBlocker READ markerBlocker NOTIFY messageChanged)
    Q_PROPERTY(bool resolutionConfirmation READ resolutionConfirmation NOTIFY resolutionChanged)
    Q_PROPERTY(QString resolutionFileId READ resolutionFileId NOTIFY resolutionChanged)
    Q_PROPERTY(QString resolutionRevision READ resolutionRevision NOTIFY resolutionChanged)
    Q_PROPERTY(bool resolutionBusy READ resolutionBusy NOTIFY resolutionChanged)
    Q_PROPERTY(QString parentStagingPath READ parentStagingPath NOTIFY mergetoolChanged)
    Q_PROPERTY(bool mergetoolWaiting READ mergetoolWaiting NOTIFY mergetoolChanged)
    Q_PROPERTY(bool parentSessionActive READ parentSessionActive NOTIFY mergetoolChanged)
    Q_PROPERTY(bool canEndParentSession READ canEndParentSession NOTIFY mergetoolChanged)
    Q_PROPERTY(QVariantMap nonConflictingPreview READ nonConflictingPreview NOTIFY nonConflictingPreviewChanged)
    Q_PROPERTY(QVariantMap wholeFilePreview READ wholeFilePreview NOTIFY wholeFilePreviewChanged)
    Q_PROPERTY(bool fileListVisible READ fileListVisible NOTIFY fileListChanged)
    Q_PROPERTY(QVariantMap navigationTarget READ navigationTarget NOTIFY navigationChanged)
    Q_PROPERTY(QVariantMap editorPlacement READ editorPlacement NOTIFY editorPlaced)
    Q_PROPERTY(QVariantMap handoff READ handoff NOTIFY handoffChanged)
    Q_PROPERTY(QString sessionOutcome READ sessionOutcome NOTIFY handoffChanged)
    Q_PROPERTY(bool resultEditable READ resultEditable NOTIFY resultEditorChanged)
    Q_PROPERTY(bool resultHasSelection READ resultHasSelection NOTIFY resultEditorChanged)
    Q_PROPERTY(bool resultCanPaste READ resultCanPaste NOTIFY resultEditorChanged)
public:
    // gitExecutable is the Git every collaborator and worker task runs; main
    // resolves it once. Empty when Git is not on PATH.
    explicit ApplicationController(QString gitExecutable, QObject *parent = nullptr);
    [[nodiscard]] QString version() const;
    [[nodiscard]] SessionModel *files() noexcept { return &files_; }
    [[nodiscard]] SessionCoordinator *coordinator() noexcept { return &coordinator_; }
    // Opens the repository and owns the launch state.
    [[nodiscard]] SessionLoader *loader() noexcept { return &loader_; }
    // Waiting mergetool launchers and the copied launch paths they can clear.
    [[nodiscard]] MergetoolSession *mergetool() noexcept { return &mergetool_; }
    [[nodiscard]] QString selectedFileId() const { return selectedFileId_; }
    [[nodiscard]] SelectedFile *selectedFile() noexcept { return &selectedFile_; }
    [[nodiscard]] int outsideAppFiles() const;
    [[nodiscard]] int loadErrorFiles() const;
    [[nodiscard]] QVariantMap commandPreview() const;
    [[nodiscard]] QString commandMessage() const { return commandMessage_; }
    // The markers that refused Mark resolved, while that message is shown for
    // the selected file's current revision: fileId, revision, count and the
    // one-based line of the first marker. Empty otherwise.
    [[nodiscard]] QVariantMap markerBlocker() const;
    [[nodiscard]] bool resolutionConfirmation() const { return publisher_.confirmationPending(); }
    [[nodiscard]] QString resolutionFileId() const { return publisher_.confirmationFileId(); }
    [[nodiscard]] QString resolutionRevision() const { return publisher_.confirmationRevision(); }
    [[nodiscard]] bool resolutionBusy() const { return publisher_.busy(); }
    [[nodiscard]] QString parentStagingPath() const;
    [[nodiscard]] bool mergetoolWaiting() const;
    [[nodiscard]] bool parentSessionActive() const;
    [[nodiscard]] bool canEndParentSession() const;
    [[nodiscard]] QVariantMap nonConflictingPreview() const;
    Q_INVOKABLE bool previewNonConflicting(const QString &fileId, const QString &revision, ChoicePreviewFlow::NonConflicting scope);
    Q_INVOKABLE bool applyNonConflicting();
    [[nodiscard]] QVariantMap wholeFilePreview() const;
    [[nodiscard]] bool fileListVisible() const { return fileListVisible_; }
    [[nodiscard]] QVariantMap navigationTarget() const { return navigator_.target(); }
    // Where the editor put a newly selected file: its first undecided region,
    // in the keys of navigationTarget, with count 0 when it has none.
    [[nodiscard]] QVariantMap editorPlacement() const { return editor_.placement(); }
    // Whether the result editor takes edits; its readOnly binds to the inverse.
    [[nodiscard]] bool resultEditable() const { return editor_.editable(); }
    [[nodiscard]] bool resultHasSelection() const { return editor_.hasSelection(); }
    [[nodiscard]] bool resultCanPaste() const { return editor_.canPaste(); }
    Q_INVOKABLE bool previewWholeFile(const QString &fileId, const QString &revision, ChoicePreviewFlow::Side side);
    Q_INVOKABLE bool applyWholeFile();
    Q_INVOKABLE bool previewChoice(const QString &fileId, const QString &revision,
                                  const QString &target, ChoicePreviewFlow::Action action);
    Q_INVOKABLE bool applyChoice(bool confirmReplacement = false);
    Q_INVOKABLE void cancelChoice();
    // Editor offsets are UTF-16 units and must identify scalar boundaries.
    bool editResult(const QString &fileId, const QString &revision,
                               int begin, int end, const QString &replacement);
    Q_INVOKABLE bool undo(const QString &fileId, const QString &revision);
    Q_INVOKABLE bool redo(const QString &fileId, const QString &revision);
    // Save checkpoints the current text and decisions in the app store only.
    Q_INVOKABLE bool saveDraft(const QString &fileId, const QString &revision);
    // Review, checkpoint and publish one exact revision. Confirmation is required while source choices remain undecided.
    Q_INVOKABLE bool markResolved(const QString &fileId, const QString &revision, bool confirmUndecided = false);
    Q_INVOKABLE void cancelResolution();
    // Moves the result cursor to the first marker named by markerBlocker.
    Q_INVOKABLE bool showFirstMarker();
    // The launcher assigns one parent-owned path. Repository mode leaves this empty.
    bool assignParentStaging(const QByteArray &path);
    bool verifyParentAcknowledgment(const QString &fileId, const QString &revision);
    [[nodiscard]] std::optional<ResolutionPublisher::Offer> parentStagingOffer(const QString &fileId) const;
    Q_INVOKABLE bool leaveMergetool();
    Q_INVOKABLE bool endParentSession();
    Q_INVOKABLE bool recheckParentAcknowledgment();
    // Discard restores the manual checkpoint, or the initial result, only after that decision is durable.
    Q_INVOKABLE bool discardUnsaved(const QString &fileId, const QString &revision);
    // Drops a draft newer than the result staged in Git and makes that staged result current again.
    Q_INVOKABLE bool discardToStaged(const QString &fileId, const QString &revision);
    Q_INVOKABLE void attachResultEditor(QObject *editor);
    // Highlights a read-only source pane with the selected file's syntax.
    Q_INVOKABLE void attachSourceEditor(QObject *editor);
    // The Kirigami colour scheme background; its lightness picks the syntax theme.
    Q_INVOKABLE void setSyntaxBackground(const QColor &background);
    Q_INVOKABLE bool resultClipboard(ResultEditorAdapter::ClipboardAction action);
    // Select by the exact byte-derived file ID, never by a display path.
    Q_INVOKABLE bool selectFile(const QString &fileId);
    Q_INVOKABLE int selectedRow() const;
    // Hide or show the file list without changing the selected file, editor
    // location or unsaved result.
    Q_INVOKABLE bool toggleFileList();
    // direction is -1 or 1. Difference and undecided stay inside the selected
    // file; the file modes walk the session inventory.
    Q_INVOKABLE bool navigate(FileNavigator::Mode mode, int direction);
    // The file handoff plus session-level blockers (running resolution,
    // launched paths Git is still waiting on). clean is the only success claim.
    [[nodiscard]] QVariantMap handoff() const;
    [[nodiscard]] QString sessionOutcome() const { return sessionOutcome_; }
    // Ends the session only when the handoff is clean.
    Q_INVOKABLE bool finishSession();
    // Deliberate leave: keeps drafts in the recovery store, tells waiting
    // launchers to leave their paths unresolved and runs no Git command.
    // Refused while pending input or a running resolution could be lost.
    Q_INVOKABLE bool leaveSession();

Q_SIGNALS:
    void selectedFileChanged();
    void inventoryChanged();
    // The command message and the marker blocker. The blocker also follows the
    // selected file and its revision, so a selection change emits this too.
    void messageChanged();
    // One per pending preview: commandPreview, nonConflictingPreview and wholeFilePreview.
    void commandPreviewChanged();
    void nonConflictingPreviewChanged();
    void wholeFilePreviewChanged();
    // A resolution started, finished or asked for confirmation.
    void resolutionChanged();
    // Waiting launchers, parent staging and the parent session. The waiting
    // state and the staging path follow the selection, so a selection change
    // emits this too.
    void mergetoolChanged();
    void fileListChanged();
    void navigationChanged();
    // The editor loaded a newly selected file and moved its cursor to the start
    // of editorPlacement, so the view can scroll the panes there.
    void editorPlaced();
    // resultEditable, resultHasSelection or resultCanPaste may have changed.
    void resultEditorChanged();
    // A native editor key requests the same action used by menus and the toolbar.
    void resultEditActionRequested(const QString &name);
    void handoffChanged();
    // Emitted once by finishSession or leaveSession; the outcome text is final.
    void sessionEnded(const QString &outcome, bool clean);

private:
    // The command guard for the current revision of a supported file. Shows a refusal and returns nothing.
    std::optional<CommandTarget> admitCommand(const QString &fileId, const QString &revision);
    [[nodiscard]] QString discardedMessage(const FileSession *current) const;
    [[nodiscard]] static QString stagedDiscardMessage();
    bool commandResult(Transition result);
    void showMessage(const QString &message);
    [[nodiscard]] QVariantMap firstUndecidedPlacement(const FileSession *file, const QString &loaded) const;
    SessionModel files_;
    SelectedFile selectedFile_;
    SessionCoordinator coordinator_;
    ResolutionPublisher publisher_;
    MergetoolSession mergetool_;
    ResultEditorAdapter editor_;
    SessionLoader loader_;
    FileNavigator navigator_;
    ChoicePreviewFlow choices_;
    QString selectedFileId_;
    QString commandMessage_;
    QVariantMap markerBlocker_;
    bool discardPending_ = false;
    bool stagedDiscardPending_ = false;
    bool fileListVisible_ = true;
    QString sessionOutcome_;
};
}
