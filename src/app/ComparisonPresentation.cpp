// Group domain scopes once, leaving only pane geometry and interaction in QML.
#include "ComparisonPresentation.h"
#include "core/SessionState.h"
#include <algorithm>

namespace branch_truce {
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
    Q_EMIT changed();
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
    const auto sideState = [&](const ComparisonContribution &c) {
        const bool left = c.side == QLatin1String("stage2");
        if (c.state == QLatin1String("accepted")) return left ? tr("Left accepted") : tr("Right accepted");
        if (c.state == QLatin1String("ignored")) return left ? tr("Left ignored") : tr("Right ignored");
        if (c.state == QLatin1String("manual")) return left ? tr("Left edited by hand") : tr("Right edited by hand");
        return left ? tr("Left undecided") : tr("Right undecided");
    };
    // Result actions use the mapped contribution range, not the wider marker
    // span used to select the active conflict at the editor cursor.
    for (const auto &s : sections_) {
        int first = -1;
        for (const auto *c : {&s.left, &s.right})
            if (c->mapped && c->resultLineBegin > 0 && (first < 0 || c->resultLineBegin < first))
                first = c->resultLineBegin;
        if (first < 0) continue;
        int row = 0;
        QStringList foldedSections;
        if (!markersShown)
            for (const auto &value : data_.markerBlocks) {
                const auto block = value.toMap();
                const auto lines = block.value("markerLines").toList();
                if (!block.value("folded").toBool() || lines.size() < 2) continue;
                const int begin = lines.first().toMap().value("line").toInt();
                const int end = lines.last().toMap().value("line").toInt();
                if (first < begin || first > end) continue;
                row = begin;
                foldedSections = block.value("sections").toStringList();
                break;
            }
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
