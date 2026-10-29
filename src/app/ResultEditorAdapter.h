#pragma once
#include <QtQml/qqmlregistration.h>
#include "highlighting/SyntaxHighlighting.h"
#include "WordDifferenceDecorations.h"
#include <QColor>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <functional>
#include <vector>

class QTextDocument;

namespace branch_truce {
class FileSession;
class SessionModel;

// The result editor, one QML TextArea, seen from C++. Only this class reads or
// sets its properties and calls its methods by name. It captures native edits
// as domain edits, loads the selected file's text, keeps the cursor and scroll
// position across text replacements, and attaches syntax highlighting and
// word-level differences to the result and source panes.
class ResultEditorAdapter final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by ApplicationController")
public:
    enum class ClipboardAction { Copy, Cut, Paste, SelectAll };
    Q_ENUM(ClipboardAction)
    // Where a newly loaded file puts its cursor: the keys of editorPlacement,
    // with utf16Begin as the cursor when it is not negative.
    using Placement = std::function<QVariantMap(const FileSession *file, const QString &loaded)>;
    struct Caret {
        int cursor = 0;
        int selectionStart = 0;
        int selectionEnd = 0;
    };
    ResultEditorAdapter(SessionModel &files, std::function<QString()> selectedFileId, Placement placement,
                        QObject *parent = nullptr);

    void attach(QObject *editor);
    void attachSource(QObject *editor);
    void setSyntaxFileName(const QString &fileName);
    void setSyntaxBackground(const QColor &background);
    bool clipboard(ClipboardAction action);
    // Loads the selected file's text, or its pending draft, into the editor.
    void sync();

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
    void holdLocation();
    void restoreLocation();

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
    void setEditable(bool editable);
    void place(const FileSession *file, const QString &loaded);
    [[nodiscard]] Location location() const;
    void keepPlace(const Location &place, const QString &before, const QString &after);
    [[nodiscard]] const FileSession *selectedFile() const;

    SessionModel &files_;
    std::function<QString()> selectedFileId_;
    Placement placementFor_;
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
