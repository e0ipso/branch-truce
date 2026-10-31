// Comparison panes keep source and result scroll positions aligned through their passage correspondence.
#pragma once
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QString>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>
#include <optional>

namespace branch_truce {
class TextBlocks;

// The text panes of one comparison by role, which keeps them scrolled together.
//
// A pane is an item with an editor (its TextEdit), a viewport (the Flickable that
// scrolls it) and blocks (the TextBlocks of its document). The roles have the
// names the passages use: "stage2" is the left source, "result" the editable
// result, "stage3" the right source and "earlier" the earlier version, which
// takes part only while it is visible.
//
// The panes share a reference row a few lines below their tops. The line on the
// leading pane's row maps through its side's passages to the result, and from
// there through each other side's passages, and each other pane scrolls to put
// its line on that row. An equal passage maps line by line; any other passage
// maps proportionally between its boundaries, so an ambiguous one scrolls
// without a jump. A side without passages maps proportionally over the whole
// text.
class ComparisonPanes : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickItem *left READ left WRITE setLeft NOTIFY panesChanged)
    Q_PROPERTY(QQuickItem *result READ result WRITE setResult NOTIFY panesChanged)
    Q_PROPERTY(QQuickItem *right READ right WRITE setRight NOTIFY panesChanged)
    Q_PROPERTY(QQuickItem *earlier READ earlier WRITE setEarlier NOTIFY panesChanged)
    // The file's correspondence passages, as maps with side, sourceLineBegin,
    // sourceLineEnd, resultLineBegin and resultLineEnd. Each side's passages
    // are in order on both of their sides.
    Q_PROPERTY(QVariantList passages READ passages WRITE setPassages NOTIFY passagesChanged)
    // The panes' line spacing. The reference row is at most three lines down.
    Q_PROPERTY(qreal lineSpacing READ lineSpacing WRITE setLineSpacing NOTIFY lineSpacingChanged)
public:
    // One passage of a side as fractional lines: [sourceBegin, sourceEnd) of
    // the source and [resultBegin, resultEnd) of the result.
    struct Span {
        qreal sourceBegin = 0;
        qreal sourceEnd = 0;
        qreal resultBegin = 0;
        qreal resultEnd = 0;
    };

    explicit ComparisonPanes(QObject *parent = nullptr);

    QQuickItem *left() const;
    void setLeft(QQuickItem *pane);
    QQuickItem *result() const;
    void setResult(QQuickItem *pane);
    QQuickItem *right() const;
    void setRight(QQuickItem *pane);
    QQuickItem *earlier() const;
    void setEarlier(QQuickItem *pane);
    QVariantList passages() const;
    void setPassages(const QVariantList &passages);
    qreal lineSpacing() const;
    void setLineSpacing(qreal spacing);

    // Maps a fractional line through one side's passages, from the source to
    // the result when toResult is set and back otherwise. fromLines and
    // toLines are the line counts of the two texts, for a side without passages.
    // At an empty passage, the boundary maps after the inserted lines, so a
    // following equal line still maps to its corresponding line.
    static qreal mapLine(const QList<Span> &spans, qreal line, bool toResult, int fromLines, int toLines);
    // The line of the pane named to that matches line of the pane named from,
    // through the result. The counts are the lines of from, the result and to.
    qreal mapBetween(const QString &from, const QString &to, qreal line, int fromLines, int resultLines,
                     int toLines) const;

    // The editor of the pane named name, or the result's for any other name.
    Q_INVOKABLE QQuickItem *editor(const QString &name) const;
    // Scrolls the other shown panes, or only the one named only, to follow
    // the pane named leader. Moving them does not make them lead in turn.
    Q_INVOKABLE void follow(const QString &leader, const QString &only = {});
    // A pane leads only when the user scrolls it: dragging, flicking or the
    // wheel, its scroll bar, or keyboard focus in its text. A relayout that
    // moves a pane the user is not scrolling leaves the others alone.
    Q_INVOKABLE void paneScrolled(const QString &name, bool byUser);
    // Puts line of the pane named lead on the reference row and the other
    // panes on their matching lines. A relayout scrolls a source to keep its
    // cursor in view, so each source's cursor goes to the line on its row.
    Q_INVOKABLE void place(const QString &lead, qreal line);
    // Brings a pane that was just shown to the result's position. Showing it
    // lays it out again, which would scroll it back to the cursor it had while
    // hidden. Following can leave a line cut at the row, so the cursor goes to
    // the first whole line below it.
    Q_INVOKABLE void paneShown(const QString &name);

Q_SIGNALS:
    void panesChanged();
    void passagesChanged();
    void lineSpacingChanged();

private:
    struct Pane {
        QString name;
        QQuickItem *viewport = nullptr;
        QQuickItem *editor = nullptr;
        TextBlocks *blocks = nullptr;
    };
    QQuickItem *pane(const QString &name) const;
    void setPane(QPointer<QQuickItem> &slot, QQuickItem *pane);
    static std::optional<Pane> partsOf(const QString &name, QQuickItem *pane);
    // The panes that scroll together, in role order.
    QList<Pane> shownPanes() const;
    qreal referenceRow(const QList<Pane> &panes) const;
    static qreal lineOnRow(const Pane &pane, qreal row);
    static void putLineOnRow(const Pane &pane, qreal line, qreal row);
    static void keepCursorOnLine(const Pane &pane, qreal line);
    qreal toResult(const QString &side, qreal line, int sideLines, int resultLines) const;
    qreal fromResult(const QString &side, qreal line, int resultLines, int sideLines) const;

    QPointer<QQuickItem> left_;
    QPointer<QQuickItem> result_;
    QPointer<QQuickItem> right_;
    QPointer<QQuickItem> earlier_;
    QVariantList passages_;
    QHash<QString, QList<Span>> spans_;
    qreal lineSpacing_ = 0;
    bool following_ = false;
};
}
