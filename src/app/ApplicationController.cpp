#include "ApplicationController.h"
#include "NavigationTargets.h"
#include "RecoveryRestore.h"
#include "TextPositions.h"
#include "core/Correspondence.h"
#include <QDir>
#include <QJSEngine>
#include "core/BuildInfo.h"
#include <QStandardPaths>
#include <algorithm>
#include <set>
#include <string_view>
#include <vector>

namespace branch_truce {
namespace {
QString text(const std::string &value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

// value as a script value, with every QVariantMap a plain object and every QVariantList an array.
QJSValue scriptValue(QJSEngine &engine, const QVariant &value)
{
    if (value.metaType() == QMetaType::fromType<QVariantMap>()) {
        auto object = engine.newObject();
        const auto map = value.toMap();
        for (auto entry = map.cbegin(); entry != map.cend(); ++entry)
            object.setProperty(entry.key(), scriptValue(engine, entry.value()));
        return object;
    }
    if (value.metaType() == QMetaType::fromType<QVariantList>()) {
        const auto list = value.toList();
        auto array = engine.newArray(static_cast<quint32>(list.size()));
        for (qsizetype index = 0; index < list.size(); ++index)
            array.setProperty(static_cast<quint32>(index), scriptValue(engine, list.at(index)));
        return array;
    }
    return engine.toScriptValue(value);
}

// The undecorated last path component, for syntax definition lookup.
QString fileName(const std::string &path)
{
    return text(path.substr(path.rfind('/') + 1));
}

QVariantMap sourceData(const std::optional<SourceSnapshot> &source)
{
    if (!source) return {};
    return {{QStringLiteral("available"), true}, {QStringLiteral("text"), text(source->worktree.utf8)},
            {QStringLiteral("label"), text(source->label)}, {QStringLiteral("objectId"), text(source->objectId)},
            {QStringLiteral("commitId"), source->commitId ? text(*source->commitId) : QString{}},
            {QStringLiteral("mode"), text(source->mode)}};
}

std::optional<RevisionId> revisionId(const QString &value)
{
    bool ok = false;
    const auto id = value.toULongLong(&ok);
    return ok && id != 0 && QString::number(id) == value ? std::optional<RevisionId>{id} : std::nullopt;
}

bool scalarBoundary(const QString &value, int offset)
{
    return offset >= 0 && offset <= value.size()
        && !(offset > 0 && offset < value.size() && value.at(offset).isLowSurrogate()
             && value.at(offset - 1).isHighSurrogate());
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

ApplicationController::ApplicationController(QObject *parent)
    : QObject(parent), files_(this),
      coordinator_(files_, QStandardPaths::findExecutable(QStringLiteral("git")), this),
      publisher_(files_, coordinator_, QStandardPaths::findExecutable(QStringLiteral("git")), this),
      mergetool_(publisher_, files_, coordinator_, [this] { return loader_.repository(); }, this),
      editor_(files_, [this] { return selectedFileId_; },
          [this](const FileSession *file, const QString &loaded) { return firstUndecidedPlacement(file, loaded); }, this),
      loader_(files_, coordinator_, SessionLoader::Owners{
          [this](const std::string &path) { return displayBytes(path); },
          [this] { return selectedFileId_; },
          [this] { return !sessionOutcome_.isEmpty() || publisher_.busy(); },
          [this] { mergetool_.clearCopiedLaunches(); },
          [this] { mergetool_.reassign(); },
          [this](const git::RepositoryContext &repository, std::map<std::string, SnapshotFileIdentity> copiedLaunches,
                 QString *error) {
              publisher_.captureBaselines(repository);
              mergetool_.adoptCopiedLaunches(std::move(copiedLaunches));
              // A launcher can already wait here after an in-window recovery choice.
              mergetool_.acceptCopiedLaunches();
              return mergetool_.listen(repository, error);
          }}, this),
      navigator_(files_, editor_, FileNavigator::Owners{
          [this] { return selectedFileId_; },
          [this] { return selectedRow(); },
          [this](const QString &fileId) { return selectFile(fileId); },
          [this](const QString &fileId) { return displayPath(fileId); }}, this),
      choices_(files_, editor_, ChoicePreviewFlow::Owners{
          [this] { return selectedFileId_; },
          [this](const QString &fileId) { return displayPath(fileId); },
          [this](Transition result) { return commandResult(result); },
          [this](const QString &message) { commandMessage_ = message; }}, this)
{
    connect(&editor_, &ResultEditorAdapter::edited, this, &ApplicationController::editResult);
    connect(&editor_, &ResultEditorAdapter::undoRequested, this, &ApplicationController::undo);
    connect(&editor_, &ResultEditorAdapter::redoRequested, this, &ApplicationController::redo);
    connect(&editor_, &ResultEditorAdapter::editRejected, this, [this] { commandResult(Transition::InvalidAnchor); });
    connect(&editor_, &ResultEditorAdapter::notice, this, [this](const QString &message) {
        commandMessage_ = message;
        Q_EMIT commandChanged();
    });
    connect(&editor_, &ResultEditorAdapter::loadRefused, this, [this](const QString &message) {
        if (commandMessage_ == message) return;
        commandMessage_ = message;
        Q_EMIT commandChanged();
    });
    connect(&editor_, &ResultEditorAdapter::placed, this, &ApplicationController::editorPlaced);
    connect(&navigator_, &FileNavigator::changed, this, &ApplicationController::navigationChanged);
    connect(&choices_, &ChoicePreviewFlow::changed, this, &ApplicationController::commandChanged);
    connect(&loader_, &SessionLoader::notice, this, [this](const QString &message) {
        commandMessage_ = message;
        Q_EMIT commandChanged();
    });
    connect(&publisher_, &ResolutionPublisher::changed, this, [this](const QString &message) {
        commandMessage_ = message;
        mergetool_.onPublisherChanged();
        Q_EMIT commandChanged();
    });
    connect(&mergetool_, &MergetoolSession::notice, this, [this](const QString &message) {
        commandMessage_ = message;
        Q_EMIT commandChanged();
    });
    connect(&mergetool_, &MergetoolSession::changed, this, [this] { Q_EMIT commandChanged(); });
    connect(&files_, &SessionModel::summaryChanged, this, [this] {
        if (!files_.file(selectedFileId_.toStdString())) {
            const auto preferred = loader_.preferredFileId();
            if (!preferred.isEmpty() && files_.file(preferred.toStdString())) {
                selectedFileId_ = preferred;
                loader_.clearPreferredFileId();
            } else {
                selectedFileId_ = files_.totalFiles() > 0
                    ? files_.data(files_.index(0), SessionModel::FileIdRole).toString() : QString{};
            }
        }
        // Retry publishes the pending discard. The failure sentence must not
        // remain once that restored result is acknowledged.
        if (discardPending_ && !files_.hasPendingRecovery()) {
            discardPending_ = false;
            commandMessage_ = discardedMessage(files_.file(selectedFileId_.toStdString()));
            Q_EMIT commandChanged();
        }
        if (stagedDiscardPending_ && !files_.hasPendingRecovery()) {
            stagedDiscardPending_ = false;
            commandMessage_ = stagedDiscardMessage();
            Q_EMIT commandChanged();
        }
        Q_EMIT inventoryChanged();
        Q_EMIT selectedFileChanged();
    });
    connect(this, &ApplicationController::selectedFileChanged, this, [this] {
        const auto *file = files_.file(selectedFileId_.toStdString());
        editor_.setSyntaxFileName(file ? fileName(file->path()) : QString{});
    });
    connect(this, &ApplicationController::selectedFileChanged, &editor_, &ResultEditorAdapter::sync);
    // The mergetool waiting state and the parent staging path follow the selection.
    connect(this, &ApplicationController::selectedFileChanged, this, &ApplicationController::commandChanged);
    connect(&files_, &SessionModel::summaryChanged, this, &ApplicationController::handoffChanged);
    connect(this, &ApplicationController::commandChanged, this, &ApplicationController::handoffChanged);
}
QString ApplicationController::version() const
{
    return QString::fromUtf8(branch_truce::version().data(),
                             static_cast<qsizetype>(branch_truce::version().size()));
}

QString ApplicationController::displayPath(const QString &fileId) const
{
    const auto *file = files_.file(fileId.toStdString());
    return file ? displayBytes(file->path()) : QString{};
}

QString ApplicationController::displayBytes(const std::string &path) const
{
    auto display = text(path);
    const QByteArray bytes(path.data(), static_cast<qsizetype>(path.size()));
    const bool exact = display.toUtf8() == bytes;
    display.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    display.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    display.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    display.replace(QLatin1Char('\t'), QStringLiteral("\\t"));
    return exact ? display : tr("%1 [path bytes: %2]").arg(display, QString::fromLatin1(bytes.toHex()));
}

QJSValue ApplicationController::selectedFileScript() const
{
    auto *engine = qjsEngine(this);
    return engine ? scriptValue(*engine, selectedFile()) : QJSValue{};
}

QVariantMap ApplicationController::selectedFile() const
{
    const auto *file = files_.file(selectedFileId_.toStdString());
    if (!file) return {};
    QString support;
    switch (file->support()) {
    case Support::Supported: support = QStringLiteral("supported"); break;
    case Support::OutsideApp: support = QStringLiteral("outside-app"); break;
    case Support::Unreadable: support = QStringLiteral("unreadable"); break;
    }
    const auto &path = file->path();
    QVariantMap result{{QStringLiteral("fileId"), selectedFileId_},
        {QStringLiteral("path"), displayPath(selectedFileId_)},
        {QStringLiteral("pathBytes"), QByteArray(path.data(), static_cast<qsizetype>(path.size()))},
        {QStringLiteral("fileName"), fileName(path)},
        {QStringLiteral("support"), support}, {QStringLiteral("supportDetail"), text(file->supportDetail())},
        {QStringLiteral("resultText"), text(file->current().draft.text)}};
    for (int row = 0; row < files_.totalFiles(); ++row) {
        const auto index = files_.index(row);
        if (files_.data(index, SessionModel::FileIdRole).toString() != selectedFileId_) continue;
        for (const auto role : {SessionModel::StatusRole, SessionModel::UnsavedRole, SessionModel::ReviewNeededRole,
             SessionModel::ResolvedRole, SessionModel::UndecidedRole, SessionModel::PendingInputRole,
             SessionModel::PendingTextRole, SessionModel::RecoveryStateRole, SessionModel::RecoveryErrorRole,
             SessionModel::FinishBlockersRole, SessionModel::CurrentRevisionRole, SessionModel::CanUndoRole,
             SessionModel::CanRedoRole, SessionModel::ProgressRole, SessionModel::DecisionsRole})
            result.insert(QString::fromLatin1(files_.roleNames().value(role)), files_.data(index, role));
        break;
    }
    if (const auto &snapshots = file->snapshots(); snapshots && file->support() == Support::Supported) {
        result.insert(QStringLiteral("stage2"), sourceData(snapshots->stage2));
        result.insert(QStringLiteral("stage3"), sourceData(snapshots->stage3));
        result.insert(QStringLiteral("earlier"), sourceData(snapshots->earlier));
        result.insert(QStringLiteral("initialResult"), QVariantMap{
            {QStringLiteral("text"), text(snapshots->launchResult.text.utf8)}});
    }
    result.insert(QStringLiteral("canEdit"), file->support() == Support::Supported
        && !files_.hasPendingRecovery() && !files_.hasRecoveryFailure(file->id()));
    // Sources and the result revision the correspondence was computed for.
    // Each passage and mapped anchor range is an offset into these texts.
    std::map<SourceSide, TextPositions> sources;
    if (file->snapshots()) {
        for (const auto side : {SourceSide::Stage2, SourceSide::Stage3, SourceSide::Earlier})
            if (const auto &source = snapshotFor(*file->snapshots(), side))
                sources.emplace(side, TextPositions(source->worktree.utf8));
    }
    const auto *mapping = file->correspondence();
    const auto resultPositions = mapping ? std::optional<TextPositions>(std::in_place, mapping->text()) : std::nullopt;
    QVariantList passages;
    if (mapping && file->support() == Support::Supported) {
        for (const auto side : {SourceSide::Stage2, SourceSide::Stage3, SourceSide::Earlier}) {
            const auto *list = mapping->passages(side);
            const auto positions = sources.find(side);
            if (!list || positions == sources.end()) continue;
            for (const auto &passage : *list) {
                if (!positions->second.contains(passage.source) || !resultPositions->contains(passage.result)) continue;
                QVariantMap entry{{QStringLiteral("side"), sideName(side)}, {QStringLiteral("kind"), kindName(passage.kind)}};
                positions->second.insert(entry, QStringLiteral("source"), passage.source);
                resultPositions->insert(entry, QStringLiteral("result"), passage.result);
                passages.push_back(entry);
            }
        }
    }
    result.insert(QStringLiteral("passages"), passages);
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
        const auto &positions = sources.at(target.source.side);
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
    result.insert(QStringLiteral("choices"), choices);
    result.insert(QStringLiteral("regions"), regions);
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
    result.insert(QStringLiteral("markerBlocks"), markerBlocks);
    result.insert(QStringLiteral("markerOrphans"), markerOrphans);
    result.insert(QStringLiteral("choiceNotice"), choices.empty() && !file->choices().empty()
        ? tr("Scoped choices are unavailable for this earlier draft. Direct editing remains available.") : QString{});
    return result;
}

bool ApplicationController::selectFile(const QString &fileId)
{
    if (!files_.file(fileId.toStdString())) return false;
    if (selectedFileId_ == fileId) return true;
    if (editor_.composing()) return false;
    cancelChoice();
    selectedFileId_ = fileId;
    navigator_.selectionChanged();
    Q_EMIT selectedFileChanged();
    return true;
}

int ApplicationController::selectedRow() const
{
    for (int row = 0; row < files_.totalFiles(); ++row)
        if (files_.data(files_.index(row), SessionModel::FileIdRole).toString() == selectedFileId_)
            return row;
    return -1;
}

QVariantMap ApplicationController::commandPreview() const
{
    return choices_.view(ChoicePreviewFlow::Scope::Choice);
}

bool ApplicationController::commandResult(Transition result)
{
    switch (result) {
    case Transition::Applied: commandMessage_.clear(); break;
    case Transition::NoChange: commandMessage_ = tr("No change."); break;
    case Transition::StaleRevision: commandMessage_ = tr("The result changed. Select the scope again."); break;
    case Transition::ConfirmationRequired: commandMessage_ = tr("Inspect and confirm the exact replacement first."); break;
    case Transition::AmbiguousTarget: commandMessage_ = tr("The target is ambiguous. Edit the result directly."); break;
    case Transition::InvalidAnchor: commandMessage_ = tr("The target is no longer valid. Select the scope again."); break;
    default: commandMessage_ = tr("Command not completed. Check pending recovery and the selected scope."); break;
    }
    Q_EMIT commandChanged();
    return result == Transition::Applied || result == Transition::NoChange;
}

bool ApplicationController::previewChoice(const QString &fileId, const QString &revision,
                                         const QString &target, const QString &action)
{
    return choices_.preview(ChoicePreviewFlow::Scope::Choice, fileId, revisionId(revision), action, target);
}

bool ApplicationController::applyChoice(bool confirmReplacement)
{
    return choices_.apply(ChoicePreviewFlow::Scope::Choice, confirmReplacement);
}

QVariantMap ApplicationController::nonConflictingPreview() const
{
    return choices_.view(ChoicePreviewFlow::Scope::NonConflicting);
}

bool ApplicationController::previewNonConflicting(const QString &fileId, const QString &revision, const QString &scope)
{
    return choices_.preview(ChoicePreviewFlow::Scope::NonConflicting, fileId, revisionId(revision), scope);
}

bool ApplicationController::applyNonConflicting()
{
    return choices_.apply(ChoicePreviewFlow::Scope::NonConflicting, true);
}

QVariantMap ApplicationController::wholeFilePreview() const
{
    return choices_.view(ChoicePreviewFlow::Scope::WholeFile);
}

bool ApplicationController::previewWholeFile(const QString &fileId, const QString &revision, const QString &side)
{
    return choices_.preview(ChoicePreviewFlow::Scope::WholeFile, fileId, revisionId(revision), side);
}

bool ApplicationController::applyWholeFile()
{
    return choices_.apply(ChoicePreviewFlow::Scope::WholeFile, true);
}

void ApplicationController::cancelChoice()
{
    choices_.cancel();
}

bool ApplicationController::editResult(const QString &fileId, const QString &revision,
                                      int begin, int end, const QString &replacement)
{
    const auto *file = files_.file(fileId.toStdString());
    const auto expected = revisionId(revision);
    if (!file || !expected || fileId != selectedFileId_ || file->current().id != *expected)
        return commandResult(Transition::StaleRevision);
    const auto old = text(file->current().draft.text);
    if (begin > end || !scalarBoundary(old, begin) || !scalarBoundary(old, end)
        || QString::fromUtf8(replacement.toUtf8()) != replacement) return commandResult(Transition::InvalidAnchor);
    return commandResult(files_.editResult(fileId.toStdString(), *expected,
        {{static_cast<std::size_t>(old.left(begin).toUtf8().size()), static_cast<std::size_t>(old.left(end).toUtf8().size())},
         replacement.toUtf8().toStdString()}));
}

bool ApplicationController::undo(const QString &fileId, const QString &revision)
{
    const auto expected = revisionId(revision);
    if (!expected || fileId != selectedFileId_) return commandResult(Transition::StaleRevision);
    if (editor_.composing()) return commandResult(Transition::Blocked);
    return commandResult(files_.undo(fileId.toStdString(), *expected));
}

bool ApplicationController::redo(const QString &fileId, const QString &revision)
{
    const auto expected = revisionId(revision);
    if (!expected || fileId != selectedFileId_) return commandResult(Transition::StaleRevision);
    if (editor_.composing()) return commandResult(Transition::Blocked);
    return commandResult(files_.redo(fileId.toStdString(), *expected));
}

bool ApplicationController::commandReady(const QString &fileId, const QString &revision, RevisionId *expected) const
{
    const auto *file = files_.file(fileId.toStdString());
    const auto id = revisionId(revision);
    if (!file || !id || fileId != selectedFileId_ || file->current().id != *id) return false;
    if (expected) *expected = *id;
    return file->support() == Support::Supported && !files_.hasPendingRecovery()
        && !files_.hasRecoveryFailure(file->id())
        && !editor_.composing();
}

bool ApplicationController::saveDraft(const QString &fileId, const QString &revision)
{
    RevisionId expected = 0;
    const auto *file = files_.file(fileId.toStdString());
    if (!file || !revisionId(revision) || fileId != selectedFileId_ || file->current().id != *revisionId(revision))
        return commandResult(Transition::StaleRevision);
    if (!commandReady(fileId, revision, &expected)) return commandResult(Transition::Blocked);
    const auto saved = file->checkpoint() && file->checkpoint()->id == expected
        && file->checkpoint()->result == file->current().result
        && file->checkpoint()->decisions == file->current().decisions
        && file->checkpointOutcome() && file->checkpointOutcome()->outcome == Outcome::Succeeded
        && file->checkpointOutcome()->revision == expected;
    if (saved) return commandResult(Transition::NoChange);
    // The recovery commit inside this call is the app checkpoint. It does not
    // write the worktree or stage a path; publication stays on Mark resolved.
    const auto result = files_.recordCheckpoint(fileId.toStdString(), expected, Outcome::Succeeded);
    if (result == Transition::Blocked && files_.hasPendingRecovery()) {
        commandMessage_ = tr("Draft save failed. The edited result is still available.");
        Q_EMIT commandChanged();
        return false;
    }
    if (result != Transition::Applied) return commandResult(result);
    commandMessage_ = tr("Draft saved in Branch Truce; repository file unchanged.");
    Q_EMIT commandChanged();
    return true;
}

QString ApplicationController::parentStagingPath() const
{
    if (!publisher_.parentStagingOffer(selectedFileId_.toStdString())) return {};
    return displayPath(selectedFileId_);
}

bool ApplicationController::mergetoolWaiting() const
{
    const auto *file = files_.file(selectedFileId_.toStdString());
    return file && mergetool_.waitingOn(QByteArray::fromStdString(file->path()));
}

bool ApplicationController::parentSessionActive() const
{
    return mergetool_.parentSessionActive();
}

bool ApplicationController::canEndParentSession() const
{
    return mergetool_.canEndParentSession();
}

bool ApplicationController::markResolved(const QString &fileId, const QString &revision, bool confirmUndecided)
{
    RevisionId expected = 0;
    const auto *session = files_.file(fileId.toStdString());
    if (!session || !revisionId(revision) || fileId != selectedFileId_ || session->current().id != *revisionId(revision))
        return commandResult(Transition::StaleRevision);
    if (!commandReady(fileId, revision, &expected)) return commandResult(Transition::Blocked);
    if (mergetool_.blocks(QByteArray::fromStdString(session->path()))) {
        commandMessage_ = tr("Waiting for Git to launch this path. It is not resolved.");
        Q_EMIT commandChanged();
        return false;
    }
    const auto started = publisher_.start(fileId.toStdString(), expected, confirmUndecided);
    commandMessage_ = started.detail;
    markerBlocker_.clear();
    if (started.markers && started.markers->first())
        markerBlocker_ = {{QStringLiteral("fileId"), fileId}, {QStringLiteral("revision"), revision},
            {QStringLiteral("message"), started.detail},
            {QStringLiteral("count"), QVariant::fromValue(static_cast<qulonglong>(started.markers->count()))},
            {QStringLiteral("line"), QVariant::fromValue(static_cast<qulonglong>(started.markers->first()->line + 1))}};
    Q_EMIT commandChanged();
    return started.transition == Transition::Applied || started.transition == Transition::NoChange;
}

QVariantMap ApplicationController::markerBlocker() const
{
    const auto *file = files_.file(selectedFileId_.toStdString());
    if (markerBlocker_.isEmpty() || !file || markerBlocker_.value(QStringLiteral("fileId")).toString() != selectedFileId_
        || markerBlocker_.value(QStringLiteral("message")).toString() != commandMessage_
        || revisionId(markerBlocker_.value(QStringLiteral("revision")).toString()) != file->current().id)
        return {};
    auto result = markerBlocker_;
    result.remove(QStringLiteral("message"));
    return result;
}

bool ApplicationController::showFirstMarker()
{
    const auto *file = files_.file(selectedFileId_.toStdString());
    // A pending draft in the editor has other offsets than the current revision.
    if (markerBlocker().isEmpty() || !editor_.attached() || files_.pendingDraft(file->id())) return false;
    const auto first = file->markers().first();
    const int cursor = first ? utf16Offset(file->current().draft.text, first->bytes.begin) : -1;
    if (cursor < 0) return false;
    editor_.setCursorPosition(cursor);
    return true;
}

void ApplicationController::cancelResolution()
{
    publisher_.cancelConfirmation();
}

bool ApplicationController::assignParentStaging(const QByteArray &path)
{
    return publisher_.assignParentStaging(path);
}

bool ApplicationController::verifyParentAcknowledgment(const QString &fileId, const QString &revision)
{
    const auto expected = revisionId(revision);
    if (!expected) return false;
    return publisher_.verifyParentAcknowledgment(fileId.toStdString(), *expected);
}

std::optional<ResolutionPublisher::Offer> ApplicationController::parentStagingOffer(const QString &fileId) const
{
    return publisher_.parentStagingOffer(fileId.toStdString());
}

bool ApplicationController::mergetoolListening() const { return mergetool_.listening(); }
QByteArray ApplicationController::mergetoolWaitingPath() const { return mergetool_.waitingPath(); }
bool ApplicationController::mergetoolHasWaiter(const QByteArray &path) const { return mergetool_.hasWaiter(path); }
QStringList ApplicationController::mergetoolIntakes() const { return mergetool_.intakes(); }

bool ApplicationController::leaveMergetool()
{
    const auto *file = files_.file(selectedFileId_.toStdString());
    if (!file) return false;
    return mergetool_.leave(QByteArray::fromStdString(file->path()));
}

bool ApplicationController::endParentSession()
{
    return mergetool_.endParentSession();
}

QVariantMap ApplicationController::handoff() const
{
    auto result = files_.handoff();
    QStringList sessionBlockers;
    if (publisher_.busy()) sessionBlockers.push_back(tr("A resolution is still running; its outcome is not known yet."));
    if (const auto launches = mergetool_.unreleasedLaunches(); launches > 0)
        sessionBlockers.push_back(launches == 1 ? tr("Git is waiting on 1 launched path with no decision yet.")
            : tr("Git is waiting on %1 launched paths with no decision yet.").arg(launches));
    const bool clean = result.value(QStringLiteral("clean")).toBool() && sessionBlockers.isEmpty();
    if (!clean && result.value(QStringLiteral("clean")).toBool())
        result.insert(QStringLiteral("outcome"), tr("Every file is resolved, but this session is not finished."));
    result.insert(QStringLiteral("clean"), clean);
    result.insert(QStringLiteral("sessionBlockers"), sessionBlockers);
    // Leaving must not lose input that never reached the recovery store, or
    // a publication whose outcome is still unknown.
    QString leaveBlocker;
    if (files_.hasPendingRecovery())
        leaveBlocker = tr("Pending input is not in recovery yet. Retry recovery before leaving so the draft is kept.");
    else if (publisher_.busy())
        leaveBlocker = tr("Wait for the running resolution to finish before leaving.");
    result.insert(QStringLiteral("canLeave"), leaveBlocker.isEmpty() && sessionOutcome_.isEmpty());
    result.insert(QStringLiteral("leaveBlocker"), leaveBlocker);
    return result;
}

bool ApplicationController::finishSession()
{
    if (!sessionOutcome_.isEmpty()) return false;
    const auto summary = handoff();
    if (!summary.value(QStringLiteral("clean")).toBool()) {
        commandMessage_ = tr("Work remains outstanding, so this session cannot finish cleanly. "
                             "Open an outstanding file or leave deliberately.");
        Q_EMIT commandChanged();
        return false;
    }
    sessionOutcome_ = summary.value(QStringLiteral("outcome")).toString();
    Q_EMIT handoffChanged();
    Q_EMIT sessionEnded(sessionOutcome_, true);
    return true;
}

bool ApplicationController::leaveSession()
{
    const auto summary = handoff();
    if (!summary.value(QStringLiteral("canLeave")).toBool()) {
        const auto blocker = summary.value(QStringLiteral("leaveBlocker")).toString();
        if (!blocker.isEmpty()) {
            commandMessage_ = blocker;
            Q_EMIT commandChanged();
        }
        return false;
    }
    const bool clean = summary.value(QStringLiteral("clean")).toBool();
    const auto launches = mergetool_.unreleasedLaunches();
    if (launches > 0) (void)mergetool_.leaveAll();
    if (clean) {
        sessionOutcome_ = summary.value(QStringLiteral("outcome")).toString();
    } else {
        const auto outstanding = summary.value(QStringLiteral("outstandingFiles")).toInt();
        const auto total = summary.value(QStringLiteral("totalFiles")).toInt();
        sessionOutcome_ = tr("Left with %1 of %2 files outstanding. Drafts stay in Branch Truce; "
                             "the Git operation was not continued or committed.").arg(outstanding).arg(total);
        if (launches > 0)
            sessionOutcome_ += QLatin1Char(' ') + (launches == 1 ? tr("1 launched path returned to Git unresolved.")
                : tr("%1 launched paths returned to Git unresolved.").arg(launches));
    }
    Q_EMIT handoffChanged();
    Q_EMIT sessionEnded(sessionOutcome_, clean);
    return true;
}

bool ApplicationController::recheckParentAcknowledgment()
{
    const auto offer = publisher_.parentStagingOffer(selectedFileId_.toStdString());
    if (!offer) return false;
    return verifyParentAcknowledgment(selectedFileId_, QString::number(offer->revision));
}

QString ApplicationController::discardedMessage(const FileSession *current) const
{
    return current && !current->facts().unsaved
        ? tr("Discarded unsaved changes. Draft saved in Branch Truce; repository file unchanged.")
        : tr("Discarded unsaved changes and restored the initial result. It is still unsaved, and the repository file is unchanged.");
}

bool ApplicationController::discardUnsaved(const QString &fileId, const QString &revision)
{
    RevisionId expected = 0;
    const auto *file = files_.file(fileId.toStdString());
    if (!file || !revisionId(revision) || fileId != selectedFileId_ || file->current().id != *revisionId(revision))
        return commandResult(Transition::StaleRevision);
    if (!commandReady(fileId, revision, &expected)) return commandResult(Transition::Blocked);
    // Applied means the restored text and decisions are already in the journal.
    const auto result = files_.discardToCheckpoint(fileId.toStdString(), expected);
    if (result == Transition::Blocked && files_.hasPendingRecovery()) {
        // The editor shows the pending restored result. The acknowledged edit
        // stays in place until this same discard commits.
        discardPending_ = true;
        commandMessage_ = tr("Discard was not completed. The restored result is shown until retry finishes it.");
        Q_EMIT commandChanged();
        return false;
    }
    discardPending_ = false;
    if (result != Transition::Applied) return commandResult(result);
    commandMessage_ = discardedMessage(files_.file(fileId.toStdString()));
    Q_EMIT commandChanged();
    return true;
}

QString ApplicationController::stagedDiscardMessage()
{
    return tr("Discarded the draft. The result staged in Git is current again; the repository file is unchanged.");
}

bool ApplicationController::discardToStaged(const QString &fileId, const QString &revision)
{
    RevisionId expected = 0;
    const auto *file = files_.file(fileId.toStdString());
    if (!file || !revisionId(revision) || fileId != selectedFileId_ || file->current().id != *revisionId(revision))
        return commandResult(Transition::StaleRevision);
    if (!commandReady(fileId, revision, &expected)) return commandResult(Transition::Blocked);
    // Applied means the staged revision is current again and the journal holds it.
    const auto result = files_.discardToStaged(fileId.toStdString(), expected);
    if (result == Transition::Blocked && files_.hasPendingRecovery()) {
        // The acknowledged draft stays current until retry commits this same discard.
        stagedDiscardPending_ = true;
        commandMessage_ = tr("Discard was not completed. The draft is kept until retry finishes the discard.");
        Q_EMIT commandChanged();
        return false;
    }
    stagedDiscardPending_ = false;
    if (result == Transition::Blocked) {
        commandMessage_ = tr("Discard was not completed. No verified result staged in Git to return to, so the draft is kept.");
        Q_EMIT commandChanged();
        return false;
    }
    if (result != Transition::Applied) return commandResult(result);
    commandMessage_ = stagedDiscardMessage();
    Q_EMIT commandChanged();
    return true;
}

void ApplicationController::attachResultEditor(QObject *editor)
{
    editor_.attach(editor);
}

void ApplicationController::attachSourceEditor(QObject *editor)
{
    editor_.attachSource(editor);
}

void ApplicationController::setSyntaxBackground(const QColor &background)
{
    editor_.setSyntaxBackground(background);
}

void ApplicationController::setVisibleWhitespace(QObject *editor, bool visible)
{
    editor_.setVisibleWhitespace(editor, visible);
}

bool ApplicationController::resultClipboard(const QString &action)
{
    return editor_.clipboard(action);
}

// A newly selected file puts the cursor at its first undecided region, or at
// the start without one. The offsets belong to the current revision, not to a
// pending draft in the editor.
QVariantMap ApplicationController::firstUndecidedPlacement(const FileSession *file, const QString &loaded) const
{
    QVariantMap placement{{QStringLiteral("fileId"), selectedFileId_}, {QStringLiteral("count"), 0}};
    const auto targets = file ? undecidedTargets(*file) : std::vector<ScopeTarget>{};
    if (targets.empty() || loaded != text(file->current().draft.text)) return placement;
    const auto &first = targets.front();
    const auto &result = file->current().draft.text;
    const int begin = first.resultRange ? utf16Offset(result, first.byteBegin) : -1;
    const int end = first.resultRange ? utf16Offset(result, first.byteEnd) : -1;
    return {{QStringLiteral("fileId"), selectedFileId_},
        {QStringLiteral("scope"), first.resultRange ? QStringLiteral("result") : QStringLiteral("unmapped-source")},
        {QStringLiteral("utf16Begin"), begin},
        {QStringLiteral("utf16End"), end},
        {QStringLiteral("contributionId"), text(first.contributionId)},
        {QStringLiteral("count"), static_cast<int>(targets.size())}};
}

bool ApplicationController::toggleFileList()
{
    if (editor_.composing()) return false;
    const auto fileId = selectedFileId_;
    const auto *file = files_.file(fileId.toStdString());
    const auto *pending = file ? files_.pendingDraft(file->id()) : nullptr;
    const auto before = file ? text(pending ? pending->text : file->current().draft.text) : QString{};
    const auto revision = file ? file->current().id : RevisionId{};
    editor_.holdLocation();
    fileListVisible_ = !fileListVisible_;
    Q_EMIT fileListChanged();
    const auto *after = files_.file(fileId.toStdString());
    const auto *afterPending = after ? files_.pendingDraft(after->id()) : nullptr;
    const auto afterText = after ? text(afterPending ? afterPending->text : after->current().draft.text) : QString{};
    const bool preserved = selectedFileId_ == fileId && before == afterText
        && (!after || after->current().id == revision);
    editor_.restoreLocation();
    return preserved;
}

bool ApplicationController::navigate(const QString &mode, int direction)
{
    return navigator_.navigate(mode, direction);
}

int ApplicationController::outsideAppFiles() const
{
    int count = 0;
    for (int row = 0; row < files_.totalFiles(); ++row)
        if (files_.data(files_.index(row), SessionModel::SupportRole).toString() == QLatin1String("outside-app")) ++count;
    return count;
}

int ApplicationController::loadErrorFiles() const
{
    int count = 0;
    for (int row = 0; row < files_.totalFiles(); ++row)
        if (files_.data(files_.index(row), SessionModel::SupportRole).toString() == QLatin1String("unreadable")) ++count;
    return count;
}
}
