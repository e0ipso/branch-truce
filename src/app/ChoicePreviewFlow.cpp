// Choice previews show how a source action would change the result before the user confirms it.
#include "ChoicePreviewFlow.h"
#include "CommandFeedback.h"
#include "CommandGuard.h"
#include "EngineText.h"
#include "QtText.h"
#include "ResultEditorAdapter.h"
#include "Selection.h"
#include "SessionModel.h"
#include "SourceIdentity.h"
#include "TextPositions.h"
#include <QStringList>
#include <map>
#include <utility>

namespace branch_truce {
using qt_text::utf8;
namespace {

std::optional<ChoiceAction> choiceAction(ChoicePreviewFlow::Action action)
{
    switch (action) {
    case ChoicePreviewFlow::Action::Accept: return ChoiceAction::Accept;
    case ChoicePreviewFlow::Action::Ignore: return ChoiceAction::Ignore;
    case ChoicePreviewFlow::Action::ResolveUsingSide: return ChoiceAction::ResolveUsingSide;
    case ChoicePreviewFlow::Action::CombineLeftFirst: return ChoiceAction::CombineLeftFirst;
    case ChoicePreviewFlow::Action::CombineRightFirst: return ChoiceAction::CombineRightFirst;
    }
    return std::nullopt;
}

std::optional<NonConflictingScope> nonConflictingScope(ChoicePreviewFlow::NonConflicting scope)
{
    switch (scope) {
    case ChoicePreviewFlow::NonConflicting::LeftChanges: return NonConflictingScope::Left;
    case ChoicePreviewFlow::NonConflicting::RightChanges: return NonConflictingScope::Right;
    case ChoicePreviewFlow::NonConflicting::BothChanges: return NonConflictingScope::Both;
    }
    return std::nullopt;
}

std::optional<SourceSide> wholeFileSide(ChoicePreviewFlow::Side side)
{
    switch (side) {
    case ChoicePreviewFlow::Side::LeftSide: return SourceSide::Stage2;
    case ChoicePreviewFlow::Side::RightSide: return SourceSide::Stage3;
    }
    return std::nullopt;
}

// One file's captured sources, each numbered on first use.
class Sources {
public:
    explicit Sources(const FileSnapshots &snapshots) : snapshots_(snapshots) {}
    [[nodiscard]] const SourceSnapshot &at(SourceSide side) const { return *snapshotFor(snapshots_, side); }
    [[nodiscard]] const TextPositions &lines(SourceSide side)
    {
        auto found = lines_.find(side);
        if (found == lines_.end()) found = lines_.emplace(side, TextPositions(at(side).worktree.utf8)).first;
        return found->second;
    }

private:
    const FileSnapshots &snapshots_;
    std::map<SourceSide, TextPositions> lines_;
};
}

ChoicePreviewFlow::ChoicePreviewFlow(SessionStore &store, const SessionModel &files, const ResultEditorAdapter &editor,
                                     const Selection &selection, CommandFeedback &feedback, QObject *parent)
    : QObject(parent), store_(store), files_(files), editor_(editor), selection_(selection), feedback_(feedback)
{
    connect(this, &ChoicePreviewFlow::changed, this, [this](Scope scope) {
        switch (scope) {
        case Scope::Choice: Q_EMIT choicePreviewChanged(); break;
        case Scope::NonConflicting: Q_EMIT nonConflictingPreviewChanged(); break;
        case Scope::WholeFile: Q_EMIT wholeFilePreviewChanged(); break;
        }
    });
    // A preview belongs to the file selected when it was made.
    connect(&selection_, &Selection::changed, this, &ChoicePreviewFlow::cancel);
    // The view reads the pending file, which a session replacement can remove.
    connect(&store_, &SessionStore::aboutToReplace, this, &ChoicePreviewFlow::cancel);
}

std::optional<ChoicePreviewFlow::Scope> ChoicePreviewFlow::pendingScope() const
{
    if (std::holds_alternative<ChoicePreview>(pending_)) return Scope::Choice;
    if (std::holds_alternative<NonConflictingPreview>(pending_)) return Scope::NonConflicting;
    if (std::holds_alternative<WholeFilePreview>(pending_)) return Scope::WholeFile;
    return std::nullopt;
}

bool ChoicePreviewFlow::preview(Scope scope, const QString &fileId, const QString &revision, Option option,
                                const QString &target)
{
    cancel();
    const auto checked =
        checkCommand(store_, selection_.fileId(), editor_.composing(), fileId, revision, CommandCheck::Selected);
    if (const auto *refused = std::get_if<Transition>(&checked)) return feedback_.report(*refused);
    const auto &[file, expected] = std::get<CommandTarget>(checked);
    if (file->current().id != expected) return feedback_.report(Transition::StaleRevision);
    if (!file->pendingAlignment()) return prepare(scope, fileId, expected, option, target);
    const auto request = request_;
    waiting_ = true;
    Q_EMIT previewPendingChanged();
    feedback_.show(tr("Waiting for the current result to align before previewing."));
    store_.awaitAlignment(fileId.toStdString(), expected)
        .then(this, [this, request, scope, fileId, expected, option, target](Transition result) {
            if (request != request_ || selection_.fileId() != fileId) return;
            waiting_ = false;
            if (result != Transition::Applied && result != Transition::NoChange) {
                Q_EMIT previewPendingChanged();
                feedback_.report(result);
                return;
            }
            prepare(scope, fileId, expected, option, target);
            Q_EMIT previewPendingChanged();
        });
    return true;
}

bool ChoicePreviewFlow::prepare(Scope scope, const QString &fileId, RevisionId expected, Option option,
                                const QString &target)
{
    const auto *file = store_.file(fileId.toStdString());
    if (!file || file->current().id != expected || selection_.fileId() != fileId || editor_.composing())
        return feedback_.report(Transition::StaleRevision);
    decltype(pending_) pending;
    QString message;
    bool accepted = false;
    switch (scope) {
    case Scope::Choice: {
        const auto *value = std::get_if<Action>(&option);
        const auto action = value ? choiceAction(*value) : std::nullopt;
        if (!action) return feedback_.report(Transition::InvalidAnchor);
        auto preview = branch_truce::previewChoice(*file, expected, target.toStdString(), *action);
        message = engine_text::choice(preview);
        accepted = preview.state == Transition::Applied || preview.state == Transition::ConfirmationRequired;
        pending = std::move(preview);
        break;
    }
    case Scope::NonConflicting: {
        const auto *value = std::get_if<NonConflicting>(&option);
        const auto kind = value ? nonConflictingScope(*value) : std::nullopt;
        if (!kind) return feedback_.report(Transition::InvalidAnchor);
        auto preview = branch_truce::previewNonConflicting(*file, expected, *kind);
        message = (preview.state == Transition::NoChange ? tr("No eligible non-conflicting changes. ") : QString{})
            + engine_text::batch(preview.note);
        accepted = preview.state == Transition::ConfirmationRequired;
        pending = std::move(preview);
        break;
    }
    case Scope::WholeFile: {
        const auto *value = std::get_if<Side>(&option);
        const auto side = value ? wholeFileSide(*value) : std::nullopt;
        if (!side) return feedback_.report(Transition::InvalidAnchor);
        auto preview = branch_truce::previewWholeFile(*file, expected, *side);
        message = engine_text::wholeFile(preview.note);
        accepted = preview.state == Transition::ConfirmationRequired;
        pending = std::move(preview);
        break;
    }
    }
    feedback_.show(message);
    if (accepted) {
        pending_ = std::move(pending);
        fileId_ = fileId;
        Q_EMIT changed(scope);
        Q_EMIT ready(scope);
    }
    return accepted;
}

bool ChoicePreviewFlow::apply(Scope scope, bool confirm)
{
    if (pendingScope() != scope || fileId_ != selection_.fileId()) return feedback_.report(Transition::StaleRevision);
    if (editor_.composing()) return feedback_.report(Transition::Blocked);
    const auto pending = pending_;
    const auto id = fileId_.toStdString();
    QFuture<Transition> result;
    if (const auto *choice = std::get_if<ChoicePreview>(&pending))
        result = store_.applyChoice(id, *choice, confirm);
    else if (const auto *changes = std::get_if<NonConflictingPreview>(&pending))
        result = store_.applyNonConflicting(id, *changes, confirm);
    else if (const auto *whole = std::get_if<WholeFilePreview>(&pending))
        result = store_.applyWholeFile(id, *whole, confirm);
    if (!result.isValid()) return feedback_.report(Transition::Blocked);
    const auto request = request_;
    const auto settled = [this, scope, request](Transition outcome) {
        if (request != request_) return false;
        if (outcome == Transition::Applied || outcome == Transition::NoChange) {
            pending_ = std::monostate{};
            fileId_.clear();
            Q_EMIT changed(scope);
        }
        return feedback_.report(outcome);
    };
    if (result.isFinished()) return settled(result.result());
    feedback_.show(tr("Command pending. Waiting for durable recovery."));
    result.then(this, settled);
    return true;
}

void ChoicePreviewFlow::cancel()
{
    ++request_;
    if (std::exchange(waiting_, false)) Q_EMIT previewPendingChanged();
    const auto previous = pendingScope();
    pending_ = std::monostate{};
    fileId_.clear();
    feedback_.show({});
    if (previous) Q_EMIT changed(*previous);
}

QVariantMap ChoicePreviewFlow::view(Scope scope) const
{
    const auto *file = store_.file(fileId_.toStdString());
    if (!file || !file->snapshots()) return {};
    switch (scope) {
    case Scope::Choice:
        if (const auto *preview = std::get_if<ChoicePreview>(&pending_)) return choiceView(*preview, *file);
        break;
    case Scope::NonConflicting:
        if (const auto *preview = std::get_if<NonConflictingPreview>(&pending_))
            return nonConflictingView(*preview, *file);
        break;
    case Scope::WholeFile:
        if (const auto *preview = std::get_if<WholeFilePreview>(&pending_)) return wholeFileView(*preview, *file);
        break;
    }
    return {};
}

QString ChoicePreviewFlow::sideLabel(SourceSide side) { return side == SourceSide::Stage2 ? tr("Left") : tr("Right"); }

QString ChoicePreviewFlow::rangeLabel(const QString &label, const TextPositions &lines, ByteRange range)
{
    return range.begin == range.end
        ? tr("%1, empty scope at line %2").arg(label).arg(lines.line(range.begin))
        : tr("%1, lines %2–%3").arg(label).arg(lines.line(range.begin)).arg(lines.line(range.end - 1));
}

QVariantMap ChoicePreviewFlow::choiceView(const ChoicePreview &preview, const FileSession &file) const
{
    Sources sources(*file.snapshots());
    QStringList scopes;
    for (const auto &scope : preview.sourceOrder) {
        scopes.push_back(tr("%1 · %2").arg(
            rangeLabel(sideLabel(scope.side), sources.lines(scope.side), scope.range),
            sourceIdentity(utf8(sources.at(scope.side).label),
                           sources.at(scope.side).commitId ? utf8(*sources.at(scope.side).commitId) : QString{})));
    }
    return {{QStringLiteral("before"), utf8(preview.before)},
            {QStringLiteral("replacement"), utf8(preview.replacement)},
            {QStringLiteral("detail"), engine_text::choice(preview)},
            {QStringLiteral("orderedCombination"), !preview.sourceOrder.empty()},
            {QStringLiteral("order"),
             preview.action == ChoiceAction::CombineLeftFirst ? tr("Left then Right") : tr("Right then Left")},
            {QStringLiteral("sourceScopes"), scopes.join(QLatin1Char('\n'))},
            {QStringLiteral("replacesManual"), preview.replacesManual},
            {QStringLiteral("confirmationRequired"), preview.state == Transition::ConfirmationRequired}};
}

QVariantMap ChoicePreviewFlow::nonConflictingView(const NonConflictingPreview &preview, const FileSession &file) const
{
    Sources sources(*file.snapshots());
    QVariantList changes;
    for (const auto &change : preview.changes) {
        QStringList scopes;
        for (const auto &source : change.sources) {
            const auto side = sideLabel(source.side);
            const auto &lines = sources.lines(source.side);
            const auto label =
                sourceIdentity(utf8(sources.at(source.side).label),
                               sources.at(source.side).commitId ? utf8(*sources.at(source.side).commitId) : QString{});
            scopes.push_back(
                source.range.begin == source.range.end
                    ? tr("%1 deletion at line %2 · %3").arg(side).arg(lines.line(source.range.begin)).arg(label)
                    : tr("%1 lines %2–%3 · %4")
                          .arg(side)
                          .arg(lines.line(source.range.begin))
                          .arg(lines.line(source.range.end - 1))
                          .arg(label));
        }
        changes.push_back(QVariantMap{{QStringLiteral("scope"), scopes.join(QLatin1Char('\n'))},
                                      {QStringLiteral("before"), utf8(change.before)},
                                      {QStringLiteral("replacement"), utf8(change.replacement)}});
    }
    return {{QStringLiteral("path"), files_.displayPath(fileId_.toStdString())},
            {QStringLiteral("scope"),
             preview.scope == NonConflictingScope::Left        ? tr("Left")
                 : preview.scope == NonConflictingScope::Right ? tr("Right")
                                                               : tr("Left and Right")},
            {QStringLiteral("detail"), engine_text::batch(preview.note)},
            {QStringLiteral("changes"), changes},
            {QStringLiteral("summary"),
             tr("%1 · %2").arg(
                 tr("%n eligible change(s)", nullptr, static_cast<int>(preview.changes.size())),
                 tr("%n other non-conflicting change(s) skipped", nullptr, static_cast<int>(preview.skipped)))}};
}

QVariantMap ChoicePreviewFlow::wholeFileView(const WholeFilePreview &preview, const FileSession &file) const
{
    const auto &captured = *file.snapshots();
    const auto &selected = snapshotFor(captured, preview.side);
    if (!selected || captured.provenance.sessionId != preview.origin.sessionId
        || selected->objectId != preview.origin.sourceObjectId)
        return {};
    const auto decisionLabel = [](const Decision &decision) {
        auto label = decision.value == DecisionValue::Accepted ? tr("accepted")
            : decision.value == DecisionValue::Ignored         ? tr("ignored")
                                                               : tr("undecided");
        if (decision.order) label = tr("%1 · order %2").arg(label).arg(*decision.order);
        return label;
    };
    Sources sources(captured);
    QVariantList contributions;
    for (const auto &contribution : preview.contributions) {
        const auto side = contribution.source.side;
        const auto &value = sources.at(side).worktree.utf8;
        contributions.push_back(
            QVariantMap{{QStringLiteral("scope"),
                         tr("%1 · %2").arg(
                             rangeLabel(sideLabel(side), sources.lines(side), contribution.source.range),
                             sourceIdentity(utf8(sources.at(side).label),
                                            sources.at(side).commitId ? utf8(*sources.at(side).commitId) : QString{}))},
                        {QStringLiteral("sourceText"),
                         utf8(value.substr(contribution.source.range.begin,
                                           contribution.source.range.end - contribution.source.range.begin))},
                        {QStringLiteral("beforeState"), decisionLabel(contribution.before)},
                        {QStringLiteral("afterState"), decisionLabel(contribution.after)}});
    }
    const TextPositions before(preview.before);
    const TextPositions replacement(preview.replacement);
    QVariantList differences;
    for (const auto &passage : preview.differences) {
        differences.push_back(QVariantMap{
            {QStringLiteral("scope"),
             tr("%1 → %2").arg(rangeLabel(tr("Current result"), before, passage.result),
                               rangeLabel(tr("Selected source"), replacement, passage.source))},
            {QStringLiteral("before"),
             utf8(preview.before.substr(passage.result.begin, passage.result.end - passage.result.begin))},
            {QStringLiteral("replacement"),
             utf8(preview.replacement.substr(passage.source.begin, passage.source.end - passage.source.begin))}});
    }
    return {{QStringLiteral("path"), files_.displayPath(fileId_.toStdString())},
            {QStringLiteral("side"), sideLabel(preview.side)},
            {QStringLiteral("source"),
             tr("%1\n%2").arg(
                 sourceIdentity(utf8(selected->label), selected->commitId ? utf8(*selected->commitId) : QString{}),
                 utf8(selected->objectId))},
            {QStringLiteral("before"), utf8(preview.before)},
            {QStringLiteral("replacement"), utf8(preview.replacement)},
            {QStringLiteral("detail"), engine_text::wholeFile(preview.note)},
            {QStringLiteral("contributions"), contributions},
            {QStringLiteral("differences"), differences},
            {QStringLiteral("retainedDecisions"),
             preview.retainedDecisions ? tr("%n earlier decision(s) without a source scope are retained.", nullptr,
                                            static_cast<int>(preview.retainedDecisions))
                                       : QString{}}};
}
}
