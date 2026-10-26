#pragma once
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

namespace branch_truce {
// Lists the document blocks of a TextEdit, one per line of the file, so a
// gutter can number lines rather than the visual lines that wrapping makes.
class TextBlocks : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(int blockCount READ blockCount NOTIFY blockCountChanged)
public:
    explicit TextBlocks(QObject *parent = nullptr);
    QQuickTextDocument *document() const;
    void setDocument(QQuickTextDocument *document);
    int blockCount() const;
    // The blocks whose layout meets [top, bottom) in document coordinates, in
    // order. Each entry has number (1-based), position (its first character),
    // and top and height, the block's extent in document coordinates.
    Q_INVOKABLE QVariantList between(qreal top, qreal bottom) const;
    // The 1-based number of the block holding the character at position, or 0
    // without a document.
    Q_INVOKABLE int lineAt(int position) const;
    // The line at y in document coordinates as a fractional 1-based number:
    // the block's number plus the part of its height above y. It is 1 above
    // the text and one past the last block below it.
    Q_INVOKABLE qreal lineAtY(qreal y) const;
    // The document y of a fractional line from lineAtY.
    Q_INVOKABLE qreal yAtLine(qreal line) const;
    // The position of the first character of a 1-based line, or 0 without a document.
    Q_INVOKABLE int positionOf(int line) const;

Q_SIGNALS:
    void documentChanged();
    void blockCountChanged();
    // The text or its layout changed, so block positions may have moved.
    void layoutChanged();

private:
    QPointer<QQuickTextDocument> document_;
    QList<QMetaObject::Connection> connections_;
};
}
