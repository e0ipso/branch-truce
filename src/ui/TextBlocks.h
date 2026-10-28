#pragma once
#include <QHash>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QRectF>
#include <QString>
#include <QTextBlock>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

class QAbstractTextDocumentLayout;

namespace branch_truce {
// Lists the document blocks of a TextEdit, one per line of the file, so a
// gutter can number lines rather than the visual lines that wrapping makes.
//
// It can also hide lines from the view. The document keeps their text, so
// the TextEdit's text, positions and line numbers stay the buffer's; only
// the layout leaves them out. A hidden line has no height and lies at the
// top of the next shown line. One TextBlocks per document sets hidden; the
// geometry of every TextBlocks on that document skips the hidden lines.
class TextBlocks : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(int blockCount READ blockCount NOTIFY blockCountChanged)
    // The lines to hide, as {line, text} maps: a 1-based line number and the
    // text that line must hold. A line is hidden only while it holds that
    // text, so an entry that no longer matches the document hides nothing.
    // Edits and replaced text keep the lines hidden that still match.
    Q_PROPERTY(QVariantList hidden READ hidden WRITE setHidden NOTIFY hiddenChanged)
public:
    explicit TextBlocks(QObject *parent = nullptr);
    QQuickTextDocument *document() const;
    void setDocument(QQuickTextDocument *document);
    int blockCount() const;
    QVariantList hidden() const;
    void setHidden(const QVariantList &hidden);
    // The shown blocks whose layout meets [top, bottom) in document
    // coordinates, in order. Each entry has number (1-based), position (its
    // first character), and top and height, the block's extent in document
    // coordinates.
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
    // True when the character at position is on a hidden line.
    Q_INVOKABLE bool hiddenAt(int position) const;
    // True when a hidden line holds any character of [begin, end).
    Q_INVOKABLE bool hiddenWithin(int begin, int end) const;
    // Where a cursor at position goes so that it is not on a hidden line:
    // position itself when its line is shown. Otherwise forward takes the
    // next shown line and backward the previous one, or the other one at
    // either end of the text. With from at 0 or more the cursor keeps the x
    // of position from and goes to the nearest row of that line, as a move
    // by lines would; otherwise it goes to the line's near edge, the start
    // of a following line or the end of a preceding one.
    Q_INVOKABLE int shownPosition(int position, bool forward, int from = -1) const;

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
    void hiddenChanged();
    // The text or its layout changed, so block positions may have moved.
    void layoutChanged();
    // A queued cursor check is due.
    void cursorCheckDue();

private:
    QRectF extent(QAbstractTextDocumentLayout *layout, const QTextBlock &block) const;
    void scheduleHiding();
    void applyHidden();

    QPointer<QQuickTextDocument> document_;
    QList<QMetaObject::Connection> connections_;
    QVariantList hidden_;
    QHash<int, QString> hiddenText_;
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
