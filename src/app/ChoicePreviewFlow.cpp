#include "ChoicePreviewFlow.h"
#include "CommandGuard.h"
#include "ResultEditorAdapter.h"
#include "SessionModel.h"
#include "TextPositions.h"
#include <QStringList>
#include <map>

namespace branch_truce {
namespace {
QString text(const std::string &value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

std::optional<ChoiceAction> choiceAction(const QString &action)
{
    if (action == QLatin1String("accept")) return ChoiceAction::Accept;
    if (action == QLatin1String("ignore")) return ChoiceAction::Ignore;
    if (action == QLatin1String("resolve")) return ChoiceAction::ResolveUsingSide;
    if (action == QLatin1String("left-first")) return ChoiceAction::CombineLeftFirst;
    if (action == QLatin1String("right-first")) return ChoiceAction::CombineRightFirst;
    return std::nullopt;
}

std::optional<NonConflictingScope> nonConflictingScope(const QString &scope)
{
    if (scope == QLatin1String("left")) return NonConflictingScope::Left;
    if (scope == QLatin1String("right")) return NonConflictingScope::Right;
    if (scope == QLatin1String("both")) return NonConflictingScope::Both;
    return std::nullopt;
}

std::optional<SourceSide> wholeFileSide(const QString &side)
{
    if (side == QLatin1String("left")) return SourceSide::Stage2;
    if (side == QLatin1String("right")) return SourceSide::Stage3;
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

ChoicePreviewFlow::ChoicePreviewFlow(SessionModel &files, ResultEditorAdapter &editor, Owners owners, QObject *parent)
    : QObject(parent), files_(files), editor_(editor), owners_(std::move(owners))
{
}

bool ChoicePreviewFlow::preview(Scope scope, const QString &fileId, const QString &revision,
                                const QString &option, const QString &target)
{
    cancel();
    const auto checked = checkCommand(files_, owners_.selectedFileId(), editor_.composing(), fileId, revision,
                                      CommandCheck::Selected);
    if (const auto *refused = std::get_if<Transition>(&checked)) return owners_.report(*refused);
    const auto &[file, expected] = std::get<CommandTarget>(checked);
    decltype(pending_) pending;
    QString message;
    bool accepted = false;
    switch (scope) {
    case Scope::Choice: {
        const auto action = choiceAction(option);
        if (!action) return owners_.report(Transition::InvalidAnchor);
        auto preview = previewChoice(*file, expected, target.toStdString(), *action);
        message = text(preview.detail);
        accepted = preview.state == Transition::Applied || preview.state == Transition::ConfirmationRequired;
        pending = std::move(preview);
        break;
    }
    case Scope::NonConflicting: {
        const auto kind = nonConflictingScope(option);
        if (!kind) return owners_.report(Transition::InvalidAnchor);
        auto preview = previewNonConflicting(*file, expected, *kind);
        message = (preview.state == Transition::NoChange ? tr("No eligible non-conflicting changes. ") : QString{})
            + text(preview.detail);
        accepted = preview.state == Transition::ConfirmationRequired;
        pending = std::move(preview);
        break;
    }
    case Scope::WholeFile: {
        const auto side = wholeFileSide(option);
        if (!side) return owners_.report(Transition::InvalidAnchor);
        auto preview = previewWholeFile(*file, expected, *side);
        message = text(preview.detail);
        accepted = preview.state == Transition::ConfirmationRequired;
        pending = std::move(preview);
        break;
    }
    }
    owners_.setMessage(message);
    if (accepted) {
        pending_ = std::move(pending);
        fileId_ = fileId;
    }
    Q_EMIT changed();
    return accepted;
}

bool ChoicePreviewFlow::apply(Scope scope, bool confirm)
{
    const bool held = scope == Scope::Choice ? std::holds_alternative<ChoicePreview>(pending_)
        : scope == Scope::NonConflicting ? std::holds_alternative<NonConflictingPreview>(pending_)
        : std::holds_alternative<WholeFilePreview>(pending_);
    if (!held || fileId_ != owners_.selectedFileId()) return owners_.report(Transition::StaleRevision);
    if (editor_.composing()) return owners_.report(Transition::Blocked);
    const auto pending = pending_;
    const auto id = fileId_.toStdString();
    auto result = Transition::Blocked;
    if (const auto *choice = std::get_if<ChoicePreview>(&pending)) result = files_.applyChoice(id, *choice, confirm);
    else if (const auto *changes = std::get_if<NonConflictingPreview>(&pending))
        result = files_.applyNonConflicting(id, *changes, confirm);
    else if (const auto *whole = std::get_if<WholeFilePreview>(&pending)) result = files_.applyWholeFile(id, *whole, confirm);
    // A single choice that replaces manual work stays pending until confirmed.
    if (scope != Scope::Choice || result != Transition::ConfirmationRequired) {
        pending_ = std::monostate{};
        fileId_.clear();
    }
    return owners_.report(result);
}

void ChoicePreviewFlow::cancel()
{
    pending_ = std::monostate{};
    fileId_.clear();
    owners_.setMessage({});
    Q_EMIT changed();
}

QVariantMap ChoicePreviewFlow::view(Scope scope) const
{
    const auto *file = files_.file(fileId_.toStdString());
    if (!file || !file->snapshots()) return {};
    switch (scope) {
    case Scope::Choice:
        if (const auto *preview = std::get_if<ChoicePreview>(&pending_)) return choiceView(*preview, *file);
        break;
    case Scope::NonConflicting:
        if (const auto *preview = std::get_if<NonConflictingPreview>(&pending_)) return nonConflictingView(*preview, *file);
        break;
    case Scope::WholeFile:
        if (const auto *preview = std::get_if<WholeFilePreview>(&pending_)) return wholeFileView(*preview, *file);
        break;
    }
    return {};
}

QString ChoicePreviewFlow::sideLabel(SourceSide side)
{
    return side == SourceSide::Stage2 ? tr("Left") : tr("Right");
}

QString ChoicePreviewFlow::rangeLabel(const QString &label, const TextPositions &lines, ByteRange range)
{
    return range.begin == range.end ? tr("%1, empty scope at line %2").arg(label).arg(lines.line(range.begin))
        : tr("%1, lines %2–%3").arg(label).arg(lines.line(range.begin)).arg(lines.line(range.end - 1));
}

QVariantMap ChoicePreviewFlow::choiceView(const ChoicePreview &preview, const FileSession &file) const
{
    Sources sources(*file.snapshots());
    QStringList scopes;
    for (const auto &scope : preview.sourceOrder) {
        scopes.push_back(rangeLabel(sideLabel(scope.side), sources.lines(scope.side), scope.range)
            + QStringLiteral(" · ") + text(sources.at(scope.side).label));
    }
    return {{QStringLiteral("before"), text(preview.before)},
        {QStringLiteral("replacement"), text(preview.replacement)},
        {QStringLiteral("detail"), text(preview.detail)},
        {QStringLiteral("orderedCombination"), !preview.sourceOrder.empty()},
        {QStringLiteral("order"), preview.action == ChoiceAction::CombineLeftFirst ? tr("Left then Right") : tr("Right then Left")},
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
            const auto label = text(sources.at(source.side).label);
            scopes.push_back(source.range.begin == source.range.end
                ? tr("%1 deletion at line %2 · %3").arg(side).arg(lines.line(source.range.begin)).arg(label)
                : tr("%1 lines %2–%3 · %4").arg(side).arg(lines.line(source.range.begin))
                    .arg(lines.line(source.range.end - 1)).arg(label));
        }
        changes.push_back(QVariantMap{{QStringLiteral("scope"), scopes.join(QLatin1Char('\n'))},
            {QStringLiteral("before"), text(change.before)}, {QStringLiteral("replacement"), text(change.replacement)}});
    }
    return {{QStringLiteral("path"), owners_.displayPath(fileId_)},
        {QStringLiteral("scope"), preview.scope == NonConflictingScope::Left ? tr("Left")
            : preview.scope == NonConflictingScope::Right ? tr("Right") : tr("Left and Right")},
        {QStringLiteral("detail"), text(preview.detail)}, {QStringLiteral("changes"), changes},
        {QStringLiteral("summary"), tr("%1 eligible changes · %2 other non-conflicting changes skipped")
            .arg(preview.changes.size()).arg(preview.skipped)}};
}

QVariantMap ChoicePreviewFlow::wholeFileView(const WholeFilePreview &preview, const FileSession &file) const
{
    const auto &captured = *file.snapshots();
    const auto &selected = snapshotFor(captured, preview.side);
    if (!selected || captured.provenance.sessionId != preview.origin.sessionId
        || selected->objectId != preview.origin.sourceObjectId) return {};
    const auto decisionLabel = [](const Decision &decision) {
        auto label = decision.value == DecisionValue::Accepted ? tr("accepted")
            : decision.value == DecisionValue::Ignored ? tr("ignored") : tr("undecided");
        if (decision.order) label += tr(" · order %1").arg(*decision.order);
        return label;
    };
    Sources sources(captured);
    QVariantList contributions;
    for (const auto &contribution : preview.contributions) {
        const auto side = contribution.source.side;
        const auto &value = sources.at(side).worktree.utf8;
        contributions.push_back(QVariantMap{
            {QStringLiteral("scope"), rangeLabel(sideLabel(side), sources.lines(side), contribution.source.range)
                + QStringLiteral(" · ") + text(sources.at(side).label)},
            {QStringLiteral("sourceText"), text(value.substr(contribution.source.range.begin,
                contribution.source.range.end - contribution.source.range.begin))},
            {QStringLiteral("beforeState"), decisionLabel(contribution.before)},
            {QStringLiteral("afterState"), decisionLabel(contribution.after)}});
    }
    const TextPositions before(preview.before);
    const TextPositions replacement(preview.replacement);
    QVariantList differences;
    for (const auto &passage : preview.differences) {
        differences.push_back(QVariantMap{
            {QStringLiteral("scope"), rangeLabel(tr("Current result"), before, passage.result)
                + QStringLiteral(" → ") + rangeLabel(tr("Selected source"), replacement, passage.source)},
            {QStringLiteral("before"), text(preview.before.substr(passage.result.begin, passage.result.end - passage.result.begin))},
            {QStringLiteral("replacement"), text(preview.replacement.substr(passage.source.begin, passage.source.end - passage.source.begin))}});
    }
    return {{QStringLiteral("path"), owners_.displayPath(fileId_)},
        {QStringLiteral("side"), sideLabel(preview.side)},
        {QStringLiteral("source"), text(selected->label) + QStringLiteral("\n") + text(selected->objectId)},
        {QStringLiteral("before"), text(preview.before)}, {QStringLiteral("replacement"), text(preview.replacement)},
        {QStringLiteral("detail"), text(preview.detail)}, {QStringLiteral("contributions"), contributions},
        {QStringLiteral("differences"), differences},
        {QStringLiteral("retainedDecisions"), preview.retainedDecisions
            ? tr("%1 earlier decisions without a source scope are retained.").arg(preview.retainedDecisions) : QString{}}};
}
}
