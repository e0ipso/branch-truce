// Choice previews show how a source action would change the result before the user confirms it.
#include "ChoicePreviewFlow.h"
#include "CommandFeedback.h"
#include "CommandGuard.h"
#include "EngineText.h"
#include "QtText.h"
#include "ResultEditorAdapter.h"
#include "RevisionWorkers.h"
#include "Selection.h"
#include "SessionModel.h"
#include "SourceIdentity.h"
#include "TextPositions.h"
#include "core/internal/ComputationInput.h"
#include <QStringList>
#include <map>
#include <optional>
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

using PendingPreview = std::variant<std::monostate, ChoicePreview, NonConflictingPreview, WholeFilePreview>;

Transition previewState(const PendingPreview &pending)
{
    return std::visit(
        [](const auto &preview) {
            if constexpr (std::is_same_v<std::decay_t<decltype(preview)>, std::monostate>)
                return Transition::Blocked;
            else
                return preview.state;
        },
        pending);
}

QString previewMessage(const PendingPreview &pending)
{
    return std::visit(
        [](const auto &preview) -> QString {
            using T = std::decay_t<decltype(preview)>;
            if constexpr (std::is_same_v<T, ChoicePreview>)
                return engine_text::choice(preview);
            else if constexpr (std::is_same_v<T, NonConflictingPreview>)
                return (preview.state == Transition::NoChange
                            ? ChoicePreviewFlow::tr("No eligible non-conflicting changes. ")
                            : QString{})
                    + engine_text::batch(preview.note);
            else if constexpr (std::is_same_v<T, WholeFilePreview>)
                return engine_text::wholeFile(preview.note);
            else
                return {};
        },
        pending);
}

