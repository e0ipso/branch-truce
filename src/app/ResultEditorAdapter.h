// The result editor adapter journals text edits and connects native editing actions to file history.
#pragma once
#include "Selection.h"
#include "WordDifferenceDecorations.h"
#include "highlighting/SyntaxHighlighting.h"
#include <QColor>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <vector>

class QTextDocument;

namespace branch_truce {
class FileSession;
class SessionStore;

// The result editor, one QML TextArea, seen from C++. Only this class reads or
// sets its properties and calls its methods by name. It captures native edits
// of the file it shows as domain edits, loads that file's text, keeps the
// cursor and scroll position across text replacements, and attaches syntax
// highlighting and word-level differences to the result and source panes.
// While an input method composes, it holds the selection.
class ResultEditorAdapter : public QObject, public SelectionGuard {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by the application")
    // Whether the result takes edits; the editor's readOnly binds to the inverse.
    Q_PROPERTY(bool editable READ editable NOTIFY stateChanged)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY stateChanged)
    Q_PROPERTY(bool canPaste READ canPaste NOTIFY stateChanged)
    // Where the editor put a newly shown file: its first undecided region, in
    // the keys of FileNavigator's target, with count 0 when it has none.
    Q_PROPERTY(QVariantMap placement READ placement NOTIFY placed)
public:
    enum class ClipboardAction { Copy, Cut, Paste, SelectAll };
    Q_ENUM(ClipboardAction)
    struct Caret {
        int cursor = 0;
        int selectionStart = 0;
        int selectionEnd = 0;
    };
    explicit ResultEditorAdapter(const SessionStore &store, QObject *parent = nullptr);

    Q_INVOKABLE void attach(QObject *editor);
    // Highlights a read-only source pane with the shown file's syntax.
    Q_INVOKABLE void attachSource(QObject *editor);
    // The Kirigami colour scheme background; its lightness picks the syntax theme.
    Q_INVOKABLE void setSyntaxBackground(const QColor &background);
    Q_INVOKABLE bool clipboard(branch_truce::ResultEditorAdapter::ClipboardAction action);
    // Shows fileId's text, or its pending draft, and reloads it after a
    // change. A newly shown file takes its syntax and its placement.
    void show(const QString &fileId);
    [[nodiscard]] QString fileId() const { return fileId_; }
    [[nodiscard]] bool holdsSelection() const override { return composing(); }

    [[nodiscard]] bool attached() const { return !editor_.isNull(); }
    // Whether the result takes edits: the selected file is supported, its recovery is
    // clear and the editor shows its text losslessly. The editor's readOnly binds to the
    // inverse, so this is the only rule for editing the result.
    [[nodiscard]] bool editable() const { return editable_; }
    // Whether the editor has selected text. False without one.
    [[nodiscard]] bool hasSelection() const;
    // Whether the editor would paste the clipboard now. False without one.
    [[nodiscard]] bool canPaste() const;
    // Whether an input method is composing in the editor. False without one.
    [[nodiscard]] bool composing() const;
    // All zero without an editor.
    [[nodiscard]] Caret caret() const;
    // Selects [anchor, cursor) with the cursor at the second offset.
    bool select(int anchor, int cursor);
    void setCursorPosition(int cursor);
    [[nodiscard]] QVariantMap placement() const { return placement_; }

    // Remember the cursor, selection and scroll position, and put them back
    // later if the editor still shows the same text of the same file.
    Q_INVOKABLE void holdLocation();
    Q_INVOKABLE void restoreLocation();

Q_SIGNALS:
    // A native edit of the selected file, as editResult arguments.
    void edited(const QString &fileId, const QString &revision, int begin, int end, const QString &replacement);
    void editActionRequested(const QString &name);
    // The editor text no longer matched the file's current revision.
    void editRejected();
    // Shown as the command message.
    void notice(const QString &message);
    // Shown as the command message unless it already is.
    void loadRefused(const QString &message);
    // A newly selected file was loaded and its cursor moved to placement().
    void placed();
    // editable(), hasSelection() or canPaste() may have changed.
    void stateChanged();

private:
    struct Location {
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
    bool eventFilter(QObject *watched, QEvent *event) override;
    void capture();
    // Loads the shown file's text, or its pending draft, into the editor.
    void sync();
    void setEditable(bool editable);
    void place(const FileSession *file, const QString &loaded);
    [[nodiscard]] Location location() const;
    void keepPlace(const Location &place, const QString &before, const QString &after);
    [[nodiscard]] const FileSession *shownFile() const;
    // The first undecided region of a newly shown file, whose loaded text is
    // its current revision; offsets of a pending draft would differ.
    [[nodiscard]] QVariantMap firstUndecidedPlacement(const FileSession *file, const QString &loaded) const;

    const SessionStore &store_;
    QString fileId_;
    SyntaxHighlighting syntax_;
    WordDifferenceDecorations words_;
    QPointer<QObject> editor_;
    QPointer<QTextDocument> document_;
    QMetaObject::Connection documentConnection_;
    std::vector<QMetaObject::Connection> stateConnections_;
    QString text_;
    // The file the editor last placed, and whether it was supported then. A
    // file opens as an unsupported placeholder until its captures load.
    QString placedFileId_;
    bool placedSupported_ = false;
    QVariantMap placement_;
    bool editable_ = false;
    bool syncing_ = false;
    bool processingEvent_ = false;
    Location heldLocation_;
};
}
