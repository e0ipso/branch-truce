#pragma once
#include "MergetoolSession.h"
#include "RecoveryRestore.h"
#include "ResolutionPublisher.h"
#include "ResultEditorAdapter.h"
#include "SessionModel.h"
#include "SessionCoordinator.h"
#include "SessionLoader.h"
#include <QObject>
#include <QString>
#include <QStringList>
#include <string>

namespace branch_truce {
class ApplicationController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(branch_truce::SessionModel *files READ files CONSTANT)
    Q_PROPERTY(branch_truce::SessionCoordinator *coordinator READ coordinator CONSTANT)
    Q_PROPERTY(branch_truce::SessionLoader *loader READ loader CONSTANT)
    Q_PROPERTY(QString selectedFileId READ selectedFileId NOTIFY selectedFileChanged)
    Q_PROPERTY(QVariantMap selectedFile READ selectedFile NOTIFY selectedFileChanged)
    Q_PROPERTY(int outsideAppFiles READ outsideAppFiles NOTIFY inventoryChanged)
    Q_PROPERTY(int loadErrorFiles READ loadErrorFiles NOTIFY inventoryChanged)
    Q_PROPERTY(QVariantMap commandPreview READ commandPreview NOTIFY commandChanged)
    Q_PROPERTY(QString commandMessage READ commandMessage NOTIFY commandChanged)
    Q_PROPERTY(QVariantMap markerBlocker READ markerBlocker NOTIFY commandChanged)
    Q_PROPERTY(bool resolutionConfirmation READ resolutionConfirmation NOTIFY commandChanged)
    Q_PROPERTY(QString resolutionFileId READ resolutionFileId NOTIFY commandChanged)
    Q_PROPERTY(QString resolutionRevision READ resolutionRevision NOTIFY commandChanged)
    Q_PROPERTY(bool resolutionBusy READ resolutionBusy NOTIFY commandChanged)
    Q_PROPERTY(QString parentStagingPath READ parentStagingPath NOTIFY commandChanged)
    Q_PROPERTY(bool mergetoolWaiting READ mergetoolWaiting NOTIFY commandChanged)
    Q_PROPERTY(bool parentSessionActive READ parentSessionActive NOTIFY commandChanged)
    Q_PROPERTY(bool canEndParentSession READ canEndParentSession NOTIFY commandChanged)
    Q_PROPERTY(QVariantMap nonConflictingPreview READ nonConflictingPreview NOTIFY commandChanged)
    Q_PROPERTY(QVariantMap wholeFilePreview READ wholeFilePreview NOTIFY commandChanged)
    Q_PROPERTY(bool fileListVisible READ fileListVisible NOTIFY fileListChanged)
    Q_PROPERTY(QVariantMap navigationTarget READ navigationTarget NOTIFY navigationChanged)
    Q_PROPERTY(QVariantMap editorPlacement READ editorPlacement NOTIFY editorPlaced)
    Q_PROPERTY(QVariantMap handoff READ handoff NOTIFY handoffChanged)
    Q_PROPERTY(QString sessionOutcome READ sessionOutcome NOTIFY handoffChanged)
public:
    explicit ApplicationController(QObject *parent = nullptr);
    [[nodiscard]] QString version() const;
    [[nodiscard]] SessionModel *files() noexcept { return &files_; }
    [[nodiscard]] SessionCoordinator *coordinator() noexcept { return &coordinator_; }
    // Opens the repository and owns the launch state.
    [[nodiscard]] SessionLoader *loader() noexcept { return &loader_; }
    [[nodiscard]] QString selectedFileId() const { return selectedFileId_; }
    [[nodiscard]] QVariantMap selectedFile() const;
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
    Q_INVOKABLE bool previewNonConflicting(const QString &fileId, const QString &revision, const QString &scope);
    Q_INVOKABLE bool applyNonConflicting();
    [[nodiscard]] QVariantMap wholeFilePreview() const;
    [[nodiscard]] bool fileListVisible() const { return fileListVisible_; }
    [[nodiscard]] QVariantMap navigationTarget() const { return navigationTarget_; }
    // Where the editor put a newly selected file: its first undecided region,
    // in the keys of navigationTarget, with count 0 when it has none.
    [[nodiscard]] QVariantMap editorPlacement() const { return editor_.placement(); }
    Q_INVOKABLE bool previewWholeFile(const QString &fileId, const QString &revision, const QString &side);
    Q_INVOKABLE bool applyWholeFile();
    Q_INVOKABLE bool previewChoice(const QString &fileId, const QString &revision,
                                  const QString &target, const QString &action);
    Q_INVOKABLE bool applyChoice(bool confirmReplacement = false);
    Q_INVOKABLE void cancelChoice();
    // Editor offsets are UTF-16 units and must identify scalar boundaries.
    Q_INVOKABLE bool editResult(const QString &fileId, const QString &revision,
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
    // Clears the recovery error of a restored file that git mergetool replaced
    // by a same-byte copy, once a launcher waits on its path with a MERGED copy
    // of the launch bytes, the file still has the identity recovery saw, and a
    // background check finds its index entries and conversion policy unchanged.
    void acceptCopiedLaunches();
    [[nodiscard]] std::optional<ResolutionPublisher::Offer> parentStagingOffer(const QString &fileId) const;
    [[nodiscard]] bool mergetoolListening() const;
    [[nodiscard]] QByteArray mergetoolWaitingPath() const;
    [[nodiscard]] bool mergetoolHasWaiter(const QByteArray &path) const;
    [[nodiscard]] QStringList mergetoolIntakes() const;
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
    // Space and tab markers are paint options. They never enter the draft, history or progress.
    Q_INVOKABLE void setVisibleWhitespace(QObject *editor, bool visible);
    Q_INVOKABLE bool resultClipboard(const QString &action);
    // Select by the exact byte-derived file ID, never by a display path.
    Q_INVOKABLE bool selectFile(const QString &fileId);
    Q_INVOKABLE int selectedRow() const;
    Q_INVOKABLE QString displayPath(const QString &fileId) const;
    // Hide or show the file list without changing the selected file, editor
    // location or unsaved result.
    Q_INVOKABLE bool toggleFileList();
    // mode is difference, undecided, unfinished or saved-or-reviewed.
    // direction is -1 or 1. Difference and undecided stay inside the selected
    // file; the file modes walk the session inventory.
    Q_INVOKABLE bool navigate(const QString &mode, int direction);
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
    void commandChanged();
    void fileListChanged();
    void navigationChanged();
    // The editor loaded a newly selected file and moved its cursor to the start
    // of editorPlacement, so the view can scroll the panes there.
    void editorPlaced();
    void handoffChanged();
    // Emitted once by finishSession or leaveSession; the outcome text is final.
    void sessionEnded(const QString &outcome, bool clean);

private:
    bool commandReady(const QString &fileId, const QString &revision, RevisionId *expected) const;
    [[nodiscard]] QString discardedMessage(const FileSession *current) const;
    [[nodiscard]] static QString stagedDiscardMessage();
    // Escaped display text for exact path bytes; see displayPath.
    [[nodiscard]] QString displayBytes(const std::string &path) const;
    [[nodiscard]] bool launchedWithCapturedBytes(const FileSession &file) const;
    void settleCopiedLaunch(const std::string &fileId, const SnapshotFileIdentity &identity, const QString &failure);
    bool commandResult(Transition result);
    [[nodiscard]] QVariantMap firstUndecidedPlacement(const FileSession *file, const QString &loaded) const;
    void clearNavigation();
    bool navigateWithinFile(const QString &mode, int direction);
    bool navigateAcrossFiles(const QString &mode, int direction);
    SessionModel files_;
    SessionCoordinator coordinator_;
    ResolutionPublisher publisher_;
    MergetoolSession mergetool_;
    ResultEditorAdapter editor_;
    SessionLoader loader_;
    QString selectedFileId_;
    // Restored files that only a waiting launcher can clear; see RecoveryRestore.
    std::map<std::string, SnapshotFileIdentity> copiedLaunches_;
    // The revalidation task in flight for a copiedLaunches_ entry.
    std::map<std::string, TaskId> copiedLaunchChecks_;
    std::optional<ChoicePreview> preview_;
    std::optional<NonConflictingPreview> nonConflictingPreview_;
    std::optional<WholeFilePreview> wholeFilePreview_;
    QString previewFileId_;
    QString commandMessage_;
    QVariantMap markerBlocker_;
    bool discardPending_ = false;
    bool stagedDiscardPending_ = false;
    bool fileListVisible_ = true;
    bool selectingFromNavigation_ = false;
    QVariantMap navigationTarget_;
    QString sessionOutcome_;
    QString navMode_;
    QString navFileId_;
    RevisionId navRevision_ = 0;
    int navIndex_ = -1;
    int navCursor_ = -1;
    int navSelectionStart_ = -1;
    int navSelectionEnd_ = -1;
    std::size_t navByteBegin_ = 0;
    std::size_t navByteEnd_ = 0;
    std::string navContribution_;
    bool navResultRange_ = false;
};
}
