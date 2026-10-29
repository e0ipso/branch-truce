#pragma once
#include <QtQml/qqmlregistration.h>
#include "core/EditingCommands.h"
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <functional>
#include <optional>
#include <variant>

namespace branch_truce {
class ResultEditorAdapter;
class SessionModel;
class TextPositions;

// Previews a replacement of the selected file's result and applies exactly
// that preview: one choice scope, the non-conflicting changes, or the whole
// file from one side. One preview is pending at a time.
class ChoicePreviewFlow final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by ApplicationController")
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
    // What the flow needs from the window.
    struct Owners {
        std::function<QString()> selectedFileId;
        // Escaped display text for a file's exact path bytes.
        std::function<QString(const QString &fileId)> displayPath;
        // Shows a command outcome as the command message and notifies it.
        // True when the command applied or changed nothing.
        std::function<bool(Transition result)> report;
        // Shows the command message and notifies it.
        std::function<void(const QString &message)> setMessage;
    };
    ChoicePreviewFlow(SessionModel &files, ResultEditorAdapter &editor, Owners owners, QObject *parent = nullptr);

    // Replaces any pending preview. The option must match the scope, with target
    // the choice's contribution ID for Choice. False when nothing is left pending.
    bool preview(Scope scope, const QString &fileId, const QString &revision,
                 Option option, const QString &target = {});
    // Applies the pending preview of scope to the selected file. confirm
    // allows a replacement the preview asked to confirm.
    bool apply(Scope scope, bool confirm);
    // Drops the pending preview and clears the command message.
    void cancel();
    // The pending preview of scope for the dialogs, or empty.
    [[nodiscard]] QVariantMap view(Scope scope) const;

Q_SIGNALS:
    // view(scope) may have changed: a preview of that scope became pending or
    // stopped pending, or the session's files were replaced under it.
    void changed(branch_truce::ChoicePreviewFlow::Scope scope);

private:
    [[nodiscard]] std::optional<Scope> pendingScope() const;
    [[nodiscard]] QVariantMap choiceView(const ChoicePreview &preview, const FileSession &file) const;
    [[nodiscard]] QVariantMap nonConflictingView(const NonConflictingPreview &preview, const FileSession &file) const;
    [[nodiscard]] QVariantMap wholeFileView(const WholeFilePreview &preview, const FileSession &file) const;
    [[nodiscard]] static QString sideLabel(SourceSide side);
    // "Left, lines 2–3", or "Left, empty scope at line 2" for an empty range.
    [[nodiscard]] static QString rangeLabel(const QString &label, const TextPositions &lines, ByteRange range);

    SessionModel &files_;
    ResultEditorAdapter &editor_;
    Owners owners_;
    std::variant<std::monostate, ChoicePreview, NonConflictingPreview, WholeFilePreview> pending_;
    QString fileId_;
};
}
