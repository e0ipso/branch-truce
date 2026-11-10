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
        if (snapshots) {
            for (const auto side : {SourceSide::Stage2, SourceSide::Stage3, SourceSide::Earlier}) {
                if (const auto &source = snapshotFor(*snapshots, side)) {
                    sourcePositions_.emplace(side, TextPositions(source->worktree.utf8));
                }
            }
        }
        Q_EMIT sourcesChanged();
    }
    reset_ = false;
    update(resultText_, file && !held ? utf8(file->current().draft.text) : QString{}, &SelectedFile::resultTextChanged);
    update(decisions_, role(SessionModel::DecisionsRole).toList(), &SelectedFile::decisionsChanged);
    const auto revision = currentRevision();
    const auto correspondence = file ? file->current().correspondence : nullptr;
    const auto support = file ? file->support() : Support::Unreadable;
    const bool pending = file && files_.store().pendingDraft(file->id());
    if (changedSources || revision != projectedRevision_ || support != projectedSupport_
        || correspondence != projectedCorrespondence_ || pending != projectedPending_) {
        projectedRevision_ = revision;
        projectedCorrespondence_ = correspondence;
        projectedSupport_ = support;
        projectedPending_ = pending;
        project(file);
    }
    // Every status property reads one of these roles, plus canEdit's session-wide recovery.
    QVariantList status;
    for (const auto role : {SessionModel::SupportRole, SessionModel::SupportDetailRole, SessionModel::StatusRole,
                            SessionModel::StatusKindRole, SessionModel::UnsavedRole, SessionModel::ResolvedRole,
                            SessionModel::UndecidedRole, SessionModel::PendingInputRole, SessionModel::PendingTextRole,
                            SessionModel::RecoveryStateRole, SessionModel::RecoveryErrorRole,
                            SessionModel::CurrentRevisionRole, SessionModel::CanUndoRole, SessionModel::CanRedoRole,
                            SessionModel::ProgressRole, SessionModel::StatusIconRole})
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
        comparison_.publish({});
        return;
    }
    const auto *mapping = file->correspondence();
    const auto resultPositions = mapping ? std::optional<TextPositions>(std::in_place, mapping->text()) : std::nullopt;
    ComparisonPresentation::Data projection;
    projection.fileId = fileId_;
    projection.revision = currentRevision();
    projection.correspondence = correspondenceIdentity(*file);
    auto &passages = projection.passages;
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
    auto &handEditedLines = projection.handEditedLines;
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
        const auto &positions = sourcePositions_.at(target.source.side);
        const auto line = positions.line(target.source.range.begin);
        const auto last =
            positions.line(target.source.range.end > target.source.range.begin ? target.source.range.end - 1
                                                                               : target.source.range.end);
        QStringList states, decisionIds;
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
        const auto *anchor =
            file->correspondence() ? file->correspondence()->anchor(target.source.contributionId) : nullptr;
        const bool mapped = anchor && anchor->state == AnchorState::Mapped && anchor->result;
        const bool available = undecided && mapped;
        const bool resultMapped = mapped && resultPositions->contains(*anchor->result);
        // Only editor changes set this, never text that differed at launch.
        const bool handEdited = anchor && anchor->handEdited;
        ComparisonContribution contribution;
        contribution.id = utf8(target.source.contributionId);
        contribution.side = sideName(target.source.side);
        contribution.scope = target.scope == ChoiceScope::Section ? QStringLiteral("section") : QStringLiteral("line");
        contribution.section = utf8(target.section);
        contribution.decisionIds = decisionIds;
        states.removeDuplicates();
        contribution.decisionState = states.join(QStringLiteral(", "));
        contribution.state = anyUndecided ? QStringLiteral("undecided")
            : handEdited                  ? QStringLiteral("manual")
            : ignored                     ? QStringLiteral("ignored")
                                          : QStringLiteral("accepted");
        contribution.handEdited = handEdited;
        contribution.markersRemain = markedSections.contains(target.section);
        contribution.mapped = resultMapped;
        const auto range = [](const TextPositions &positions, ByteRange bytes, int &first, int &end, int &beginUtf,
                              int &endUtf) {
            first = positions.line(bytes.begin);
            end = bytes.end > bytes.begin ? positions.line(bytes.end - 1) + 1 : first;
            beginUtf = positions.utf16(bytes.begin);
            endUtf = positions.utf16(bytes.end);
        };
        range(positions, target.source.range, contribution.sourceLineBegin, contribution.sourceLineEnd,
              contribution.sourceUtf16Begin, contribution.sourceUtf16End);
        if (resultMapped)
            range(*resultPositions, *anchor->result, contribution.resultLineBegin, contribution.resultLineEnd,
                  contribution.resultUtf16Begin, contribution.resultUtf16End);
        const bool left = target.source.side == SourceSide::Stage2;
        contribution.label = target.scope == ChoiceScope::Section
            ? (left ? tr("Left section, lines %1–%2") : tr("Right section, lines %1–%2")).arg(line).arg(last)
            : (left ? tr("Left line %1") : tr("Right line %1")).arg(line);
        contribution.available = available;
        contribution.combinationAvailable = mapped;
        contribution.detail = !undecided
            ? tr("Preview both to replace this scope, or undo intervening actions to change its decision.")
            : !mapped ? tr("This scope cannot be mapped safely. Edit the result directly.")
                      : QString{};
        projection.contributions.append(std::move(contribution));
    }
    // Fold recognized blocks independently of whether a choice can safely
    // target them. A pending editor draft has different lines, so it gets none.
    auto &markerBlocks = projection.markerBlocks;
    auto &markerOrphans = projection.markerOrphans;
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
    projection.choiceNotice = projection.contributions.empty() && !file->choices().empty()
        ? tr("Scoped choices are unavailable for this earlier draft. Direct editing remains available.")
        : QString{};
    comparison_.publish(std::move(projection));
}
}
