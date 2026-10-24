#include "ApplicationController.h"
#include "RecoveryRestore.h"
#include <QDir>
#include <QFileInfo>
#include "core/BuildInfo.h"
#include <QStandardPaths>
#include <QUuid>
#include <QQuickTextDocument>
#include <QQuickItem>
#include <QTextDocument>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QCoreApplication>
#include <QScopedValueRollback>
#include <algorithm>
#include <limits>

namespace branch_truce {
namespace {
QString text(const std::string &value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
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

QString documentText(const QTextDocument &document)
{
    return document.toRawText().replace(QChar(0x2029), QLatin1Char('\n'));
}

Support supportFor(const git::Conflict &file)
{
    return file.support == git::ConflictSupport::LoadFailure || file.support == git::ConflictSupport::InspectionFailure
        ? Support::Unreadable : Support::OutsideApp;
}

TextSnapshot textSnapshot(const storage::TextDocument &document)
{
    SnapshotFormat format;
    switch (document.format.encoding) {
    case storage::TextEncoding::Utf8: format.encoding = SnapshotFormat::Encoding::Utf8; break;
    case storage::TextEncoding::Utf16LittleEndian: format.encoding = SnapshotFormat::Encoding::Utf16LittleEndian; break;
    case storage::TextEncoding::Utf16BigEndian: format.encoding = SnapshotFormat::Encoding::Utf16BigEndian; break;
    }
    format.bom = document.format.bom;
    format.newline = document.format.newline == storage::Newline::CrLf
        ? SnapshotFormat::Newline::CrLf : SnapshotFormat::Newline::Lf;
    format.finalNewline = document.format.finalNewline;
    return {document.utf8.toStdString(), document.originalBytes.toStdString(), format};
}

std::optional<FileSnapshots> captureFile(const git::Conflict &file,
    const git::RepositoryContext &repository, const std::string &sessionId)
{
    if (!file.editable() || !file.result || !file.resultIdentity || !repository.supported
        || file.sources.size() != file.stages.size() || file.sources.isEmpty()) return std::nullopt;
    FileSnapshots captured;
    captured.path = file.path.toStdString();
    captured.resultMode = file.mode.toStdString();
    captured.launchResult = {textSnapshot(*file.result), *file.resultIdentity};
    auto &provenance = captured.provenance;
    provenance.sessionId = sessionId;
    provenance.worktreeRoot = repository.worktreeRoot.toStdString();
    provenance.gitDirectory = repository.gitDirectory.toStdString();
    if (repository.operation == git::RepositoryOperation::Merge) provenance.operation = SnapshotProvenance::Operation::Merge;
    else if (repository.operation == git::RepositoryOperation::Rebase) provenance.operation = SnapshotProvenance::Operation::Rebase;
    else return std::nullopt;
    provenance.stage2CommitId = repository.stage2.commitId.toStdString();
    provenance.stage3CommitId = repository.stage3.commitId.toStdString();
    if (!repository.baseCommitId.isEmpty()) provenance.baseCommitId = repository.baseCommitId.toStdString();
    provenance.headName = repository.headName.toStdString();
    provenance.originalHead = repository.originalHead.toStdString();
    provenance.onto = repository.onto.toStdString();
    const auto &policy = file.sources.front().policy;
    for (auto entry = policy.attributes.cbegin(); entry != policy.attributes.cend(); ++entry)
        captured.conversion.attributes.emplace(entry.key().toStdString(), entry.value().toStdString());
    for (auto entry = policy.configuration.cbegin(); entry != policy.configuration.cend(); ++entry)
        captured.conversion.configuration.emplace(entry.key().toStdString(), entry.value().toStdString());
    for (qsizetype i = 0; i < file.stages.size(); ++i) {
        const auto &stage = file.stages[i];
        const auto &source = file.sources[i];
        if (stage.path != file.path || source.path != file.path || source.worktreeRoot != repository.worktreeRoot
            || source.canonicalObjectId != stage.objectId || source.sourceMode != stage.mode
            || source.mode != file.mode || source.policy != policy) return std::nullopt;
        SourceSnapshot snapshot{stage.objectId.toStdString(), stage.mode.toStdString(),
            source.canonicalBytes.toStdString(), textSnapshot(source.document), std::nullopt, {}};
        if (stage.stage == 1 && !captured.earlier) {
            snapshot.commitId = provenance.baseCommitId;
            snapshot.label = "Common earlier version";
            captured.earlier = std::move(snapshot);
        } else if (stage.stage == 2 && !captured.stage2) {
            snapshot.commitId = provenance.stage2CommitId;
            snapshot.label = repository.stage2.label.toStdString();
            captured.stage2 = std::move(snapshot);
        } else if (stage.stage == 3 && !captured.stage3) {
            snapshot.commitId = provenance.stage3CommitId;
            snapshot.label = repository.stage3.label.toStdString();
            captured.stage3 = std::move(snapshot);
        } else return std::nullopt;
    }
    if (!captured.stage2 || !captured.stage3) return std::nullopt;
    return captured;
}
}

ApplicationController::ApplicationController(QObject *parent)
    : QObject(parent), files_(this),
      coordinator_(files_, QStandardPaths::findExecutable(QStringLiteral("git")), this),
      classifier_(QStandardPaths::findExecutable(QStringLiteral("git")), this)
{
    connect(&classifier_, &git::ConflictClassifier::finished, this, &ApplicationController::classified);
    connect(&files_, &SessionModel::summaryChanged, this, [this] {
        if (!files_.file(selectedFileId_.toStdString())) {
            selectedFileId_ = files_.totalFiles() > 0
                ? files_.data(files_.index(0), SessionModel::FileIdRole).toString() : QString{};
        }
        Q_EMIT inventoryChanged();
        Q_EMIT selectedFileChanged();
    });
    connect(this, &ApplicationController::selectedFileChanged, this, &ApplicationController::syncResultEditor);
}
QString ApplicationController::version() const
{
    return QString::fromUtf8(branch_truce::version().data(),
                             static_cast<qsizetype>(branch_truce::version().size()));
}

QString ApplicationController::stage2() const
{
    if (!repository_.supported) return {};
    return repository_.stage2.label + QStringLiteral(" · ") + QString::fromLatin1(repository_.stage2.commitId);
}

QString ApplicationController::stage3() const
{
    if (!repository_.supported) return {};
    return repository_.stage3.label + QStringLiteral(" · ") + QString::fromLatin1(repository_.stage3.commitId);
}

QString ApplicationController::workflow() const
{
    return tr("Continue the operation and commit in your usual Git workflow. "
              "This session covers only the work opened at launch.");
}

QString ApplicationController::displayPath(const QString &fileId) const
{
    const auto *file = files_.file(fileId.toStdString());
    if (!file) return {};
    auto display = text(file->path());
    const QByteArray bytes(file->path().data(), static_cast<qsizetype>(file->path().size()));
    const bool exact = display.toUtf8() == bytes;
    display.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    display.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    display.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    display.replace(QLatin1Char('\t'), QStringLiteral("\\t"));
    return exact ? display : tr("%1 [path bytes: %2]").arg(display, QString::fromLatin1(bytes.toHex()));
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
    QVariantMap result{{QStringLiteral("fileId"), selectedFileId_},
        {QStringLiteral("path"), displayPath(selectedFileId_)},
        {QStringLiteral("pathBytes"), QByteArray(file->path().data(), static_cast<qsizetype>(file->path().size()))},
        {QStringLiteral("support"), support}, {QStringLiteral("supportDetail"), text(file->supportDetail())},
        {QStringLiteral("resultText"), text(file->current().draft.text)}};
    for (int row = 0; row < files_.totalFiles(); ++row) {
        const auto index = files_.index(row);
        if (files_.data(index, SessionModel::FileIdRole).toString() != selectedFileId_) continue;
        for (const auto role : {SessionModel::StatusRole, SessionModel::UnsavedRole, SessionModel::ReviewNeededRole,
             SessionModel::ResolvedRole, SessionModel::UndecidedRole, SessionModel::PendingInputRole,
             SessionModel::PendingTextRole, SessionModel::RecoveryStateRole, SessionModel::RecoveryErrorRole,
             SessionModel::FinishBlockersRole, SessionModel::CurrentRevisionRole, SessionModel::CanUndoRole,
             SessionModel::CanRedoRole})
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
    QVariantList choices;
    std::map<SourceSide, std::vector<std::size_t>> lineStarts;
    if (file->snapshots()) {
        for (const auto side : {SourceSide::Stage2, SourceSide::Stage3}) {
            const auto &source = side == SourceSide::Stage2 ? file->snapshots()->stage2 : file->snapshots()->stage3;
            if (!source) continue;
            auto &starts = lineStarts[side];
            starts.push_back(0);
            for (std::size_t i = 0; i < source->worktree.utf8.size(); ++i)
                if (source->worktree.utf8[i] == '\n') starts.push_back(i + 1);
        }
    }
    for (const auto &target : file->choices()) {
        // Older recovery records retain their exact decision vocabulary and
        // status. Do not invent new undecided judgments while restoring them.
        if (std::any_of(target.decisions.begin(), target.decisions.end(), [&](const auto &id) {
            return !file->current().draft.decisions.contains(id);
        })) continue;
        const auto &source = target.source.side == SourceSide::Stage2 ? file->snapshots()->stage2 : file->snapshots()->stage3;
        const auto &sourceText = source->worktree.utf8;
        const auto &starts = lineStarts.at(target.source.side);
        const auto lineNumber = [&](std::size_t offset) {
            return std::distance(starts.begin(), std::upper_bound(starts.begin(), starts.end(), offset));
        };
        const auto line = lineNumber(target.source.range.begin);
        const auto last = lineNumber(target.source.range.end > target.source.range.begin
            ? target.source.range.end - 1 : target.source.range.end);
        QStringList states;
        bool undecided = true;
        for (const auto &id : target.decisions) {
            const auto found = file->current().draft.decisions.find(id);
            const auto state = found == file->current().draft.decisions.end() ? DecisionValue::Undecided : found->second.value;
            undecided = undecided && state == DecisionValue::Undecided;
            states.push_back(state == DecisionValue::Accepted ? tr("accepted")
                : state == DecisionValue::Ignored ? tr("ignored") : tr("undecided"));
        }
        states.removeDuplicates();
        const auto *anchor = file->correspondence() ? file->correspondence()->anchor(target.source.contributionId) : nullptr;
        const bool mapped = anchor && anchor->state == AnchorState::Mapped && anchor->result;
        const bool available = undecided && mapped;
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
    result.insert(QStringLiteral("choiceNotice"), choices.empty() && !file->choices().empty()
        ? tr("Scoped choices are unavailable for this earlier draft. Direct editing remains available.") : QString{});
    return result;
}

bool ApplicationController::selectFile(const QString &fileId)
{
    if (!files_.file(fileId.toStdString())) return false;
    if (selectedFileId_ == fileId) return true;
    if (resultEditor_ && resultEditor_->property("inputMethodComposing").toBool()) return false;
    cancelChoice();
    selectedFileId_ = fileId;
    Q_EMIT selectedFileChanged();
    return true;
}

QVariantMap ApplicationController::commandPreview() const
{
    if (!preview_) return {};
    QStringList sources;
    const auto *file = files_.file(previewFileId_.toStdString());
    if (!file || !file->snapshots()) return {};
    for (const auto &scope : preview_->sourceOrder) {
        const auto &source = scope.side == SourceSide::Stage2 ? file->snapshots()->stage2 : file->snapshots()->stage3;
        const auto &value = source->worktree.utf8;
        const auto line = [&](std::size_t offset) {
            return 1 + std::count(value.begin(), value.begin() + static_cast<std::ptrdiff_t>(offset), '\n');
        };
        const auto side = scope.side == SourceSide::Stage2 ? tr("Left") : tr("Right");
        sources.push_back(scope.range.begin == scope.range.end
            ? tr("%1, empty scope at line %2 · %3").arg(side).arg(line(scope.range.begin)).arg(text(source->label))
            : tr("%1, lines %2–%3 · %4").arg(side).arg(line(scope.range.begin)).arg(line(scope.range.end - 1))
                .arg(text(source->label)));
    }
    return {{QStringLiteral("before"), text(preview_->before)},
        {QStringLiteral("replacement"), text(preview_->replacement)},
        {QStringLiteral("detail"), text(preview_->detail)},
        {QStringLiteral("orderedCombination"), !preview_->sourceOrder.empty()},
        {QStringLiteral("order"), preview_->action == ChoiceAction::CombineLeftFirst ? tr("Left then Right") : tr("Right then Left")},
        {QStringLiteral("sourceScopes"), sources.join(QLatin1Char('\n'))},
        {QStringLiteral("replacesManual"), preview_->replacesManual},
        {QStringLiteral("confirmationRequired"), preview_->state == Transition::ConfirmationRequired}};
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
    cancelChoice();
    const auto *file = files_.file(fileId.toStdString());
    const auto expected = revisionId(revision);
    if (!file || !expected || fileId != selectedFileId_) return commandResult(Transition::StaleRevision);
    if (files_.hasPendingRecovery() || files_.hasRecoveryFailure(file->id())
        || (resultEditor_ && resultEditor_->property("inputMethodComposing").toBool())) return commandResult(Transition::Blocked);
    ChoiceAction kind;
    if (action == QLatin1String("accept")) kind = ChoiceAction::Accept;
    else if (action == QLatin1String("ignore")) kind = ChoiceAction::Ignore;
    else if (action == QLatin1String("resolve")) kind = ChoiceAction::ResolveUsingSide;
    else if (action == QLatin1String("left-first")) kind = ChoiceAction::CombineLeftFirst;
    else if (action == QLatin1String("right-first")) kind = ChoiceAction::CombineRightFirst;
    else return commandResult(Transition::InvalidAnchor);
    auto preview = branch_truce::previewChoice(*file, *expected, target.toStdString(), kind);
    commandMessage_ = text(preview.detail);
    if (preview.state != Transition::Applied && preview.state != Transition::ConfirmationRequired) {
        Q_EMIT commandChanged();
        return false;
    }
    preview_ = std::move(preview);
    previewFileId_ = fileId;
    Q_EMIT commandChanged();
    return true;
}

bool ApplicationController::applyChoice(bool confirmReplacement)
{
    if (!preview_ || previewFileId_ != selectedFileId_) return commandResult(Transition::StaleRevision);
    const auto preview = *preview_;
    const auto id = previewFileId_;
    if (resultEditor_ && resultEditor_->property("inputMethodComposing").toBool()) return commandResult(Transition::Blocked);
    const auto result = files_.applyChoice(id.toStdString(), preview, confirmReplacement);
    if (result != Transition::ConfirmationRequired) { preview_.reset(); previewFileId_.clear(); }
    return commandResult(result);
}

QVariantMap ApplicationController::nonConflictingPreview() const
{
    if (!nonConflictingPreview_) return {};
    const auto &preview = *nonConflictingPreview_;
    const auto *file = files_.file(previewFileId_.toStdString());
    if (!file || !file->snapshots()) return {};
    QVariantList changes;
    for (const auto &change : preview.changes) {
        QStringList scopes;
        for (const auto &source : change.sources) {
            const auto &captured = source.side == SourceSide::Stage2 ? file->snapshots()->stage2 : file->snapshots()->stage3;
            const auto &value = captured->worktree.utf8;
            const auto line = [&](std::size_t offset) {
                return 1 + std::count(value.begin(), value.begin() + static_cast<std::ptrdiff_t>(offset), '\n');
            };
            const auto side = source.side == SourceSide::Stage2 ? tr("Left") : tr("Right");
            scopes.push_back(source.range.begin == source.range.end
                ? tr("%1 deletion at line %2 · %3").arg(side).arg(line(source.range.begin)).arg(text(captured->label))
                : tr("%1 lines %2–%3 · %4").arg(side).arg(line(source.range.begin)).arg(line(source.range.end - 1))
                    .arg(text(captured->label)));
        }
        changes.push_back(QVariantMap{{QStringLiteral("scope"), scopes.join(QLatin1Char('\n'))},
            {QStringLiteral("before"), text(change.before)}, {QStringLiteral("replacement"), text(change.replacement)}});
    }
    return {{QStringLiteral("path"), displayPath(previewFileId_)},
        {QStringLiteral("scope"), preview.scope == NonConflictingScope::Left ? tr("Left")
            : preview.scope == NonConflictingScope::Right ? tr("Right") : tr("Left and Right")},
        {QStringLiteral("detail"), text(preview.detail)}, {QStringLiteral("changes"), changes},
        {QStringLiteral("summary"), tr("%1 eligible changes · %2 other non-conflicting changes skipped")
            .arg(preview.changes.size()).arg(preview.skipped)}};
}

bool ApplicationController::previewNonConflicting(const QString &fileId, const QString &revision, const QString &scope)
{
    cancelChoice();
    const auto *file = files_.file(fileId.toStdString());
    const auto expected = revisionId(revision);
    if (!file || !expected || fileId != selectedFileId_) return commandResult(Transition::StaleRevision);
    if (files_.hasPendingRecovery() || files_.hasRecoveryFailure(file->id())
        || (resultEditor_ && resultEditor_->property("inputMethodComposing").toBool())) return commandResult(Transition::Blocked);
    NonConflictingScope kind;
    if (scope == QLatin1String("left")) kind = NonConflictingScope::Left;
    else if (scope == QLatin1String("right")) kind = NonConflictingScope::Right;
    else if (scope == QLatin1String("both")) kind = NonConflictingScope::Both;
    else return commandResult(Transition::InvalidAnchor);
    auto preview = branch_truce::previewNonConflicting(*file, *expected, kind);
    commandMessage_ = preview.state == Transition::NoChange ? tr("No eligible non-conflicting changes. ") : QString{};
    commandMessage_ += text(preview.detail);
    if (preview.state != Transition::ConfirmationRequired) {
        Q_EMIT commandChanged();
        return false;
    }
    nonConflictingPreview_ = std::move(preview);
    previewFileId_ = fileId;
    Q_EMIT commandChanged();
    return true;
}

bool ApplicationController::applyNonConflicting()
{
    if (!nonConflictingPreview_ || previewFileId_ != selectedFileId_) return commandResult(Transition::StaleRevision);
    if (resultEditor_ && resultEditor_->property("inputMethodComposing").toBool()) return commandResult(Transition::Blocked);
    const auto preview = *nonConflictingPreview_;
    const auto id = previewFileId_;
    const auto result = files_.applyNonConflicting(id.toStdString(), preview, true);
    nonConflictingPreview_.reset();
    previewFileId_.clear();
    return commandResult(result);
}

QVariantMap ApplicationController::wholeFilePreview() const
{
    if (!wholeFilePreview_) return {};
    const auto &preview = *wholeFilePreview_;
    const auto *file = files_.file(previewFileId_.toStdString());
    if (!file || !file->snapshots()) return {};
    const auto &captured = *file->snapshots();
    const auto &selected = preview.side == SourceSide::Stage2 ? captured.stage2 : captured.stage3;
    if (!selected || captured.provenance.sessionId != preview.origin.sessionId
        || selected->objectId != preview.origin.sourceObjectId) return {};
    const auto rangeLabel = [&](const QString &label, const std::string &value, ByteRange range) {
        const auto line = [&](std::size_t offset) {
            return 1 + std::count(value.begin(), value.begin() + static_cast<std::ptrdiff_t>(offset), '\n');
        };
        return range.begin == range.end ? tr("%1, empty scope at line %2").arg(label).arg(line(range.begin))
            : tr("%1, lines %2–%3").arg(label).arg(line(range.begin)).arg(line(range.end - 1));
    };
    const auto decisionLabel = [&](const Decision &decision) {
        auto label = decision.value == DecisionValue::Accepted ? tr("accepted")
            : decision.value == DecisionValue::Ignored ? tr("ignored") : tr("undecided");
        if (decision.order) label += tr(" · order %1").arg(*decision.order);
        return label;
    };
    QVariantList contributions;
    for (const auto &contribution : preview.contributions) {
        const auto &source = contribution.source.side == SourceSide::Stage2 ? captured.stage2 : captured.stage3;
        const auto &value = source->worktree.utf8;
        contributions.push_back(QVariantMap{
            {QStringLiteral("scope"), rangeLabel(contribution.source.side == SourceSide::Stage2 ? tr("Left") : tr("Right"),
                value, contribution.source.range) + QStringLiteral(" · ") + text(source->label)},
            {QStringLiteral("sourceText"), text(value.substr(contribution.source.range.begin,
                contribution.source.range.end - contribution.source.range.begin))},
            {QStringLiteral("beforeState"), decisionLabel(contribution.before)},
            {QStringLiteral("afterState"), decisionLabel(contribution.after)}});
    }
    QVariantList differences;
    for (const auto &passage : preview.differences) {
        differences.push_back(QVariantMap{
            {QStringLiteral("scope"), rangeLabel(tr("Current result"), preview.before, passage.result)
                + QStringLiteral(" → ") + rangeLabel(tr("Selected source"), preview.replacement, passage.source)},
            {QStringLiteral("before"), text(preview.before.substr(passage.result.begin, passage.result.end - passage.result.begin))},
            {QStringLiteral("replacement"), text(preview.replacement.substr(passage.source.begin, passage.source.end - passage.source.begin))}});
    }
    return {{QStringLiteral("path"), displayPath(previewFileId_)},
        {QStringLiteral("side"), preview.side == SourceSide::Stage2 ? tr("Left") : tr("Right")},
        {QStringLiteral("source"), text(selected->label) + QStringLiteral("\n") + text(selected->objectId)},
        {QStringLiteral("before"), text(preview.before)}, {QStringLiteral("replacement"), text(preview.replacement)},
        {QStringLiteral("detail"), text(preview.detail)}, {QStringLiteral("contributions"), contributions},
        {QStringLiteral("differences"), differences},
        {QStringLiteral("retainedDecisions"), preview.retainedDecisions
            ? tr("%1 earlier decisions without a source scope are retained.").arg(preview.retainedDecisions) : QString{}}};
}

bool ApplicationController::previewWholeFile(const QString &fileId, const QString &revision, const QString &side)
{
    cancelChoice();
    const auto *file = files_.file(fileId.toStdString());
    const auto expected = revisionId(revision);
    if (!file || !expected || fileId != selectedFileId_) return commandResult(Transition::StaleRevision);
    if (files_.hasPendingRecovery() || files_.hasRecoveryFailure(file->id())
        || (resultEditor_ && resultEditor_->property("inputMethodComposing").toBool())) return commandResult(Transition::Blocked);
    SourceSide source;
    if (side == QLatin1String("left")) source = SourceSide::Stage2;
    else if (side == QLatin1String("right")) source = SourceSide::Stage3;
    else return commandResult(Transition::InvalidAnchor);
    auto preview = branch_truce::previewWholeFile(*file, *expected, source);
    commandMessage_ = text(preview.detail);
    if (preview.state != Transition::ConfirmationRequired) {
        Q_EMIT commandChanged();
        return false;
    }
    wholeFilePreview_ = std::move(preview);
    previewFileId_ = fileId;
    Q_EMIT commandChanged();
    return true;
}

bool ApplicationController::applyWholeFile()
{
    if (!wholeFilePreview_ || previewFileId_ != selectedFileId_) return commandResult(Transition::StaleRevision);
    if (resultEditor_ && resultEditor_->property("inputMethodComposing").toBool()) return commandResult(Transition::Blocked);
    const auto preview = *wholeFilePreview_;
    const auto id = previewFileId_;
    const auto result = files_.applyWholeFile(id.toStdString(), preview, true);
    wholeFilePreview_.reset();
    previewFileId_.clear();
    return commandResult(result);
}

void ApplicationController::cancelChoice()
{
    preview_.reset();
    nonConflictingPreview_.reset();
    wholeFilePreview_.reset();
    previewFileId_.clear();
    commandMessage_.clear();
    Q_EMIT commandChanged();
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
    if (resultEditor_ && resultEditor_->property("inputMethodComposing").toBool()) return commandResult(Transition::Blocked);
    return commandResult(files_.undo(fileId.toStdString(), *expected));
}

bool ApplicationController::redo(const QString &fileId, const QString &revision)
{
    const auto expected = revisionId(revision);
    if (!expected || fileId != selectedFileId_) return commandResult(Transition::StaleRevision);
    if (resultEditor_ && resultEditor_->property("inputMethodComposing").toBool()) return commandResult(Transition::Blocked);
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
        && !(resultEditor_ && resultEditor_->property("inputMethodComposing").toBool());
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
        commandMessage_ = tr("Discard was not completed. The edited result is unchanged.");
        Q_EMIT commandChanged();
        return false;
    }
    if (result != Transition::Applied) return commandResult(result);
    const auto *current = files_.file(fileId.toStdString());
    commandMessage_ = current && !current->facts().unsaved
        ? tr("Discarded unsaved changes. Draft saved in Branch Truce; repository file unchanged.")
        : tr("Discarded unsaved changes and restored the initial result. It is still unsaved, and the repository file is unchanged.");
    Q_EMIT commandChanged();
    return true;
}

void ApplicationController::attachResultEditor(QObject *editor)
{
    disconnect(editorConnection_);
    if (resultEditor_) resultEditor_->removeEventFilter(this);
    resultEditor_ = editor;
    if (editor) editor->installEventFilter(this);
    auto *quick = editor ? editor->property("textDocument").value<QQuickTextDocument *>() : nullptr;
    resultDocument_ = quick ? quick->textDocument() : nullptr;
    if (!resultDocument_) return;
    resultDocument_->setUndoRedoEnabled(false);
    editorConnection_ = connect(resultDocument_, &QTextDocument::contentsChange,
        this, &ApplicationController::captureResultEdit);
    syncResultEditor();
}

void ApplicationController::captureResultEdit()
{
    if (syncingEditor_ || processingEditorEvent_ || !resultDocument_) return;
    const auto next = documentText(*resultDocument_);
    if (next == editorText_) return;
    const auto *file = files_.file(selectedFileId_.toStdString());
    const auto old = editorText_;
    if (!file || old != text(file->current().draft.text)
        || std::max(old.size(), next.size()) > std::numeric_limits<int>::max()) {
        commandResult(Transition::InvalidAnchor);
        syncResultEditor();
        return;
    }
    // Observe the complete native event, including a selection replacement or
    // an IME commit with several document notifications, as one domain command.
    qsizetype begin = 0;
    while (begin < old.size() && begin < next.size() && old.at(begin) == next.at(begin)) ++begin;
    if (begin > 0 && begin < old.size() && old.at(begin).isLowSurrogate()) --begin;
    auto oldEnd = old.size();
    auto newEnd = next.size();
    while (oldEnd > begin && newEnd > begin && old.at(oldEnd - 1) == next.at(newEnd - 1)) {
        --oldEnd;
        --newEnd;
    }
    if (oldEnd < old.size() && old.at(oldEnd).isLowSurrogate()) {
        ++oldEnd;
        ++newEnd;
    }
    editorText_ = next;
    (void)editResult(selectedFileId_, QString::number(file->current().id), static_cast<int>(begin),
        static_cast<int>(oldEnd), next.mid(begin, newEnd - begin));
    syncResultEditor();
}

bool ApplicationController::resultClipboard(const QString &action)
{
    if (!resultEditor_ || processingEditorEvent_) return false;
    const char *method = nullptr;
    if (action == QLatin1String("copy")) method = "copy";
    else if (action == QLatin1String("selectAll")) method = "selectAll";
    else if (action == QLatin1String("cut")) method = "cut";
    else if (action == QLatin1String("paste")) method = "paste";
    else return false;
    if ((action == QLatin1String("cut") || action == QLatin1String("paste"))
        && (resultEditor_->property("readOnly").toBool() || resultEditor_->property("inputMethodComposing").toBool()))
        return false;
    if (auto *item = qobject_cast<QQuickItem *>(resultEditor_.data())) item->forceActiveFocus(Qt::ShortcutFocusReason);
    bool invoked = false;
    {
        QScopedValueRollback guard(processingEditorEvent_, true);
        invoked = QMetaObject::invokeMethod(resultEditor_, method, Qt::DirectConnection);
    }
    captureResultEdit();
    return invoked && !files_.hasPendingRecovery();
}

bool ApplicationController::eventFilter(QObject *watched, QEvent *event)
{
    if (syncingEditor_ || processingEditorEvent_) return QObject::eventFilter(watched, event);
    if (watched == resultEditor_ && (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        auto *key = static_cast<QKeyEvent *>(event);
        const bool backwards = key->matches(QKeySequence::Undo);
        const bool forwards = key->matches(QKeySequence::Redo);
        if (backwards || forwards) {
            event->accept();
            if (event->type() == QEvent::KeyPress) {
                const auto *file = files_.file(selectedFileId_.toStdString());
                if (file) {
                    const auto revision = QString::number(file->current().id);
                    if (backwards) (void)undo(selectedFileId_, revision);
                    else (void)redo(selectedFileId_, revision);
                }
            }
            return true;
        }
    }
    const bool middlePaste = (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease)
        && static_cast<QMouseEvent *>(event)->button() == Qt::MiddleButton;
    if (watched == resultEditor_ && (event->type() == QEvent::KeyPress || event->type() == QEvent::InputMethod
        || event->type() == QEvent::Drop || middlePaste)) {
        {
            QScopedValueRollback guard(processingEditorEvent_, true);
            QCoreApplication::sendEvent(watched, event);
        }
        captureResultEdit();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

void ApplicationController::syncResultEditor()
{
    if (!resultDocument_ || !resultEditor_ || syncingEditor_) return;
    const auto *file = files_.file(selectedFileId_.toStdString());
    const auto *pending = file ? files_.pendingDraft(file->id()) : nullptr;
    const auto desired = file ? text(pending ? pending->text : file->current().draft.text) : QString{};
    syncingEditor_ = true;
    if (documentText(*resultDocument_) != desired) resultDocument_->setPlainText(desired);
    editorText_ = desired;
    const bool lossless = documentText(*resultDocument_) == desired && !desired.contains(QChar(0x2028))
        && !desired.contains(QChar(0x2029)) && !desired.contains(QLatin1Char('\r'));
    resultEditor_->setProperty("readOnly", !file || file->support() != Support::Supported || !lossless
        || files_.hasPendingRecovery() || files_.hasRecoveryFailure(file->id()));
    if (!lossless && file && file->support() == Support::Supported) {
        commandMessage_ = tr("This text cannot be edited losslessly by the current editor. Its captured result is retained.");
        Q_EMIT commandChanged();
    }
    syncingEditor_ = false;
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

void ApplicationController::setState(QString state, QString detail)
{
    state_ = std::move(state);
    detail_ = std::move(detail);
    Q_EMIT launchChanged();
}

bool ApplicationController::openRepository(const QString &directory)
{
    if (state_ != QLatin1String("idle")) return false;
    setState(QStringLiteral("opening"), tr("Inspecting the selected local repository…"));
    git::ConflictClassificationRequest request;
    request.directory = directory;
    if (!classifier_.start(request)) {
        setState(QStringLiteral("error"), tr("Could not start repository inspection."));
        return false;
    }
    return true;
}

void ApplicationController::classified(const git::ConflictClassificationResult &result)
{
    repository_ = result.repository;
    switch (repository_.operation) {
    case git::RepositoryOperation::Merge: operation_ = tr("Merge"); break;
    case git::RepositoryOperation::Rebase: operation_ = tr("Rebase"); break;
    case git::RepositoryOperation::Unsupported: operation_ = tr("Unsupported operation"); break;
    case git::RepositoryOperation::None: break;
    }
    if (result.outcome != git::Outcome::Success) operation_.clear();

    // Even an ended/unsupported operation can have expected recovery work.
    // Inspect its inventory so identity rejection is explicit, without applying it.
    if (result.outcome == git::Outcome::Success && !repository_.supported && !repository_.worktreeRoot.isEmpty()) {
        loadWorkingFiles(result.conflicts);
        return;
    }
    if (result.outcome != git::Outcome::Success || !repository_.supported) {
        std::vector<FileSession> files;
        for (const auto &file : result.conflicts) {
            files.emplace_back(file.path.toHex().toStdString(), file.path.toStdString(), Draft{},
                               supportFor(file), file.reason.isEmpty() ? result.diagnostic.toStdString() : file.reason.toStdString());
        }
        if (!files.empty()) coordinator_.replaceSession(repository_.worktreeRoot, QStringLiteral("unsupported"), std::move(files));
        if (result.outcome != git::Outcome::Success) {
            setState(QStringLiteral("error"), tr("Could not open the selected work: %1").arg(result.diagnostic));
        } else if (repository_.operation == git::RepositoryOperation::None) {
            setState(QStringLiteral("no-session"), tr("No merge or rebase is in progress in this repository."));
        } else {
            setState(QStringLiteral("unsupported"), repository_.diagnostic);
        }
        return;
    }
    loadWorkingFiles(result.conflicts);
}

void ApplicationController::loadWorkingFiles(QList<git::Conflict> conflicts)
{
    std::vector<FileSession> pending;
    for (const auto &file : conflicts) {
        pending.emplace_back(file.path.toHex().toStdString(), file.path.toStdString(), Draft{},
                             Support::Unreadable, "Opening the classified conflict");
    }
    if (pending.empty()) pending.emplace_back("recovery-inventory", "recovery-inventory", Draft{},
        Support::Unreadable, "Inspecting app-owned recovery records");
    const auto identity = operation_ + QLatin1Char(':') + QString::fromLatin1(repository_.stage2.commitId)
        + QLatin1Char(':') + QString::fromLatin1(repository_.stage3.commitId);
    const auto root = repository_.worktreeRoot;
    if (!coordinator_.replaceSession(root, identity, std::move(pending))) {
        setState(QStringLiteral("error"), tr("Could not bind the session to the discovered worktree."));
        return;
    }
    auto loaded = std::make_shared<std::vector<FileSession>>();
    auto recovery = std::make_shared<RecoveryRestore>();
    const auto anchor = conflicts.empty() ? std::string("recovery-inventory") : conflicts.front().path.toHex().toStdString();
    const auto recoveryDirectory = files_.recoveryDirectory();
    const auto recoveryInventory = QFileInfo(recoveryDirectory).absolutePath();
    const auto executable = QStandardPaths::findExecutable(QStringLiteral("git"));
    const auto sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    const auto task = coordinator_.submit(anchor, 1,
        [files = std::move(conflicts), repository = repository_, sessionId, loaded, recovery,
         recoveryDirectory, recoveryInventory, executable](const TaskInput &, const TaskControl &control) {
            for (const auto &file : files) {
                if (control.cancellationRequested()) return TaskResult{TaskStatus::Cancelled, {}, {}, {}, {}};
                Draft draft;
                auto support = supportFor(file);
                auto detail = file.reason;
                auto snapshots = captureFile(file, repository, sessionId);
                if (snapshots) {
                    support = Support::Supported;
                    draft.text = file.result->utf8.toStdString();
                    const auto lines = file.result->utf8.split('\n');
                    draft.unresolvedMarkers = static_cast<std::size_t>(std::count_if(
                        lines.cbegin(), lines.cend(), [](const auto &line) { return line.startsWith("<<<<<<<"); }));
                } else if (detail.isEmpty()) {
                    support = Support::Unreadable;
                    detail = QStringLiteral("Load error: conflict inspection did not provide complete source and result snapshots; reopen the session.");
                }
                loaded->emplace_back(file.path.toHex().toStdString(), file.path.toStdString(), std::move(draft),
                                     support, detail.toStdString(), std::move(snapshots), true);
            }
            *recovery = restoreRecovery(recoveryInventory, recoveryDirectory, repository, *loaded, executable);
            return TaskResult{};
        }, [this, loaded, recovery](const TaskCompletion &completion) {
            const auto diagnostics = recovery->diagnostics.join(QLatin1Char('\n'));
            const bool recovered = recovery->found;
            if (completion.result.status != TaskStatus::Succeeded
                || !coordinator_.completeRecoveryLoad(completion.stamp, std::move(*loaded), std::move(*recovery))) {
                setState(QStringLiteral("error"), tr("Could not load the current conflict files."));
                return;
            }
            if (!repository_.supported && repository_.operation != git::RepositoryOperation::None) {
                auto detail = repository_.diagnostic;
                if (!diagnostics.isEmpty()) detail += QLatin1Char('\n') + diagnostics;
                setState(QStringLiteral("unsupported"), detail);
                return;
            }
            if (files_.totalFiles() == 0) {
                setState(diagnostics.isEmpty() ? QStringLiteral("no-session") : QStringLiteral("error"),
                    diagnostics.isEmpty() ? tr("This operation has no current unmerged files. Return to your Git workflow.") : diagnostics);
                return;
            }
            auto detail = recovered ? tr("Recovered app drafts and decisions. Manual Save, review and Git resolution keep their recorded status.")
                : tr("Current conflict files are open. Files marked outside app remain outstanding.");
            if (!diagnostics.isEmpty()) detail += QLatin1Char('\n') + diagnostics;
            setState(QStringLiteral("ready"), detail);
        });
    if (task == 0) setState(QStringLiteral("error"), tr("Could not schedule conflict loading."));
}
}
