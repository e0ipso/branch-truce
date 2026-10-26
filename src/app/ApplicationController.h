#pragma once
#include "MergetoolSession.h"
#include "RecoveryRestore.h"
#include "ResolutionPublisher.h"
#include "SessionModel.h"
#include "SessionCoordinator.h"
#include "git/ConflictClassifier.h"
#include <QObject>
#include <QString>
#include <QStringList>
#include <QPointer>
#include <string>

class QTextDocument;

namespace branch_truce {
class ApplicationController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(branch_truce::SessionModel *files READ files CONSTANT)
    Q_PROPERTY(branch_truce::SessionCoordinator *coordinator READ coordinator CONSTANT)
    Q_PROPERTY(QString startupState READ startupState NOTIFY launchChanged)
    Q_PROPERTY(bool opening READ opening NOTIFY launchChanged)
    Q_PROPERTY(QString repositoryRoot READ repositoryRoot NOTIFY launchChanged)
    Q_PROPERTY(QString operation READ operation NOTIFY launchChanged)
    Q_PROPERTY(QString detail READ detail NOTIFY launchChanged)
    Q_PROPERTY(QVariantList recoveryChoices READ recoveryChoices NOTIFY launchChanged)
    Q_PROPERTY(QString stage2 READ stage2 NOTIFY launchChanged)
    Q_PROPERTY(QString stage3 READ stage3 NOTIFY launchChanged)
    Q_PROPERTY(QString stage2CommitId READ stage2CommitId NOTIFY launchChanged)
    Q_PROPERTY(QString stage3CommitId READ stage3CommitId NOTIFY launchChanged)
    Q_PROPERTY(QString workflow READ workflow CONSTANT)
    Q_PROPERTY(QString selectedFileId READ selectedFileId NOTIFY selectedFileChanged)
    Q_PROPERTY(QVariantMap selectedFile READ selectedFile NOTIFY selectedFileChanged)
    Q_PROPERTY(int outsideAppFiles READ outsideAppFiles NOTIFY inventoryChanged)
    Q_PROPERTY(int loadErrorFiles READ loadErrorFiles NOTIFY inventoryChanged)
    Q_PROPERTY(QVariantMap commandPreview READ commandPreview NOTIFY commandChanged)
    Q_PROPERTY(QString commandMessage READ commandMessage NOTIFY commandChanged)
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
    [[nodiscard]] QString startupState() const { return state_; }
    [[nodiscard]] bool opening() const { return state_ == QLatin1String("opening"); }
    [[nodiscard]] QString repositoryRoot() const { return repository_.worktreeRoot; }
    [[nodiscard]] QString operation() const { return operation_; }
    [[nodiscard]] QString detail() const { return detail_; }
    // Matching recovery sessions with app edits that blocked restoration at
    // this load: session, lastChanged and the display paths it edited.
    [[nodiscard]] QVariantList recoveryChoices() const;
    // Each source's label, which already names its commit.
    [[nodiscard]] QString stage2() const;
    [[nodiscard]] QString stage3() const;
    [[nodiscard]] QString stage2CommitId() const;
    [[nodiscard]] QString stage3CommitId() const;
    [[nodiscard]] QString workflow() const;
    [[nodiscard]] QString selectedFileId() const { return selectedFileId_; }
    [[nodiscard]] QVariantMap selectedFile() const;
    [[nodiscard]] int outsideAppFiles() const;
    [[nodiscard]] int loadErrorFiles() const;
    [[nodiscard]] QVariantMap commandPreview() const;
    [[nodiscard]] QString commandMessage() const { return commandMessage_; }
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
    [[nodiscard]] QVariantMap editorPlacement() const { return editorPlacement_; }
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
    Q_INVOKABLE void attachResultEditor(QObject *editor);
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
    // One launched repository session per controller. Paths are launch directories,
    // never inferred from a filename or a Git environment override.
    // preferredFileId selects that file first when the session contains it.
    // recoverySession names the recovery session the user chose to restore when
    // several match; the other matching sessions are set aside.
    bool openRepository(const QString &directory, const QString &preferredFileId = {},
                        const QString &recoverySession = {});
    // Reloads the session with one of recoveryChoices, through the same startup
    // restore as --recovery-session: it is adopted and the other matching
    // sessions are set aside, not deleted. Refused while another window holds
    // one of the choices, or while this window's own session holds app edits,
    // pending input or a running resolution that a reload would leave behind.
    Q_INVOKABLE bool chooseRecoverySession(const QString &session);

Q_SIGNALS:
    void launchChanged();
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
    // Escaped display text for exact path bytes; see displayPath.
    [[nodiscard]] QString displayBytes(const std::string &path) const;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void classified(const git::ConflictClassificationResult &result);
    void loadWorkingFiles(QList<git::Conflict> conflicts);
    [[nodiscard]] bool launchedWithCapturedBytes(const FileSession &file) const;
    void settleCopiedLaunch(const std::string &fileId, const SnapshotFileIdentity &identity, const QString &failure);
    void setState(QString state, QString detail);
    bool commandResult(Transition result);
    void captureResultEdit();
    void syncResultEditor();
    void placeEditor(const FileSession *file, const QString &loaded);
    void applyVisibleWhitespace(QTextDocument *document);
    void noteEditorLocation();
    void restoreEditorLocation();
    void clearNavigation();
    bool navigateWithinFile(const QString &mode, int direction);
    bool navigateAcrossFiles(const QString &mode, int direction);
    SessionModel files_;
    SessionCoordinator coordinator_;
    ResolutionPublisher publisher_;
    git::ConflictClassifier classifier_;
    MergetoolSession mergetool_;
    git::RepositoryContext repository_;
    QString state_ = QStringLiteral("idle");
    QString operation_;
    QString detail_;
    QString selectedFileId_;
    QString preferredFileId_;
    QString recoverySession_;
    std::vector<RecoveryChoice> recoveryChoices_;
    // Restored files that only a waiting launcher can clear; see RecoveryRestore.
    std::map<std::string, SnapshotFileIdentity> copiedLaunches_;
    // The revalidation task in flight for a copiedLaunches_ entry.
    std::map<std::string, TaskId> copiedLaunchChecks_;
    std::optional<ChoicePreview> preview_;
    std::optional<NonConflictingPreview> nonConflictingPreview_;
    std::optional<WholeFilePreview> wholeFilePreview_;
    QString previewFileId_;
    QString commandMessage_;
    bool discardPending_ = false;
    QPointer<QObject> resultEditor_;
    QPointer<QTextDocument> resultDocument_;
    QMetaObject::Connection editorConnection_;
    QString editorText_;
    // The file the editor last placed, and whether it was supported then. A
    // file opens as an unsupported placeholder until its captures load.
    QString editorFileId_;
    bool editorSupported_ = false;
    QVariantMap editorPlacement_;
    bool syncingEditor_ = false;
    bool processingEditorEvent_ = false;
    bool showWhitespace_ = false;
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
    struct EditorLocation {
        bool valid = false;
        QString fileId;
        QString text;
        int cursor = 0;
        int selectionStart = 0;
        int selectionEnd = 0;
        qreal contentX = 0;
        qreal contentY = 0;
        bool hasViewport = false;
    };
    EditorLocation heldLocation_;
    [[nodiscard]] EditorLocation editorLocation() const;
    void keepEditorPlace(const EditorLocation &place, const QString &before, const QString &after);
};
}
