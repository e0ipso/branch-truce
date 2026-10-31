// Passage search finds text within the selected comparison scope and highlights its matches.
#pragma once
#include "ComparisonPanes.h"
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

namespace branch_truce {
// Passage search in the comparison panes: its scopes, its matching and the status it shows.
//
// The scopes are the left source, the editable result and the right source, and the
// earlier version while that pane is shown. Matching defaults to case-insensitive. find() returns
// the match range and the number of matches in the scope, so a search bar can show
// which match is selected out of how many and step to the next or the previous one.
class PassageSearch : public QObject {
    Q_OBJECT
    QML_ELEMENT
    // The panes whose editors the scopes name.
    Q_PROPERTY(branch_truce::ComparisonPanes *panes READ panes WRITE setPanes NOTIFY panesChanged)
    // Whether the earlier version is shown, which adds it to the scopes.
    Q_PROPERTY(bool earlierShown READ earlierShown WRITE setEarlierShown NOTIFY scopesChanged)
    // The scope labels in order, for the scope selector.
    Q_PROPERTY(QStringList scopeLabels READ scopeLabels NOTIFY scopesChanged)
    // The selected scope's index in scopeLabels. When the scopes change, the
    // selection stays on the same pane, or goes to the result when its pane left.
    Q_PROPERTY(int scopeIndex READ scopeIndex WRITE setScopeIndex NOTIFY scopeIndexChanged)
    // The pane name of the selected scope: stage2, result, stage3 or earlier.
    Q_PROPERTY(QString scope READ scope NOTIFY scopeIndexChanged)
    // The sentence under the search field: the last search's outcome, or the
    // selected scope until a search runs in it.
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(int matchCount READ matchCount NOTIFY matchesChanged)
    Q_PROPERTY(int matchIndex READ matchIndex NOTIFY matchesChanged)
public:
    // A match of [begin, end) in the text, with begin -1 without one. count is
    // the number of matches that do not overlap, counted from the start.
    struct Match {
        int begin = -1;
        int end = -1;
        int count = 0;
    };

    explicit PassageSearch(QObject *parent = nullptr);

    ComparisonPanes *panes() const;
    void setPanes(ComparisonPanes *panes);
    bool earlierShown() const;
    void setEarlierShown(bool shown);
    QStringList scopeLabels() const;
    int scopeIndex() const;
    void setScopeIndex(int index);
    QString scope() const;
    QString status() const;
    int matchCount() const { return matchCount_; }
    int matchIndex() const { return matchIndex_; }

    // Finds query in text with the requested case sensitivity. [selectionStart, selectionEnd) is
    // a selection that holds the query, or empty without one. Forward takes
    // the first match from the selection's end, backward the last match that
    // starts before the selection's start; without such a selection they take
    // the first and the last match. Either wraps around to the other end of
    // the text when no match lies that way.
    static Match search(const QString &text, const QString &query, int selectionStart, int selectionEnd,
                        bool forward, Qt::CaseSensitivity sensitivity = Qt::CaseSensitive);
    // Finds query in the text of the pane named scope, going on from its
    // selection when the selection holds the query, and sets the status. The
    // result has begin and end, the match range, with begin -1 without a
    // match, and count, the number of matches in the scope. Forward steps to
    // the next match and backward to the previous one.
    Q_INVOKABLE QVariantMap find(const QString &scope, const QString &query, bool forward = true);
    // Recompute and paint the selected scope without changing its document text.
    Q_INVOKABLE void update(const QString &query, bool caseSensitive, bool active);

Q_SIGNALS:
    void panesChanged();
    void scopesChanged();
    void scopeIndexChanged();
    void statusChanged();
    void matchesChanged();

private:
    struct Scope {
        QString name;
        QString label;
    };
    QList<Scope> scopes() const;
    QString labelOf(const QString &name) const;
    void setDetail(const QString &detail);

    QPointer<ComparisonPanes> panes_;
    bool earlierShown_ = false;
    int scopeIndex_ = 0;
    // The last search's sentence, empty until a search runs in the selected scope.
    QString detail_;
    bool refreshing_ = false;
    bool caseSensitive_ = false;
    int matchCount_ = 0;
    int matchIndex_ = 0;
};
}
