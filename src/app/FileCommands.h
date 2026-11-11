// File commands change the selected file's result, Save it, discard it or Mark it resolved.
#pragma once
#include "CommandGuard.h"
#include "core/Correspondence.h"
#include <QFuture>
#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>
#include <functional>
#include <optional>
#include <string>

namespace branch_truce {
class CommandFeedback;
class MergetoolSession;
class ResolutionPublisher;
class ResultEditorAdapter;
class Selection;
class SessionStore;

// Every command names the file and the revision the view showed, and is
// refused when either moved on. A true return accepts the request; durable
// outcomes arrive later through the store and CommandFeedback.
class FileCommands : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by the application")
    Q_PROPERTY(bool resolutionConfirmation READ resolutionConfirmation NOTIFY resolutionChanged)
    Q_PROPERTY(QString resolutionFileId READ resolutionFileId NOTIFY resolutionChanged)
    Q_PROPERTY(QString resolutionRevision READ resolutionRevision NOTIFY resolutionChanged)
    Q_PROPERTY(bool resolutionBusy READ resolutionBusy NOTIFY resolutionChanged)
public:
    FileCommands(SessionStore &store, const Selection &selection, ResultEditorAdapter &editor,
                 ResolutionPublisher &publisher, const MergetoolSession &mergetool, CommandFeedback &feedback,
                 QObject *parent = nullptr);
    [[nodiscard]] bool resolutionConfirmation() const;
    [[nodiscard]] QString resolutionFileId() const;
    [[nodiscard]] QString resolutionRevision() const;
    [[nodiscard]] bool resolutionBusy() const;

    // Editor offsets are UTF-16 units and must identify scalar boundaries.
    // A cut, paste or drop: one command, acknowledged once durable.
    bool editResult(const QString &fileId, const QString &revision, int begin, int end, const QString &replacement);
    // Typing: applied at once and recorded behind the editor
    // (SessionStore::typeResult).
    bool typeResult(const QString &fileId, const QString &revision, int begin, int end, const QString &replacement);
    // Writes typed edits that wait for their record now, as closing the window
    // needs. False when one could not be kept and waits for retry.
    Q_INVOKABLE bool recordTyping();
    Q_INVOKABLE bool undo(const QString &fileId, const QString &revision);
    Q_INVOKABLE bool redo(const QString &fileId, const QString &revision);
    // Save checkpoints the current text and decisions in the app store only.
    Q_INVOKABLE bool saveDraft(const QString &fileId, const QString &revision);
    // Review, checkpoint and publish one exact revision. Confirmation is
    // required while source choices remain undecided.
    Q_INVOKABLE bool markResolved(const QString &fileId, const QString &revision, bool confirmUndecided = false);
    Q_INVOKABLE void cancelResolution();
    // Moves the result cursor to the first marker named by the marker blocker.
    Q_INVOKABLE bool showFirstMarker();
    // Discard restores the manual checkpoint, or the initial result, only after that decision is durable.
    Q_INVOKABLE bool discardUnsaved(const QString &fileId, const QString &revision);
    // Drops a draft newer than the result staged in Git and makes that staged result current again.
    Q_INVOKABLE bool discardToStaged(const QString &fileId, const QString &revision);
    // Opens a file held back for its line count, after the user confirmed it.
    Q_INVOKABLE bool loadLargeFile(const QString &fileId);
    // Commits a file's pending recovery input again.
    Q_INVOKABLE bool retryPendingRecovery(const QString &fileId);

Q_SIGNALS:
    // A resolution started, finished or asked for confirmation.
    void resolutionChanged();
    void stagedDiscardFinished(const QString &fileId, const QString &revision, bool succeeded);

private:
    // The command target for the current revision of a supported file, or a shown refusal.
    std::optional<CommandTarget> admit(const QString &fileId, const QString &revision, CommandCheck check);
    // The byte edit an editor change makes to the target's current text, or a shown refusal.
    std::optional<ResultEdit> resultEdit(const CommandTarget &target, int begin, int end, const QString &replacement);
    [[nodiscard]] QString discardedMessage(const FileSession &file) const;
    [[nodiscard]] static QString stagedDiscardMessage();
    bool acknowledge(QFuture<Transition> result, const QString &failure = {}, std::function<void(bool)> finished = {});

    SessionStore &store_;
    const Selection &selection_;
    ResultEditorAdapter &editor_;
    ResolutionPublisher &publisher_;
    const MergetoolSession &mergetool_;
    CommandFeedback &feedback_;
};
}
