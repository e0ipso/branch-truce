#pragma once
#include "SessionModel.h"
#include "SessionCoordinator.h"
#include "git/ConflictClassifier.h"
#include <QObject>
#include <QString>
#include <QPointer>

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
    Q_INVOKABLE bool previewChoice(const QString &fileId, const QString &revision,
                                  const QString &target, const QString &action);
    Q_INVOKABLE bool applyChoice(bool confirmReplacement = false);
    Q_INVOKABLE void cancelChoice();
    // Editor offsets are UTF-16 units and must identify scalar boundaries.
    Q_INVOKABLE bool editResult(const QString &fileId, const QString &revision,
                               int begin, int end, const QString &replacement);
    Q_INVOKABLE bool undo(const QString &fileId, const QString &revision);
    Q_INVOKABLE bool redo(const QString &fileId, const QString &revision);
    Q_INVOKABLE void attachResultEditor(QObject *editor);
    // Select by the exact byte-derived file ID, never by a display path.
    Q_INVOKABLE bool selectFile(const QString &fileId);
    Q_INVOKABLE QString displayPath(const QString &fileId) const;
    // One launched repository session per controller. Paths are launch directories,
    // never inferred from a filename or a Git environment override.
    bool openRepository(const QString &directory);

Q_SIGNALS:
    void launchChanged();
    void selectedFileChanged();
    void inventoryChanged();
    void commandChanged();

private:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void classified(const git::ConflictClassificationResult &result);
    void loadWorkingFiles(QList<git::Conflict> conflicts);
    void setState(QString state, QString detail);
    bool commandResult(Transition result);
    void syncResultEditor();
    SessionModel files_;
    SessionCoordinator coordinator_;
    git::ConflictClassifier classifier_;
    git::RepositoryContext repository_;
    QString state_ = QStringLiteral("idle");
    QString operation_;
    QString detail_;
    QString selectedFileId_;
    std::optional<ChoicePreview> preview_;
    QString previewFileId_;
    QString commandMessage_;
    QPointer<QObject> resultEditor_;
    QPointer<QTextDocument> resultDocument_;
    QMetaObject::Connection editorConnection_;
    QString editorText_;
    bool syncingEditor_ = false;
};
}
