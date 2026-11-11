// Group domain scopes once, leaving only pane geometry and interaction in QML.
#include "ComparisonPresentation.h"
#include "core/SessionState.h"
#include <algorithm>

namespace branch_truce {
namespace {
QString sideState(const ComparisonContribution &c)
{
    const bool left = c.side == QLatin1String("stage2");
    if (c.state == QLatin1String("accepted"))
        return left ? ComparisonPresentation::tr("Left accepted") : ComparisonPresentation::tr("Right accepted");
    if (c.state == QLatin1String("ignored"))
        return left ? ComparisonPresentation::tr("Left ignored") : ComparisonPresentation::tr("Right ignored");
    if (c.state == QLatin1String("manual"))
        return left ? ComparisonPresentation::tr("Left edited by hand")
                    : ComparisonPresentation::tr("Right edited by hand");
    return left ? ComparisonPresentation::tr("Left undecided") : ComparisonPresentation::tr("Right undecided");
}
struct FoldedRow {
    int row = 0;
    QStringList sections;
};
FoldedRow foldedRow(const QVariantList &blocks, int first, bool markersShown)
{
    FoldedRow result;
    if (markersShown) return result;
    for (const auto &value : blocks) {
        const auto block = value.toMap();
        const auto lines = block.value("markerLines").toList();
        if (!block.value("folded").toBool() || lines.size() < 2) continue;
        const int begin = lines.first().toMap().value("line").toInt();
        const int end = lines.last().toMap().value("line").toInt();
        if (first < begin || first > end) continue;
        result.row = begin;
        result.sections = block.value("sections").toStringList();
        break;
    }
    return result;
}
}
int ComparisonRowModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}
QVariant ComparisonRowModel::data(const QModelIndex &index, int role) const
{
    if (index.model() != this || index.column() != 0 || index.row() < 0 || index.row() >= rows_.size()
        || role != Qt::UserRole + 1)
        return {};
    return QVariant::fromValue(rows_[index.row()]);
}
QHash<int, QByteArray> ComparisonRowModel::roleNames() const { return {{Qt::UserRole + 1, "modelData"}}; }
void ComparisonRowModel::stage(const QList<ComparisonRow> &rows, const QString &fileId)
{
    resetting_ = fileId_ != fileId || rows_.size() != rows.size()
        || !std::equal(rows_.begin(), rows_.end(), rows.begin(),
                       [](const auto &a, const auto &b) { return a.choice.id == b.choice.id; });
    changed_.clear();
    if (resetting_)
        beginResetModel();
    else
        for (int row = 0; row < rows.size(); ++row)
            if (rows_[row] != rows[row]) changed_.push_back(row);
    rows_ = rows;
    fileId_ = fileId;
}
void ComparisonRowModel::publish()
{
    if (resetting_) {
        resetting_ = false;
        endResetModel();
        return;
    }
    // One update per changed run retains every unaffected native control and popup.
    for (qsizetype at = 0; at < changed_.size();) {
        const int first = changed_[at++];
        int last = first;
        while (at < changed_.size() && changed_[at] == last + 1) last = changed_[at++];
        Q_EMIT dataChanged(index(first), index(last), {Qt::UserRole + 1});
    }
    changed_.clear();
}
QString correspondenceIdentity(const FileSession &file)
{
    return QString::number(reinterpret_cast<quintptr>(file.current().correspondence.get()), 16);
}
void ComparisonPresentation::publish(Data data)
{
    const auto previousLeft = leftRows_, previousRight = rightRows_;
    data_ = std::move(data);
    ++generation_;
    byId_.clear();
    sections_.clear();
    QHash<QString, qsizetype> sections;
    for (const auto &choice : data_.contributions) {
        byId_.insert(choice.id, choice);
        if (choice.scope != QLatin1String("section")) continue;
        if (!sections.contains(choice.section)) {
            sections.insert(choice.section, sections_.size());
            sections_.append({choice.section, {}, {}});
        }
        auto &section = sections_[sections.value(choice.section)];
        (choice.side == QLatin1String("stage2") ? section.left : section.right) = choice;
        if (choice.mapped && choice.resultLineBegin > 0) {
            section.begin =
                section.begin < 0 ? choice.resultLineBegin : std::min(section.begin, choice.resultLineBegin);
            section.end = std::max(section.end, std::max(choice.resultLineBegin + 1, choice.resultLineEnd));
        }
    }
    for (const auto &value : data_.markerBlocks) {
        const auto block = value.toMap();
        for (const auto &id : block.value("sections").toStringList()) {
            if (!sections.contains(id)) continue;
            auto &section = sections_[sections.value(id)];
            section.marked = true;
            section.begin = block.value("resultLineBegin").toInt();
            section.end = block.value("resultLineEnd").toInt();
        }
    }
    std::stable_sort(sections_.begin(), sections_.end(),
                     [](const auto &a, const auto &b) { return a.begin < b.begin; });
    const auto rowsFor = [&](const QString &side) {
        QList<ComparisonRow> rows;
        for (const auto &choice : data_.contributions)
            if (choice.side == side && choice.scope == QLatin1String("section"))
                rows.append({choice.sourceLineBegin, choice.sourceLineEnd, choice, {}});
        for (const auto &choice : data_.contributions) {
            if (choice.side != side || choice.scope == QLatin1String("section")) continue;
            auto owner = std::find_if(rows.begin(), rows.end(), [&](const auto &row) {
                return row.choice.section == choice.section && row.line <= choice.sourceLineBegin
                    && choice.sourceLineEnd <= row.end;
            });
            if (owner == rows.end())
                rows.append({choice.sourceLineBegin, choice.sourceLineEnd, choice, {}});
            else
                owner->extras.append(choice);
        }
        std::stable_sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.line < b.line; });
        return rows;
    };
    leftRows_ = rowsFor(QStringLiteral("stage2"));
    rightRows_ = rowsFor(QStringLiteral("stage3"));
    leftRowModel_.stage(leftRows_, data_.fileId);
    rightRowModel_.stage(rightRows_, data_.fileId);
    Q_EMIT changed();
    leftRowModel_.publish();
    rightRowModel_.publish();
    if (previousLeft != leftRows_) Q_EMIT leftRowsChanged();
    if (previousRight != rightRows_) Q_EMIT rightRowsChanged();
}
bool ComparisonPresentation::accepts(const QVariantMap &target) const
{
    return !fileId().isEmpty() && target.value("fileId").toString() == fileId()
        && target.value("revision").toString() == revision()
        && target.value("correspondence").toString() == correspondence()
        && (!target.contains("generation") || target.value("generation").toString() == generation());
}
QVariantMap ComparisonPresentation::choiceTarget(const QString &id) const
{
    const auto found = byId_.constFind(id);
    if (found == byId_.cend()) return {};
    const auto &c = found.value();
    return {{"scope", "decoration"},
            {"fileId", fileId()},
            {"revision", revision()},
            {"generation", generation()},
            {"correspondence", correspondence()},
            {"contributionId", c.id},
            {"side", c.side},
            {"sourceBegin", c.sourceLineBegin},
            {"sourceEnd", c.sourceLineEnd},
            {"resultBegin", c.mapped ? c.resultLineBegin : -1},
            {"resultEnd", c.mapped ? c.resultLineEnd : -1}};
}
QVariantMap ComparisonPresentation::targetAt(const QString &side, int line) const
{
    for (const auto &c : data_.contributions) {
        const bool result = side == QLatin1String("result");
        if (result ? !c.mapped : c.side != side) continue;
        const int begin = result ? c.resultLineBegin : c.sourceLineBegin;
        const int end = result ? c.resultLineEnd : c.sourceLineEnd;
        if (line >= begin && line < std::max(begin + 1, end)) return choiceTarget(c.id);
    }
    return {};
}
QVariantMap ComparisonPresentation::activeConflict(int line, const QVariantMap &navigation,
                                                   const QString &generation) const
{
    if (generation != this->generation()) return {};
    QList<const Section *> conflicts;
    for (const auto &s : sections_)
        if ((s.marked || (!s.left.id.isEmpty() && !s.right.id.isEmpty()))
            && (s.left.undecided() || s.right.undecided()))
            conflicts.append(&s);
    const auto view = [](const Section *s) -> QVariantMap {
        return s ? QVariantMap{{"section", s->id},
                               {"stage2", QVariant::fromValue(s->left)},
                               {"stage3", QVariant::fromValue(s->right)}}
                 : QVariantMap{};
    };
    for (const auto *s : conflicts)
        if (s->begin > 0 && line >= s->begin && line < s->end) return view(s);
    if (accepts(navigation)) {
        const auto owner = byId_.value(navigation.value("contributionId").toString()).section;
        for (const auto *s : conflicts)
            if (s->id == owner) return view(s);
    }
    for (const auto *s : conflicts)
        if (s->begin >= line) return view(s);
    return view(conflicts.isEmpty() ? nullptr : conflicts.first());
}
QVariantMap ComparisonPresentation::resultSections(bool markersShown) const
{
    QVariantMap decisions, combinations;
    // Result actions use the mapped contribution range, not the wider marker
    // span used to select the active conflict at the editor cursor.
    for (const auto &s : sections_) {
        int first = -1;
        for (const auto *c : {&s.left, &s.right})
            if (c->mapped && c->resultLineBegin > 0 && (first < 0 || c->resultLineBegin < first))
                first = c->resultLineBegin;
        if (first < 0) continue;
        const auto folded = foldedRow(data_.markerBlocks, first, markersShown);
        const int row = folded.row;
        const auto &foldedSections = folded.sections;
        const auto key = QString::number(row > 0 ? row : first);
        if (!s.left.undecided() && !s.right.undecided()) {
            if (decisions.contains(key)) continue;
            const auto state = s.left.state == QLatin1String("manual") || s.right.state == QLatin1String("manual")
                ? QStringLiteral("manual")
                : s.left.state == QLatin1String("accepted") || s.right.state == QLatin1String("accepted")
                ? QStringLiteral("accepted")
                : QStringLiteral("ignored");
            const auto label = s.left.id.isEmpty() ? sideState(s.right)
                : s.right.id.isEmpty()             ? sideState(s.left)
                                                   : tr("%1, %2").arg(sideState(s.left), sideState(s.right));
            decisions.insert(key, QVariantMap{{"state", state}, {"label", label}});
        } else if (row > 0 && !s.left.id.isEmpty() && !s.right.id.isEmpty() && s.left.combinationAvailable
                   && foldedSections.contains(s.id)) {
            auto offers = combinations.value(key).toList();
            offers.append(QVariantMap{{"id", s.left.id}, {"label", tr("%1 and %2").arg(s.left.label, s.right.label)}});
            combinations.insert(key, offers);
        }
    }
    return {{"decisions", decisions}, {"combinations", combinations}};
}
}
