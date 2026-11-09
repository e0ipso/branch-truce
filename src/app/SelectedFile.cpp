// SelectedFile keeps immutable sources indexed while projecting changed result revisions.
#include "SelectedFile.h"
#include "QtText.h"
#include "Selection.h"
#include "SourceIdentity.h"
#include <algorithm>
#include <set>
#include <utility>

namespace branch_truce {
using qt_text::baseName;
using qt_text::utf8;
namespace {

QVariantMap sourceData(const std::optional<SourceSnapshot> &source)
{
    if (!source) return {};
    const auto commitId = source->commitId ? utf8(*source->commitId) : QString{};
    return {{QStringLiteral("available"), true},
            {QStringLiteral("text"), utf8(source->worktree.utf8)},
            {QStringLiteral("label"), sourceRole(utf8(source->label), commitId)},
            {QStringLiteral("shortCommitId"), shortId(commitId)},
            {QStringLiteral("shortObjectId"), shortId(utf8(source->objectId))},
            {QStringLiteral("objectId"), utf8(source->objectId)},
            {QStringLiteral("commitId"), source->commitId ? utf8(*source->commitId) : QString{}},
            {QStringLiteral("mode"), utf8(source->mode)}};
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
SelectedFile::SelectedFile(const SessionModel &files, const Selection &selection, QObject *parent)
    : QObject(parent), files_(files), selection_(selection)
{
    connect(&files_, &QAbstractItemModel::modelReset, this, [this] { reset_ = true; });
    connect(&selection_, &Selection::changed, this, &SelectedFile::sync);
    connect(&files_, &SessionModel::summaryChanged, this, &SelectedFile::sync);
}
QVariant SelectedFile::role(int role) const
{
    const auto row = files_.rowOf(fileId_.toStdString());
    return row < 0 ? QVariant{} : files_.data(files_.index(row), role);
}
QString SelectedFile::mimeIcon() const
{
    const auto icon = role(SessionModel::MimeIconRole).toString();
    return icon.isEmpty() ? QStringLiteral("unknown") : icon;
}
QString SelectedFile::mimeDescription() const { return role(SessionModel::MimeDescriptionRole).toString(); }
QString SelectedFile::formatDescription() const
{
    const auto *file = files_.store().file(fileId_.toStdString());
    const auto *snapshots = file ? file->snapshots() : nullptr;
    if (!snapshots || file->support() != Support::Supported) return {};
    const auto &format = snapshots->launchResult.text.format;
    QString encoding;
    switch (format.encoding) {
    case SnapshotFormat::Encoding::Utf8: encoding = QStringLiteral("UTF-8"); break;
    case SnapshotFormat::Encoding::Utf16LittleEndian: encoding = QStringLiteral("UTF-16 LE"); break;
    case SnapshotFormat::Encoding::Utf16BigEndian: encoding = QStringLiteral("UTF-16 BE"); break;
    }
    const auto newline =
        format.newline == SnapshotFormat::Newline::CrLf ? QStringLiteral("CRLF") : QStringLiteral("LF");
    return format.bom ? tr("%1 with BOM · %2").arg(encoding, newline) : tr("%1 · %2").arg(encoding, newline);
}
bool SelectedFile::canEdit() const
{
    const auto *file = files_.store().file(fileId_.toStdString());
    return file && file->support() == Support::Supported && !files_.store().hasPendingRecovery()
        && !files_.store().hasRecoveryFailure(file->id());
}
void SelectedFile::sync()
{
    const auto fileId = selection_.fileId();
    const auto *file = files_.store().file(fileId.toStdString());
    const bool selectionChanged = fileId_ != fileId || reset_;
    if (selectionChanged) {
        fileId_ = fileId;
        path_ = file ? files_.displayPath(file->id()) : QString{};
        pathBytes_ = file ? QByteArray(file->path().data(), static_cast<qsizetype>(file->path().size())) : QByteArray{};
        fileName_ = file ? baseName(file->path()) : QString{};
        Q_EMIT identityChanged();
    }
    // A large file shows no text until the user loads it.
    const bool held = file && file->support() == Support::LargeFile;
    const auto *snapshots = file && !held ? file->snapshots() : nullptr;
    const bool changedSources = selectionChanged || snapshots != sourceIdentity_;
    if (changedSources) {
        sourceIdentity_ = snapshots;
        stage2_ = snapshots ? sourceData(snapshots->stage2) : QVariantMap{};
        stage3_ = snapshots ? sourceData(snapshots->stage3) : QVariantMap{};
        earlier_ = snapshots ? sourceData(snapshots->earlier) : QVariantMap{};
        initialResult_ =
            snapshots ? QVariantMap{{QStringLiteral("text"), utf8(snapshots->launchResult.text.utf8)}} : QVariantMap{};
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
    update(resultText_, file && !held ? utf8(file->current().draft.text) : QString{}, &SelectedFile::resultTextChanged);
    update(decisions_, role(SessionModel::DecisionsRole).toList(), &SelectedFile::decisionsChanged);
    const auto revision = currentRevision();
    const auto support = file ? file->support() : Support::Unreadable;
    const bool pending = file && files_.store().pendingDraft(file->id());
    if (changedSources || revision != projectedRevision_ || support != projectedSupport_
        || pending != projectedPending_) {
        projectedRevision_ = revision;
        projectedSupport_ = support;
        projectedPending_ = pending;
        project(file);
    }
    // Every status property reads one of these roles, plus canEdit's session-wide recovery.
    QVariantList status;
    for (const auto role :
         {SessionModel::SupportRole,       SessionModel::SupportDetailRole,  SessionModel::StatusRole,
          SessionModel::StatusKindRole,    SessionModel::UnsavedRole,        SessionModel::ReviewNeededRole,
          SessionModel::ResolvedRole,      SessionModel::UndecidedRole,      SessionModel::ConflictCountRole,
          SessionModel::PendingInputRole,  SessionModel::PendingTextRole,    SessionModel::RecoveryStateRole,
          SessionModel::RecoveryErrorRole, SessionModel::FinishBlockersRole, SessionModel::CurrentRevisionRole,
          SessionModel::CanUndoRole,       SessionModel::CanRedoRole,        SessionModel::ProgressRole,
          SessionModel::StatusIconRole,    SessionModel::StatusLineRole,     SessionModel::ChipsRole,
          SessionModel::SummaryRole})
        status.push_back(this->role(role));
    status.push_back(canEdit());
    if (status != status_ || selectionChanged) {
        status_ = std::move(status);
        Q_EMIT statusChanged();
    }
}
void SelectedFile::project(const FileSession *file)
{
    if (!file) {
        publishProjection({});
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
                QVariantMap entry{{QStringLiteral("side"), sideName(side)},
                                  {QStringLiteral("kind"), kindName(passage.kind)}};
                positions->second.insert(entry, QStringLiteral("source"), passage.source);
                resultPositions->insert(entry, QStringLiteral("result"), passage.result);
                passages.push_back(entry);
            }
        }
    }
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
        if (std::any_of(target.decisions.begin(), target.decisions.end(),
                        [&](const auto &id) { return !file->current().draft.decisions.contains(id); }))
            continue;
        const auto &source =
            target.source.side == SourceSide::Stage2 ? file->snapshots()->stage2 : file->snapshots()->stage3;
        const auto &sourceText = source->worktree.utf8;
        const auto &positions = sourcePositions_.at(target.source.side);
        const auto line = positions.line(target.source.range.begin);
        const auto last =
            positions.line(target.source.range.end > target.source.range.begin ? target.source.range.end - 1
                                                                               : target.source.range.end);
        QStringList states;
        QStringList decisionIds;
        bool undecided = true;
        bool anyUndecided = target.decisions.empty();
        bool ignored = !target.decisions.empty();
        for (const auto &id : target.decisions) {
            const auto found = file->current().draft.decisions.find(id);
            const auto state =
                found == file->current().draft.decisions.end() ? DecisionValue::Undecided : found->second.value;
            undecided = undecided && state == DecisionValue::Undecided;
            anyUndecided = anyUndecided || state == DecisionValue::Undecided;
            ignored = ignored && state == DecisionValue::Ignored;
            states.push_back(state == DecisionValue::Accepted      ? tr("accepted")
                                 : state == DecisionValue::Ignored ? tr("ignored")
                                                                   : tr("undecided"));
            decisionIds.push_back(utf8(id));
        }
        states.removeDuplicates();
        const auto *anchor =
            file->correspondence() ? file->correspondence()->anchor(target.source.contributionId) : nullptr;
        const bool mapped = anchor && anchor->state == AnchorState::Mapped && anchor->result;
        const bool available = undecided && mapped;
        const bool resultMapped = mapped && resultPositions->contains(*anchor->result);
        // Only editor changes set this, never text that differed at launch.
        const bool handEdited = anchor && anchor->handEdited;
        // Any undecided line keeps the region undecided, so outstanding work stays visible.
        QVariantMap region{{QStringLiteral("id"), utf8(target.source.contributionId)},
                           {QStringLiteral("side"), sideName(target.source.side)},
                           {QStringLiteral("scope"),
                            target.scope == ChoiceScope::Section ? QStringLiteral("section") : QStringLiteral("line")},
                           {QStringLiteral("section"), utf8(target.section)},
                           {QStringLiteral("decisionIds"), decisionIds},
                           {QStringLiteral("state"),
                            anyUndecided     ? QStringLiteral("undecided")
                                : handEdited ? QStringLiteral("manual")
                                : ignored    ? QStringLiteral("ignored")
                                             : QStringLiteral("accepted")},
                           {QStringLiteral("handEdited"), handEdited},
                           {QStringLiteral("markersRemain"), markedSections.contains(target.section)},
                           {QStringLiteral("mapped"), resultMapped}};
        positions.insert(region, QStringLiteral("source"), target.source.range);
        if (resultMapped)
            resultPositions->insert(region, QStringLiteral("result"), *anchor->result);
        else
            insertUnmapped(region, QStringLiteral("result"));
        regions.push_back(region);
        const bool left = target.source.side == SourceSide::Stage2;
        const auto label = target.scope == ChoiceScope::Section
            ? (left ? tr("Left section, lines %1–%2") : tr("Right section, lines %1–%2")).arg(line).arg(last)
            : (left ? tr("Left line %1") : tr("Right line %1")).arg(line);
        choices.push_back(QVariantMap{
            {QStringLiteral("id"), utf8(target.source.contributionId)},
            {QStringLiteral("label"), label},
            {QStringLiteral("state"), states.join(QStringLiteral(", "))},
            {QStringLiteral("sourceText"),
             utf8(sourceText.substr(target.source.range.begin, target.source.range.end - target.source.range.begin))},
            {QStringLiteral("available"), available},
            {QStringLiteral("combinationAvailable"), mapped},
            {QStringLiteral("detail"),
             !undecided ? tr("Preview both to replace this scope, or undo intervening actions to change its decision.")
                 : !mapped ? tr("This scope cannot be mapped safely. Edit the result directly.")
                           : QString{}}});
    }
    // Fold recognized blocks independently of whether a choice can safely
    // target them. A pending editor draft has different lines, so it gets none.
    QVariantList markerBlocks;
    QVariantList markerOrphans;
    if (file->support() == Support::Supported && !files_.store().pendingDraft(file->id())) {
        const auto &draft = file->current().draft.text;
        const TextPositions lines(draft);
        const auto scan = file->markers();
        for (const auto &block : scan.blocks) {
            QVariantList markerLines{markerLine(block.begin, draft, lines)};
            if (block.base) markerLines.push_back(markerLine(*block.base, draft, lines));
            markerLines.push_back(markerLine(block.separator, draft, lines));
            markerLines.push_back(markerLine(block.end, draft, lines));
            QStringList blockSections;
            for (const auto &section : sections)
                if (section.block == block)
                    for (const auto &id : section.sections) blockSections.push_back(utf8(id));
            markerBlocks.push_back(QVariantMap{{QStringLiteral("resultLineBegin"), lines.line(block.begin.bytes.begin)},
                                               {QStringLiteral("resultLineEnd"), lines.line(block.end.bytes.begin) + 1},
                                               {QStringLiteral("folded"), true},
                                               {QStringLiteral("markerLines"), markerLines},
                                               {QStringLiteral("sections"), blockSections}});
        }
        for (const auto &orphan : scan.orphans) markerOrphans.push_back(markerLine(orphan, draft, lines));
    }
    const auto notice = choices.empty() && !file->choices().empty()
        ? tr("Scoped choices are unavailable for this earlier draft. Direct editing remains available.")
        : QString{};
    publishProjection({std::move(passages), std::move(handEditedLines), std::move(choices), std::move(regions),
                       std::move(markerBlocks), std::move(markerOrphans), notice});
}

void SelectedFile::publishProjection(Projection projection)
{
    const bool passages = passages_ != projection.passages;
    const bool handEditedLines = handEditedLines_ != projection.handEditedLines;
    const bool choices = choices_ != projection.choices || choiceNotice_ != projection.choiceNotice;
    const bool regions = regions_ != projection.regions;
    const bool markerBlocks = markerBlocks_ != projection.markerBlocks;
    const bool markerOrphans = markerOrphans_ != projection.markerOrphans;
    if (!passages && !handEditedLines && !choices && !regions && !markerBlocks && !markerOrphans) return;

    // A QML binding often reads several of these fields. Install the complete
    // revision first, then invalidate that binding once through a shared notifier.
    passages_ = std::move(projection.passages);
    handEditedLines_ = std::move(projection.handEditedLines);
    choices_ = std::move(projection.choices);
    choiceNotice_ = std::move(projection.choiceNotice);
    regions_ = std::move(projection.regions);
    markerBlocks_ = std::move(projection.markerBlocks);
    markerOrphans_ = std::move(projection.markerOrphans);
    Q_EMIT comparisonChanged();

    // Preserve focused observers, with the same coherent state as QML receives.
    if (passages) Q_EMIT passagesChanged();
    if (handEditedLines) Q_EMIT handEditedLinesChanged();
    if (choices) Q_EMIT choicesChanged();
    if (regions) Q_EMIT regionsChanged();
    if (markerBlocks) Q_EMIT markerBlocksChanged();
    if (markerOrphans) Q_EMIT markerOrphansChanged();
}
}
