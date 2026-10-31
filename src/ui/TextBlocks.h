// Text block geometry keeps line numbers, folds and comparison decorations aligned with the editor.
#pragma once
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QTextBlock>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

class QAbstractTextDocumentLayout;
class QTextDocument;

namespace branch_truce {
// Lists the document blocks of a TextEdit, one per line of the file, so a
// gutter can number lines rather than the visual lines that wrapping makes.
//
// It can also hide lines from the view. The document keeps their text, so
// the TextEdit's text, positions and line numbers stay the buffer's; only
// the layout leaves them out. A hidden line has no height and lies at the
// top of the next shown line. One TextBlocks per document sets hidden; the
// geometry of every TextBlocks on that document skips the hidden lines.
//
// A folded block keeps its first line in the layout as the row the view
// draws over in the block's place. That row counts as hidden for the cursor
// and the other queries of the TextBlocks that sets hidden, but its geometry
// is a shown line's.
class TextBlocks : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(qreal rowSpacing READ rowSpacing WRITE setRowSpacing NOTIFY rowSpacingChanged)
    // The first shown visual row's laid-out advance, including line distance.
    Q_PROPERTY(qreal rowHeight READ rowHeight NOTIFY layoutChanged)
    Q_PROPERTY(int blockCount READ blockCount NOTIFY blockCountChanged)
    // The lines to hide, as {line, text} maps: a 1-based line number and the
    // text that line must hold. A line is hidden only while it holds that
    // text, so an entry that no longer matches the document hides nothing.
    // Edits and replaced text keep the lines hidden that still match.
    //
    // An entry with last and lastText folds the block of lines from line to
    // last, while line holds text and last holds lastText. The lines after
    // line are hidden and line becomes the block's row.
    Q_PROPERTY(QVariantList hidden READ hidden WRITE setHidden NOTIFY hiddenChanged)
public:
    explicit TextBlocks(QObject *parent = nullptr);
    QQuickTextDocument *document() const;
    void setDocument(QQuickTextDocument *document);
    qreal rowSpacing() const;
    void setRowSpacing(qreal spacing);
    qreal rowHeight() const;
    // Finish a pending text-change relayout before the editor restores its viewport.
    Q_INVOKABLE void flushSpacing();
    int blockCount() const;
    QVariantList hidden() const;
    void setHidden(const QVariantList &hidden);
    // The shown blocks whose layout meets [top, bottom) in document
    // coordinates, in order. Each entry has number (1-based), position (its
    // first character), top and height (the block's extent in document
    // coordinates), and rowHeight (its first visual row's laid-out advance).
    Q_INVOKABLE QVariantList between(qreal top, qreal bottom) const;
    // The 1-based number of the block holding the character at position, or 0
    // without a document.
    Q_INVOKABLE int lineAt(int position) const;
    // The shown line at y in document coordinates as a fractional 1-based
    // number: the block's number plus the part of its height above y. It is 1
    // above the text and one past the last shown block below it.
    Q_INVOKABLE qreal lineAtY(qreal y) const;
    // The document y of a fractional line from lineAtY. A hidden line is at
    // the top of the next shown line.
    Q_INVOKABLE qreal yAtLine(qreal line) const;
    // The position of the first character of a 1-based line, or 0 without a document.
    Q_INVOKABLE int positionOf(int line) const;
    // True when the character at position is on a hidden line or a folded
    // block's row.
    Q_INVOKABLE bool hiddenAt(int position) const;
    // True when a hidden line or a folded block's row holds any character of [begin, end).
    Q_INVOKABLE bool hiddenWithin(int begin, int end) const;
    // Where a cursor at position goes so that it is not on a hidden line or a row:
    // position itself when its line is shown. Otherwise forward takes the
    // next shown line and backward the previous one, or the other one at
    // either end of the text. With from at 0 or more the cursor keeps the x
    // of position from and goes to the nearest row of that line, as a move
    // by lines would; otherwise it goes to the line's near edge, the start
    // of a following line or the end of a preceding one.
    Q_INVOKABLE int shownPosition(int position, bool forward, int from = -1) const;
    // The space and tab markers on the shown lines other than rows that meet area, in document
    // coordinates, as {x, y, w, h, tab} maps relative to area's top left. A
    // marker covers its character's advance and its row's height, and is at
    // least stroke wide and high. Where the next position starts another row,
    // as at a wrap, a space's marker is stroke wide and a tab's tabWidth.
    Q_INVOKABLE QVariantList whitespaceWithin(const QRectF &area, qreal stroke, qreal tabWidth) const;

    // The cursor never rests on a hidden line. A key that moves it there takes
    // it on in the key's direction: Up, Down, Page Up and Page Down move by
    // lines and keep the column the cursor had, and any other key goes on the
    // way the cursor went. A selection set by other code ends on its anchor's
    // side, and a cursor placed by other code goes to the next shown line.
    //
    // Notes the key that is about to move the cursor and asks for a check.
    Q_INVOKABLE void keyPressed(int key);
    // Asks for one cursor check after the current change completes, however
    // often it is asked before then: an edit that joins a hidden line to its
    // neighbour shows that line only once the text is captured.
    Q_INVOKABLE void queueCursorCheck();
    // Where the cursor at position goes, with anchor the other end of its
    // selection, or position without one. It uses up the noted key, and the
    // answer is where the next check starts from.
    Q_INVOKABLE int settledPosition(int position, int anchor);

Q_SIGNALS:
    void documentChanged();
    void blockCountChanged();
    void rowSpacingChanged();
    void hiddenChanged();
    // The text or its layout changed, so block positions may have moved.
    void layoutChanged();
    // A queued cursor check is due.
    void cursorCheckDue();

private:
    // One entry of hidden: a single line when last is 0, otherwise a block.
    struct Fold {
        int line = 0;
        QString text;
        int last = 0;
        QString lastText;
    };
    QRectF extent(QAbstractTextDocumentLayout *layout, const QTextBlock &block) const;
    qreal firstRowHeight(QAbstractTextDocumentLayout *layout, const QTextBlock &block) const;
    // A hidden line or a folded block's row, where the cursor never rests.
    bool folded(const QTextBlock &block) const;
    // block itself or the nearest one in that direction that is not folded.
    QTextBlock reachable(QTextBlock block, bool forward) const;
    void reconnectDocument();
    void scheduleSpacing(int position, int removed, int added);
    void applySpacing(QTextDocument *text, qreal spacing, int from, int to);
    void scheduleHiding();
    void applyHidden();

    QPointer<QQuickTextDocument> document_;
    QList<QMetaObject::Connection> connections_;
    QMetaObject::Connection documentSwapConnection_;
    QPointer<QTextDocument> text_;
    qreal rowSpacing_ = 0;
    int spacingFrom_ = -1;
    int spacingTo_ = -1;
    bool spacingQueued_ = false;
    QVariantList hidden_;
    QList<Fold> folds_;
    // The 1-based lines that are rows of folded blocks after the last application.
    QSet<int> rows_;
    // Hidden blocks after the last application. Without one no edit makes a
    // hidden block, so an empty hidden list then has nothing to apply.
    int hiddenCount_ = 0;
    bool hidingQueued_ = false;
    bool applying_ = false;
    int cursorKey_ = 0;
    int settledCursor_ = 0;
    bool cursorCheckQueued_ = false;
};
}
