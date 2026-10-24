#pragma once
#include "SessionModel.h"
#include "SessionCoordinator.h"
#include "git/ConflictClassifier.h"
#include <QObject>
#include <QString>
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
    Q_PROPERTY(QString stage2 READ stage2 NOTIFY launchChanged)
    Q_PROPERTY(QString stage3 READ stage3 NOTIFY launchChanged)
    Q_PROPERTY(QString workflow READ workflow CONSTANT)
    Q_PROPERTY(QString selectedFileId READ selectedFileId NOTIFY selectedFileChanged)
    Q_PROPERTY(QVariantMap selectedFile READ selectedFile NOTIFY selectedFileChanged)
    Q_PROPERTY(int outsideAppFiles READ outsideAppFiles NOTIFY inventoryChanged)
    Q_PROPERTY(int loadErrorFiles READ loadErrorFiles NOTIFY inventoryChanged)
    Q_PROPERTY(QVariantMap commandPreview READ commandPreview NOTIFY commandChanged)
    Q_PROPERTY(QString commandMessage READ commandMessage NOTIFY commandChanged)
    Q_PROPERTY(QVariantMap nonConflictingPreview READ nonConflictingPreview NOTIFY commandChanged)
    Q_PROPERTY(QVariantMap wholeFilePreview READ wholeFilePreview NOTIFY commandChanged)
    Q_PROPERTY(bool fileListVisible READ fileListVisible NOTIFY fileListChanged)
    Q_PROPERTY(QVariantMap navigationTarget READ navigationTarget NOTIFY navigationChanged)
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
    [[nodiscard]] QString stage2() const;
    [[nodiscard]] QString stage3() const;
    [[nodiscard]] QString workflow() const;
    [[nodiscard]] QString selectedFileId() const { return selectedFileId_; }
    [[nodiscard]] QVariantMap selectedFile() const;
    [[nodiscard]] int outsideAppFiles() const;
    [[nodiscard]] int loadErrorFiles() const;
    [[nodiscard]] QVariantMap commandPreview() const;
    [[nodiscard]] QString commandMessage() const { return commandMessage_; }
    [[nodiscard]] QVariantMap nonConflictingPreview() const;
    Q_INVOKABLE bool previewNonConflicting(const QString &fileId, const QString &revision, const QString &scope);
    Q_INVOKABLE bool applyNonConflicting();
    [[nodiscard]] QVariantMap wholeFilePreview() const;
    [[nodiscard]] bool fileListVisible() const { return fileListVisible_; }
    [[nodiscard]] QVariantMap navigationTarget() const { return navigationTarget_; }
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
    // Discard restores the manual checkpoint, or the initial result, only after that decision is durable.
    Q_INVOKABLE bool discardUnsaved(const QString &fileId, const QString &revision);
    Q_INVOKABLE void attachResultEditor(QObject *editor);
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
    // One launched repository session per controller. Paths are launch directories,
    // never inferred from a filename or a Git environment override.
    bool openRepository(const QString &directory);

Q_SIGNALS:
    void launchChanged();
    void selectedFileChanged();
    void inventoryChanged();
    void commandChanged();
    void fileListChanged();
    void navigationChanged();

private:
    bool commandReady(const QString &fileId, const QString &revision, RevisionId *expected) const;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void classified(const git::ConflictClassificationResult &result);
    void loadWorkingFiles(QList<git::Conflict> conflicts);
    void setState(QString state, QString detail);
    bool commandResult(Transition result);
    void captureResultEdit();
    void syncResultEditor();
    void noteEditorLocation();
    void restoreEditorLocation();
    void clearNavigation();
    bool navigateWithinFile(const QString &mode, int direction);
    bool navigateAcrossFiles(const QString &mode, int direction);
    SessionModel files_;
    SessionCoordinator coordinator_;
    git::ConflictClassifier classifier_;
    git::RepositoryContext repository_;
    QString state_ = QStringLiteral("idle");
    QString operation_;
    QString detail_;
    QString selectedFileId_;
    std::optional<ChoicePreview> preview_;
    std::optional<NonConflictingPreview> nonConflictingPreview_;
    std::optional<WholeFilePreview> wholeFilePreview_;
    QString previewFileId_;
    QString commandMessage_;
    QPointer<QObject> resultEditor_;
    QPointer<QTextDocument> resultDocument_;
    QMetaObject::Connection editorConnection_;
    QString editorText_;
    bool syncingEditor_ = false;
    bool processingEditorEvent_ = false;
    bool fileListVisible_ = true;
    bool selectingFromNavigation_ = false;
    QVariantMap navigationTarget_;
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
};
}