// One file's captured sources, each numbered on first use.
class Sources {
public:
    explicit Sources(const FileSnapshots &snapshots) : snapshots_(snapshots) {}
    [[nodiscard]] const SourceSnapshot &at(SourceSide side) const
    {
        const auto &source = snapshotFor(snapshots_, side);
        if (source) return *source;
        throw std::bad_optional_access{};
    }
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
                                     const Selection &selection, CommandFeedback &feedback, RevisionWorkers &workers,
                                     QObject *parent)
    : QObject(parent),
      store_(store),
      files_(files),
      editor_(editor),
      selection_(selection),
      feedback_(feedback),
      workers_(workers)
{
    connect(this, &ChoicePreviewFlow::changed, this, [this](Scope scope) {
        switch (scope) {
        case Scope::Choice: Q_EMIT choicePreviewChanged(); break;
        case Scope::NonConflicting: Q_EMIT nonConflictingPreviewChanged(); break;
        case Scope::WholeFile: Q_EMIT wholeFilePreviewChanged(); break;
        }
    });
    connect(&store_, &SessionStore::fileChanged, this, [this](int) {
        const auto *file = store_.file(fileId_.toStdString());
        if (!fileId_.isEmpty()
            && (!file || file->current().id != expected_
                || (mapping_ && (file->correspondence() != mapping_ || file->snapshots() != captures_))))
            cancel();
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
    Q_ASSERT(QThread::currentThread() == thread());
    const auto request = request_ + 1;
    cancel();
    if (request != request_) return false;
    const auto checked =
        checkCommand(store_, selection_.fileId(), editor_.composing(), fileId, revision, CommandCheck::Selected);
    if (const auto *refused = std::get_if<Transition>(&checked)) return feedback_.report(*refused);
    const auto &[file, expected] = std::get<CommandTarget>(checked);
    if (file->current().id != expected) {
        if (scope == Scope::Choice) return feedback_.report(Transition::StaleRevision);
        feedback_.show(scope == Scope::NonConflicting ? engine_text::batch(BatchNote::StaleRevision)
                                                      : engine_text::wholeFile(WholeFileNote::StaleRevision));
        return false;
    }
    fileId_ = fileId;
    expected_ = expected;
    waiting_ = true;
    Q_EMIT previewPendingChanged();
    if (request != request_) return true;
    if (!file->pendingAlignment()) return prepare(scope, fileId, expected, option, target);
    feedback_.show(tr("Waiting for the current result to align before previewing."));
    store_.awaitAlignment(fileId.toStdString(), expected)
        .then(this, [this, request, scope, fileId, expected, option, target](Transition result) {
            if (request != request_) return;
            if (result == Transition::Applied || result == Transition::NoChange) {
                prepare(scope, fileId, expected, option, target);
            } else {
                cancel();
                feedback_.report(result);
            }
        });
    return true;
}

bool ChoicePreviewFlow::prepare(Scope scope, const QString &fileId, RevisionId expected, Option option,
                                const QString &target)
{
    const auto *file = store_.file(fileId.toStdString());
    const auto refuse = [this](Transition state) {
        cancel();
        return feedback_.report(state);
    };
    if (!file || file->current().id != expected || selection_.fileId() != fileId || editor_.composing())
        return refuse(Transition::StaleRevision);
    const auto action = std::get_if<Action>(&option);
    const auto kind = std::get_if<NonConflicting>(&option);
    const auto side = std::get_if<Side>(&option);
    if ((scope == Scope::Choice && (!action || !choiceAction(*action)))
        || (scope == Scope::NonConflicting && (!kind || !nonConflictingScope(*kind)))
        || (scope == Scope::WholeFile && (!side || !wholeFileSide(*side))))
        return refuse(Transition::InvalidAnchor);
    mapping_ = file->correspondence();
    const auto *mapping = mapping_;
    captures_ = file->snapshots();
    const auto *captures = captures_;
    const auto resultRevision = file->current().result;
    const auto decisions = file->current().decisions;
    const auto request = request_;
    using Pending = decltype(pending_);
    const auto accepted = workers_.submit(
        detail::ComputationInput::retainedBytes(*file), this, false,
        [file, scope, option, target = target.toStdString(), expected] {
            return [input = detail::ComputationInput(*file), scope, option, target, expected]() -> Pending {
                switch (scope) {
                case Scope::Choice:
                    return detail::planChoice(input.planning(), input.planner(), *input.revision.markers, expected,
                                              target, *choiceAction(std::get<Action>(option)));
                case Scope::NonConflicting:
                    return detail::planNonConflicting(input.planning(), input.targets, expected,
                                                      *nonConflictingScope(std::get<NonConflicting>(option)));
                case Scope::WholeFile:
                    return detail::planWholeFile(input.planning(), expected, *wholeFileSide(std::get<Side>(option)));
                }
                return {};
            };
        },
        [this, request, scope, fileId, expected, mapping, captures, resultRevision, decisions](const auto &computed) {
            if (request != request_) return;
            const auto *current = store_.file(fileId.toStdString());
            if (!current || selection_.fileId() != fileId || editor_.composing() || current->current().id != expected
                || current->current().result != resultRevision || current->current().decisions != decisions
                || current->correspondence() != mapping || current->snapshots() != captures) {
                cancel();
                feedback_.report(Transition::StaleRevision);
                return;
            }
            if (const auto *failure = std::get_if<Transition>(&computed)) {
                cancel();
                feedback_.report(*failure);
                return;
            }
            if (store_.hasPendingRecovery() || store_.hasRecoveryFailure(fileId.toStdString())) {
                cancel();
                feedback_.report(Transition::Blocked);
                return;
            }
            auto pending = std::get<Pending>(computed);
            const auto state = previewState(pending);
            const auto message = previewMessage(pending);
            if (state != Transition::Applied && state != Transition::ConfirmationRequired) {
                cancel();
                if (state == Transition::ResourceLimit || state == Transition::OutOfMemory
                    || state == Transition::ComputationFailed)
                    feedback_.report(state);
                else
                    feedback_.show(message);
                return;
            }
            feedback_.show(message);
            if (request != request_) return;
            pending_ = std::move(pending);
            waiting_ = false;
            Q_EMIT changed(scope);
            if (request != request_) return;
            // Deliver ready before pendingChanged: Main keeps its armed request
            // through this signal, including requests that needed alignment first.
            Q_EMIT ready(scope);
            Q_EMIT previewPendingChanged();
        });
    if (!accepted) return refuse(Transition::ResourceLimit);
    feedback_.show(tr("Preparing the preview."));
    return true;
}

bool ChoicePreviewFlow::apply(Scope scope, bool confirm)
{
    if (waiting_ || pendingScope() != scope || fileId_ != selection_.fileId())
        return feedback_.report(Transition::StaleRevision);
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
        const bool succeeded = outcome == Transition::Applied || outcome == Transition::NoChange;
        if (request != request_) {
            Q_EMIT applyFinished(scope, QString::number(request), succeeded);
            return succeeded;
        }
        if (outcome == Transition::Applied || outcome == Transition::NoChange) {
            pending_ = std::monostate{};
            fileId_.clear();
            Q_EMIT changed(scope);
            Q_EMIT previewPendingChanged();
        }
        feedback_.report(outcome);
        Q_EMIT applyFinished(scope, QString::number(request), succeeded);
        return succeeded;
    };
    if (result.isFinished()) return settled(result.result());
    feedback_.show(tr("Command pending. Waiting for durable recovery."));
    result.then(this, settled);
    return true;
}

void ChoicePreviewFlow::cancel()
{
    Q_ASSERT(QThread::currentThread() == thread());
    ++request_;
    const bool wasWaiting = std::exchange(waiting_, false);
    const auto previous = pendingScope();
    pending_ = std::monostate{};
    fileId_.clear();
    mapping_ = nullptr;
    captures_ = nullptr;
    feedback_.show({});
    if (previous) Q_EMIT changed(*previous);
    if (wasWaiting || previous) Q_EMIT previewPendingChanged();
}

QVariantMap ChoicePreviewFlow::view(Scope scope) const
{
    const auto *file = store_.file(fileId_.toStdString());
    if (!file || !file->snapshots() || file->current().id != expected_ || file->correspondence() != mapping_) return {};
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
        const auto &captured = sources.at(scope.side);
        scopes.push_back(tr("%1 · %2").arg(
            rangeLabel(sideLabel(scope.side), sources.lines(scope.side), scope.range),
            sourceIdentity(utf8(captured.label), captured.commitId ? utf8(*captured.commitId) : QString{})));
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
            const auto &captured = sources.at(source.side);
            const auto label =
                sourceIdentity(utf8(captured.label), captured.commitId ? utf8(*captured.commitId) : QString{});
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
        const auto &capturedSource = sources.at(side);
        const auto &value = capturedSource.worktree.utf8;
        contributions.push_back(QVariantMap{
            {QStringLiteral("scope"),
             tr("%1 · %2").arg(rangeLabel(sideLabel(side), sources.lines(side), contribution.source.range),
                               sourceIdentity(utf8(capturedSource.label),
                                              capturedSource.commitId ? utf8(*capturedSource.commitId) : QString{}))},
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
