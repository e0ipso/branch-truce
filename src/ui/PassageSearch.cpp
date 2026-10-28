#include "PassageSearch.h"
#include <QQuickItem>
#include <QScopedValueRollback>

namespace branch_truce {
namespace {
const QString resultName = QStringLiteral("result");
}

PassageSearch::PassageSearch(QObject *parent) : QObject(parent) {}

ComparisonPanes *PassageSearch::panes() const { return panes_; }

void PassageSearch::setPanes(ComparisonPanes *panes)
{
    if (panes_ == panes) return;
    panes_ = panes;
    Q_EMIT panesChanged();
}

bool PassageSearch::earlierShown() const { return earlierShown_; }

void PassageSearch::setEarlierShown(bool shown)
{
    if (earlierShown_ == shown) return;
    const auto current = scope();
    earlierShown_ = shown;
    {
        // A selector given new scopes may select its first; the selection is set below instead.
        QScopedValueRollback guard(refreshing_, true);
        Q_EMIT scopesChanged();
    }
    const auto list = scopes();
    int index = -1;
    for (int at = 0; at < list.size(); ++at) {
        if (list.at(at).name == current) index = at;
    }
    for (int at = 0; index < 0 && at < list.size(); ++at) {
        if (list.at(at).name == resultName) index = at;
    }
    scopeIndex_ = index;
    // Even with the same index, so a selector that moved to its first scope comes back.
    Q_EMIT scopeIndexChanged();
    Q_EMIT statusChanged();
}

QList<PassageSearch::Scope> PassageSearch::scopes() const
{
    QList<Scope> list{{QStringLiteral("stage2"), tr("Left source")}, {resultName, tr("Editable result")},
        {QStringLiteral("stage3"), tr("Right source")}};
    if (earlierShown_) list.append({QStringLiteral("earlier"), tr("Earlier version")});
    return list;
}

QStringList PassageSearch::scopeLabels() const
{
    QStringList labels;
    for (const auto &entry : scopes()) labels.append(entry.label);
    return labels;
}

int PassageSearch::scopeIndex() const { return scopeIndex_; }

void PassageSearch::setScopeIndex(int index)
{
    // Choosing another scope, or getting new scopes, clears the last search's sentence.
    setDetail({});
    if (refreshing_ || index == scopeIndex_) return;
    scopeIndex_ = index;
    Q_EMIT scopeIndexChanged();
    Q_EMIT statusChanged();
}

QString PassageSearch::scope() const
{
    const auto list = scopes();
    return scopeIndex_ >= 0 && scopeIndex_ < list.size() ? list.at(scopeIndex_).name : resultName;
}

QString PassageSearch::labelOf(const QString &name) const
{
    for (const auto &entry : scopes()) {
        if (entry.name == name) return entry.label;
    }
    return tr("Editable result");
}

QString PassageSearch::status() const
{
    return detail_.isEmpty() ? tr("Search scope: %1.").arg(labelOf(scope())) : detail_;
}

void PassageSearch::setDetail(const QString &detail)
{
    if (detail_ == detail) return;
    detail_ = detail;
    Q_EMIT statusChanged();
}

PassageSearch::Match PassageSearch::search(const QString &text, const QString &query, int selectionStart,
                                           int selectionEnd, bool forward)
{
    Match match;
    if (query.isEmpty()) return match;
    for (auto at = text.indexOf(query); at >= 0; at = text.indexOf(query, at + query.size())) ++match.count;
    if (match.count == 0) return match;
    const bool selected = selectionEnd > selectionStart;
    qsizetype found = -1;
    if (forward) {
        found = text.indexOf(query, selected ? selectionEnd : 0);
        if (found < 0) found = text.indexOf(query);
    } else {
        found = selected && selectionStart > 0 ? text.lastIndexOf(query, selectionStart - 1) : -1;
        if (found < 0) found = text.lastIndexOf(query);
    }
    match.begin = static_cast<int>(found);
    match.end = static_cast<int>(found + query.size());
    return match;
}

QVariantMap PassageSearch::find(const QString &scope, const QString &query, bool forward)
{
    const auto label = labelOf(scope);
    auto *editor = panes_ ? panes_->editor(scope) : nullptr;
    Match match;
    if (!editor || query.isEmpty()) {
        setDetail(tr("Search scope: %1. Enter a passage.").arg(label));
    } else {
        // A selection that holds the query is the last match, so the search goes on from it.
        const bool continuing = editor->property("selectedText").toString() == query;
        match = search(editor->property("text").toString(), query,
            continuing ? editor->property("selectionStart").toInt() : 0,
            continuing ? editor->property("selectionEnd").toInt() : 0, forward);
        setDetail(match.begin < 0 ? tr("No passage in %1.").arg(label) : tr("Passage in %1.").arg(label));
    }
    return {{QStringLiteral("begin"), match.begin}, {QStringLiteral("end"), match.end},
        {QStringLiteral("count"), match.count}};
}
}
