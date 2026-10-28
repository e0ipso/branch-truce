#pragma once
#include "SyntaxHighlighting.h"
#include "WordDifferenceDecorations.h"
#include <QColor>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <functional>

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
public:
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
    void setVisibleWhitespace(QObject *editor, bool visible);
    bool clipboard(const QString &action);
    // Loads the selected file's text, or its pending draft, into the editor.
    void sync();

    [[nodiscard]] bool attached() const { return !editor_.isNull(); }
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
    void undoRequested(const QString &fileId, const QString &revision);
    void redoRequested(const QString &fileId, const QString &revision);
    // The editor text no longer matched the file's current revision.
    void editRejected();
    // Shown as the command message.
    void notice(const QString &message);
    // Shown as the command message unless it already is.
    void loadRefused(const QString &message);
    // A newly selected file was loaded and its cursor moved to placement().
    void placed();

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
    void place(const FileSession *file, const QString &loaded);
    void applyVisibleWhitespace(QTextDocument *document);
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
    QString text_;
    // The file the editor last placed, and whether it was supported then. A
    // file opens as an unsupported placeholder until its captures load.
    QString placedFileId_;
    bool placedSupported_ = false;
    QVariantMap placement_;
    bool syncing_ = false;
    bool processingEvent_ = false;
    bool showWhitespace_ = false;
    Location heldLocation_;
};
}
