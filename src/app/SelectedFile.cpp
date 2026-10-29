// SelectedFile keeps immutable sources indexed while projecting changed result revisions.
#include "SelectedFile.h"
#include "SourceIdentity.h"
#include <algorithm>
#include <set>

namespace branch_truce {
namespace {
QString text(const std::string &value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

// The undecorated last path component, for syntax definition lookup.
QString baseName(const std::string &path)
{
    return text(path.substr(path.rfind('/') + 1));
}

QVariantMap sourceData(const std::optional<SourceSnapshot> &source)
{
    if (!source) return {};
    const auto commitId = source->commitId ? text(*source->commitId) : QString{};
    return {{QStringLiteral("available"), true}, {QStringLiteral("text"), text(source->worktree.utf8)},
            {QStringLiteral("label"), sourceRole(text(source->label), commitId)},
            {QStringLiteral("shortCommitId"), shortId(commitId)},
            {QStringLiteral("shortObjectId"), shortId(text(source->objectId))}, {QStringLiteral("objectId"), text(source->objectId)},
            {QStringLiteral("commitId"), source->commitId ? text(*source->commitId) : QString{}},
            {QStringLiteral("mode"), text(source->mode)}};
}

void insertUnmapped(QVariantMap &map, const QString &prefix)
{
    for (const auto *key : {"LineBegin", "LineEnd", "Utf16Begin", "Utf16End"})
        map.insert(prefix + QLatin1String(key), -1);
}

// A marker line of the result: its 1-based line and its text without the line ending.
QVariantMap markerLine(const MarkerLine &marker, std::string_view result, const TextPositions &positions)
{
    auto line = result.substr(marker.bytes.begin, marker.bytes.end - marker.bytes.begin);
    if (line.ends_with('\n')) line.remove_suffix(1);
    return {{QStringLiteral("line"), positions.line(marker.bytes.begin)},
        {QStringLiteral("text"), QString::fromUtf8(line.data(), static_cast<qsizetype>(line.size()))}};
}

QString sideName(SourceSide side)
{
    switch (side) {
    case SourceSide::Earlier: return QStringLiteral("earlier");
    case SourceSide::Stage2: return QStringLiteral("stage2");
    case SourceSide::Stage3: return QStringLiteral("stage3");
    }
    return {};
}

QString kindName(PassageKind kind)
{
    switch (kind) {
    case PassageKind::Equal: return QStringLiteral("equal");
    case PassageKind::Changed: return QStringLiteral("changed");
    case PassageKind::SourceOnly: return QStringLiteral("source-only");
    case PassageKind::ResultOnly: return QStringLiteral("result-only");
    case PassageKind::Ambiguous: return QStringLiteral("ambiguous");
    }
    return {};
}

}
SelectedFile::SelectedFile(SessionModel &files, QObject *parent) : QObject(parent), files_(files)
{
    connect(&files_, &QAbstractItemModel::modelReset, this, [this] { reset_ = true; });
}
QVariant SelectedFile::role(int role) const
{
    return row_ < 0 ? QVariant{} : files_.data(files_.index(row_), role);
}
bool SelectedFile::canEdit() const
{
    const auto *file = files_.fileAt(row_);
    return file && file->support() == Support::Supported
        && !files_.hasPendingRecovery() && !files_.hasRecoveryFailure(file->id());
}
void SelectedFile::sync(const QString &fileId)
{
    const auto *file = files_.file(fileId.toStdString());
    const bool selectionChanged = fileId_ != fileId || reset_;
    row_ = files_.rowOf(fileId.toStdString());
    if (selectionChanged) {
        fileId_ = fileId;
        path_ = file ? files_.displayPath(file->id()) : QString{};
        pathBytes_ = file ? QByteArray(file->path().data(), static_cast<qsizetype>(file->path().size())) : QByteArray{};
        fileName_ = file ? baseName(file->path()) : QString{};
        Q_EMIT identityChanged();
    }
    const auto *snapshots = file ? file->snapshots() : nullptr;
    const bool changedSources = selectionChanged || snapshots != sourceIdentity_;
    if (changedSources) {
        sourceIdentity_ = snapshots;
        stage2_ = snapshots ? sourceData(snapshots->stage2) : QVariantMap{};
        stage3_ = snapshots ? sourceData(snapshots->stage3) : QVariantMap{};
        earlier_ = snapshots ? sourceData(snapshots->earlier) : QVariantMap{};
        initialResult_ = snapshots ? QVariantMap{{QStringLiteral("text"), text(snapshots->launchResult.text.utf8)}} : QVariantMap{};
        sourcePositions_.clear();
        sourceTexts_.clear();
        if (snapshots) {
            for (const auto side : {SourceSide::Stage2, SourceSide::Stage3, SourceSide::Earlier}) {
                if (const auto &source = snapshotFor(*snapshots, side)) {
                    auto [entry, inserted] = sourceTexts_.emplace(side, source->worktree.utf8);
                    (void)inserted;
                    sourcePositions_.emplace(side, TextPositions(entry->second));
                }
            }
        }
        Q_EMIT sourcesChanged();
    }
    reset_ = false;
    update(resultText_, file ? text(file->current().draft.text) : QString{}, &SelectedFile::resultTextChanged);
    update(decisions_, role(SessionModel::DecisionsRole).toList(), &SelectedFile::decisionsChanged);
    const auto revision = currentRevision();
    const auto support = file ? file->support() : Support::Unreadable;
    const bool pending = file && files_.pendingDraft(file->id());
    if (changedSources || revision != projectedRevision_ || support != projectedSupport_ || pending != projectedPending_) {
        projectedRevision_ = revision;
        projectedSupport_ = support;
        projectedPending_ = pending;
        project(file);
    }
    Q_EMIT statusChanged();
}
void SelectedFile::project(const FileSession *file)
{
    if (!file) {
        update(passages_, QVariantList{}, &SelectedFile::passagesChanged);
        update(handEditedLines_, QVariantList{}, &SelectedFile::handEditedLinesChanged);
        update(choices_, QVariantList{}, &SelectedFile::choicesChanged);
        update(regions_, QVariantList{}, &SelectedFile::regionsChanged);
        update(markerBlocks_, QVariantList{}, &SelectedFile::markerBlocksChanged);
        update(markerOrphans_, QVariantList{}, &SelectedFile::markerOrphansChanged);
        update(choiceNotice_, QString{}, &SelectedFile::choicesChanged);
        return;
    }
    const auto *mapping = file->correspondence();
    const auto resultPositions = mapping ? std::optional<TextPositions>(std::in_place, mapping->text()) : std::nullopt;
    QVariantList passages;
    if (mapping && file->support() == Support::Supported) {
        for (const auto side : {SourceSide::Stage2, SourceSide::Stage3, SourceSide::Earlier}) {
            const auto *list = mapping->passages(side);
            const auto positions = sourcePositions_.find(side);
            if (!list || positions == sourcePositions_.end()) continue;
            for (const auto &passage : *list) {
                if (!positions->second.contains(passage.source) || !resultPositions->contains(passage.result)) continue;
                QVariantMap entry{{QStringLiteral("side"), sideName(side)}, {QStringLiteral("kind"), kindName(passage.kind)}};
                positions->second.insert(entry, QStringLiteral("source"), passage.source);
                resultPositions->insert(entry, QStringLiteral("result"), passage.result);
                passages.push_back(entry);
            }
        }
    }
    update(passages_, passages, &SelectedFile::passagesChanged);
    // Result lines editor changes wrote, inside choice regions or not. A
    // deletion marks the line it joined, or the last line after a final newline.
    QVariantList handEditedLines;
    if (mapping && file->support() == Support::Supported) {
        const auto &draft = mapping->text();
        for (auto range : mapping->handEditedRanges()) {
            if (range.begin == range.end && range.begin == draft.size() && draft.ends_with('\n'))
                range = {range.begin - 1, range.begin - 1};
            QVariantMap entry;
            resultPositions->insert(entry, QStringLiteral("result"), range);
            handEditedLines.push_back(entry);
        }
    }
    update(handEditedLines_, handEditedLines, &SelectedFile::handEditedLinesChanged);
    QVariantList choices;
    QVariantList regions;
    // A marker block tied to choice sections stays in the result until
    // confirmed choices decide every section it encloses.
    const auto sections = file->support() == Support::Supported ? file->markerSections() : std::vector<MarkerSection>{};
    std::set<std::string> markedSections;
    for (const auto &section : sections) markedSections.insert(section.sections.begin(), section.sections.end());
    for (const auto &target : file->choices()) {
        // Older recovery records retain their exact decision vocabulary and
        // status. Do not invent new undecided judgments while restoring them.
        if (std::any_of(target.decisions.begin(), target.decisions.end(), [&](const auto &id) {
            return !file->current().draft.decisions.contains(id);
        })) continue;
        const auto &source = target.source.side == SourceSide::Stage2 ? file->snapshots()->stage2 : file->snapshots()->stage3;
        const auto &sourceText = source->worktree.utf8;
        const auto &positions = sourcePositions_.at(target.source.side);
        const auto line = positions.line(target.source.range.begin);
        const auto last = positions.line(target.source.range.end > target.source.range.begin
            ? target.source.range.end - 1 : target.source.range.end);
        QStringList states;
        QStringList decisionIds;
        bool undecided = true;
        bool anyUndecided = target.decisions.empty();
        bool ignored = !target.decisions.empty();
        for (const auto &id : target.decisions) {
            const auto found = file->current().draft.decisions.find(id);
            const auto state = found == file->current().draft.decisions.end() ? DecisionValue::Undecided : found->second.value;
            undecided = undecided && state == DecisionValue::Undecided;
            anyUndecided = anyUndecided || state == DecisionValue::Undecided;
            ignored = ignored && state == DecisionValue::Ignored;
            states.push_back(state == DecisionValue::Accepted ? tr("accepted")
                : state == DecisionValue::Ignored ? tr("ignored") : tr("undecided"));
            decisionIds.push_back(text(id));
        }
        states.removeDuplicates();
        const auto *anchor = file->correspondence() ? file->correspondence()->anchor(target.source.contributionId) : nullptr;
        const bool mapped = anchor && anchor->state == AnchorState::Mapped && anchor->result;
        const bool available = undecided && mapped;
        const bool resultMapped = mapped && resultPositions->contains(*anchor->result);
        // Only editor changes set this, never text that differed at launch.
        const bool handEdited = anchor && anchor->handEdited;
        // Any undecided line keeps the region undecided, so outstanding work stays visible.
        QVariantMap region{{QStringLiteral("id"), text(target.source.contributionId)},
            {QStringLiteral("side"), sideName(target.source.side)},
            {QStringLiteral("scope"), target.scope == ChoiceScope::Section ? QStringLiteral("section") : QStringLiteral("line")},
            {QStringLiteral("section"), text(target.section)},
            {QStringLiteral("decisionIds"), decisionIds},
            {QStringLiteral("state"), anyUndecided ? QStringLiteral("undecided") : handEdited ? QStringLiteral("manual")
                : ignored ? QStringLiteral("ignored") : QStringLiteral("accepted")},
            {QStringLiteral("handEdited"), handEdited},
            {QStringLiteral("markersRemain"), markedSections.contains(target.section)},
            {QStringLiteral("mapped"), resultMapped}};
        positions.insert(region, QStringLiteral("source"), target.source.range);
        if (resultMapped) resultPositions->insert(region, QStringLiteral("result"), *anchor->result);
        else insertUnmapped(region, QStringLiteral("result"));
        regions.push_back(region);
        choices.push_back(QVariantMap{{QStringLiteral("id"), text(target.source.contributionId)},
            {QStringLiteral("label"), (target.source.side == SourceSide::Stage2 ? tr("Left") : tr("Right"))
                + (target.scope == ChoiceScope::Section ? tr(" section, lines %1–%2").arg(line).arg(last) : tr(" line %1").arg(line))},
            {QStringLiteral("state"), states.join(QStringLiteral(", "))},
            {QStringLiteral("sourceText"), text(sourceText.substr(target.source.range.begin, target.source.range.end - target.source.range.begin))},
            {QStringLiteral("available"), available},
            {QStringLiteral("combinationAvailable"), mapped},
            {QStringLiteral("detail"), !undecided ? tr("Preview both to replace this scope, or undo intervening actions to change its decision.")
                : !mapped ? tr("This scope cannot be mapped safely. Edit the result directly.") : QString{}}});
    }
    update(choices_, choices, &SelectedFile::choicesChanged);
    update(regions_, regions, &SelectedFile::regionsChanged);
    // The conflict-marker lines of the current result. The view may fold the
    // lines of a block tied to a choice section; other marker lines stay
    // shown. A pending draft in the editor has other lines, so it gets none.
    QVariantList markerBlocks;
    QVariantList markerOrphans;
    if (file->support() == Support::Supported && !files_.pendingDraft(file->id())) {
        const auto &draft = file->current().draft.text;
        const TextPositions lines(draft);
        const auto scan = file->markers();
        for (const auto &block : scan.blocks) {
            QVariantList markerLines{markerLine(block.begin, draft, lines)};
            if (block.base) markerLines.push_back(markerLine(*block.base, draft, lines));
            markerLines.push_back(markerLine(block.separator, draft, lines));
            markerLines.push_back(markerLine(block.end, draft, lines));
            const bool folded = std::any_of(sections.begin(), sections.end(),
                [&](const auto &section) { return section.block == block; });
            markerBlocks.push_back(QVariantMap{{QStringLiteral("resultLineBegin"), lines.line(block.begin.bytes.begin)},
                {QStringLiteral("resultLineEnd"), lines.line(block.end.bytes.begin) + 1},
                {QStringLiteral("folded"), folded}, {QStringLiteral("markerLines"), markerLines}});
        }
        for (const auto &orphan : scan.orphans) markerOrphans.push_back(markerLine(orphan, draft, lines));
    }
    update(markerBlocks_, markerBlocks, &SelectedFile::markerBlocksChanged);
    update(markerOrphans_, markerOrphans, &SelectedFile::markerOrphansChanged);
    update(choiceNotice_, choices_.empty() && !file->choices().empty()
        ? tr("Scoped choices are unavailable for this earlier draft. Direct editing remains available.") : QString{}, &SelectedFile::choicesChanged);
}
}
