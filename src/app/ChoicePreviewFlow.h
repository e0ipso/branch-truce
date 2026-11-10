// Choice previews show how a source action would change the result before the user confirms it.
#pragma once
#include "core/EditingCommands.h"
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <optional>
#include <variant>

namespace branch_truce {
class CommandFeedback;
class ResultEditorAdapter;
class Selection;
class SessionModel;
class SessionStore;
class TextPositions;

// Previews a replacement of the selected file's result and applies exactly
// that preview: one choice scope, the non-conflicting changes, or the whole
// file from one side. One preview is pending at a time; its outcome is shown
// as the command message.
class ChoicePreviewFlow : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by the application")
    // The pending preview of each scope for the dialogs, or empty.
    Q_PROPERTY(bool previewPending READ previewPending NOTIFY previewPendingChanged)
    Q_PROPERTY(QVariantMap choicePreview READ choicePreview NOTIFY choicePreviewChanged)
    Q_PROPERTY(QVariantMap nonConflictingPreview READ nonConflictingPreview NOTIFY nonConflictingPreviewChanged)
    Q_PROPERTY(QVariantMap wholeFilePreview READ wholeFilePreview NOTIFY wholeFilePreviewChanged)
public:
    enum class Scope { Choice, NonConflicting, WholeFile };
    Q_ENUM(Scope)
    enum class Action { Accept, Ignore, ResolveUsingSide, CombineLeftFirst, CombineRightFirst };
    Q_ENUM(Action)
    enum class NonConflicting { LeftChanges, RightChanges, BothChanges };
    Q_ENUM(NonConflicting)
    enum class Side { LeftSide, RightSide };
    Q_ENUM(Side)
    using Option = std::variant<Action, NonConflicting, Side>;
    ChoicePreviewFlow(SessionStore &store, const SessionModel &files, const ResultEditorAdapter &editor,
                      const Selection &selection, CommandFeedback &feedback, QObject *parent = nullptr);

    Q_INVOKABLE bool previewChoice(const QString &fileId, const QString &revision, const QString &target,
                                   branch_truce::ChoicePreviewFlow::Action action)
    {
        return preview(Scope::Choice, fileId, revision, action, target);
    }
    Q_INVOKABLE bool previewNonConflicting(const QString &fileId, const QString &revision,
                                           branch_truce::ChoicePreviewFlow::NonConflicting scope)
    {
        return preview(Scope::NonConflicting, fileId, revision, scope);
    }
    Q_INVOKABLE bool previewWholeFile(const QString &fileId, const QString &revision,
                                      branch_truce::ChoicePreviewFlow::Side side)
    {
        return preview(Scope::WholeFile, fileId, revision, side);
    }
    [[nodiscard]] bool previewPending() const { return waiting_; }
    // confirmReplacement allows a replacement of manual work the preview asked to confirm.
    Q_INVOKABLE bool applyChoice(bool confirmReplacement = false) { return apply(Scope::Choice, confirmReplacement); }
    Q_INVOKABLE bool applyNonConflicting() { return apply(Scope::NonConflicting, true); }
    Q_INVOKABLE bool applyWholeFile() { return apply(Scope::WholeFile, true); }
    [[nodiscard]] QVariantMap choicePreview() const { return view(Scope::Choice); }
    [[nodiscard]] QVariantMap nonConflictingPreview() const { return view(Scope::NonConflicting); }
    [[nodiscard]] QVariantMap wholeFilePreview() const { return view(Scope::WholeFile); }

    // Replaces any pending preview. The option must match the scope, with target
    // the choice's contribution ID for Choice. False when nothing is left pending.
    bool preview(Scope scope, const QString &fileId, const QString &revision, Option option,
                 const QString &target = {});
    // Applies the pending preview of scope to the selected file. confirm
    // allows a replacement the preview asked to confirm.
    bool apply(Scope scope, bool confirm);
    // Drops the pending preview and clears the command message.
    Q_INVOKABLE void cancel();
    // The pending preview of scope for the dialogs, or empty.
    [[nodiscard]] QVariantMap view(Scope scope) const;

Q_SIGNALS:
    // view(scope) may have changed: a preview of that scope became pending or
    // stopped pending, or the session's files were replaced under it.
    void changed(branch_truce::ChoicePreviewFlow::Scope scope);
    void ready(branch_truce::ChoicePreviewFlow::Scope scope);
    void previewPendingChanged();
    void choicePreviewChanged();
    void nonConflictingPreviewChanged();
    void wholeFilePreviewChanged();

private:
    bool prepare(Scope scope, const QString &fileId, RevisionId expected, Option option, const QString &target);
    [[nodiscard]] std::optional<Scope> pendingScope() const;
    [[nodiscard]] QVariantMap choiceView(const ChoicePreview &preview, const FileSession &file) const;
    [[nodiscard]] QVariantMap nonConflictingView(const NonConflictingPreview &preview, const FileSession &file) const;
    [[nodiscard]] QVariantMap wholeFileView(const WholeFilePreview &preview, const FileSession &file) const;
    [[nodiscard]] static QString sideLabel(SourceSide side);
    // "Left, lines 2–3", or "Left, empty scope at line 2" for an empty range.
    [[nodiscard]] static QString rangeLabel(const QString &label, const TextPositions &lines, ByteRange range);

    SessionStore &store_;
    const SessionModel &files_;
    const ResultEditorAdapter &editor_;
    const Selection &selection_;
    CommandFeedback &feedback_;
    std::variant<std::monostate, ChoicePreview, NonConflictingPreview, WholeFilePreview> pending_;
    QString fileId_;
    quint64 request_ = 0;
    bool waiting_ = false;
};
}
