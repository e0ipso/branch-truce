// File presentation turns a stored file's facts and recovery state into the states and sentences views show.
#include "FilePresentation.h"
#include "EngineText.h"
#include "QtText.h"
#include <array>

namespace branch_truce {
namespace {
using FileStates::HandoffCategory;
using FileStates::NextAction;
using FileStates::StatusKind;

// One row per handoff category, in handoff order. Icon names are written as
// designated initializers so the theme icon check collects them from this file.
struct Category {
    HandoffCategory category;
    const char *key;
    const char *icon;
};
constexpr std::array categories{
    Category{.category = HandoffCategory::Undecided, .key = "undecided", .icon = "vcs-conflicting"},
    Category{.category = HandoffCategory::SavedDraft, .key = "saved-draft", .icon = "document-save"},
    Category{.category = HandoffCategory::ReviewNeeded, .key = "review-needed", .icon = "vcs-locally-modified"},
    Category{.category = HandoffCategory::NewerDraft, .key = "newer-draft", .icon = "vcs-update-required"},
    Category{
        .category = HandoffCategory::WrittenUnresolved, .key = "written-unresolved", .icon = "vcs-locally-modified"},
    Category{.category = HandoffCategory::Failed, .key = "failed", .icon = "dialog-error"},
    Category{.category = HandoffCategory::OutsideApp, .key = "outside-app", .icon = "unknown"},
    // The emblem is the positive check tile; the action named checkmark is monochrome.
    Category{.category = HandoffCategory::Published, .key = "published", .icon = "emblem-checked"},
};

const Category &categoryOf(HandoffCategory category)
{
    for (const auto &entry : categories)
        if (entry.category == category) return entry;
    return categories.back();
}

// One status fact. tone is neutral, positive, negative or warning.
struct Chip {
    const char *icon;
    QString text;
    const char *tone;
};

QVariantMap chipData(const Chip &chip)
{
    return {{QStringLiteral("icon"), QString::fromLatin1(chip.icon)},
            {QStringLiteral("text"), chip.text},
            {QStringLiteral("tone"), QString::fromLatin1(chip.tone)}};
}

static_assert(static_cast<int>(StatusKind::OutsideApp) == static_cast<int>(FileStatus::OutsideApp)
                  && static_cast<int>(StatusKind::ReviewNeeded) == static_cast<int>(FileStatus::ReviewNeeded),
              "StatusKind mirrors FileStatus");
static_assert(static_cast<int>(FileStates::SupportKind::LargeFile) == static_cast<int>(Support::LargeFile),
              "SupportKind mirrors Support");
}

FileStates::SupportKind FilePresentation::supportKind(Support support)
{
    return static_cast<FileStates::SupportKind>(support);
}

QString FilePresentation::categoryKey(HandoffCategory category)
{
    return QString::fromLatin1(categoryOf(category).key);
}

QString FilePresentation::categoryIcon(HandoffCategory category)
{
    return QString::fromLatin1(categoryOf(category).icon);
}

QString FilePresentation::categoryTitle(HandoffCategory category)
{
    switch (category) {
    case HandoffCategory::Undecided: return tr("Unresolved decisions");
    case HandoffCategory::SavedDraft: return tr("Saved drafts in Branch Truce; repository files unchanged");
    case HandoffCategory::ReviewNeeded: return tr("Review needed");
    case HandoffCategory::NewerDraft: return tr("Newer drafts than the result staged in Git");
    case HandoffCategory::WrittenUnresolved: return tr("Written, not yet resolved in Git");
    case HandoffCategory::Failed: return tr("Failed or uncertain work");
    case HandoffCategory::OutsideApp: return tr("Files to handle outside Branch Truce");
    case HandoffCategory::Published: return tr("Written and resolved in Git");
    }
    return {};
}

bool FilePresentation::resolved(std::size_t row) const
{
    const auto &id = store_.files()[row].id();
    return !store_.pendingAction(id) && !store_.hasRecoveryFailure(id) && store_.facts(row).resolved;
}

bool FilePresentation::unsaved(std::size_t row) const
{
    return store_.pendingAction(store_.files()[row].id()) || store_.facts(row).unsaved;
}

FilePresentation::Progress FilePresentation::progress(std::size_t row) const
{
    const auto &facts = store_.facts(row);
    const auto &id = store_.files()[row].id();
    const bool pending = store_.pendingAction(id).has_value();
    const bool recoveryClear = !pending && !store_.hasRecoveryFailure(id);
    return {facts.undecided,
            !pending && !facts.unsaved,
            !facts.needsReview,
            resolved(row),
            facts.earlierStaged,
            store_.files()[row].conflictCount(),
            recoveryClear && facts.settled};
}

// Dimensions come from the current revision and the saved, reviewed and staged
// revisions. They stay separate so one resolved flag cannot hide the others.
QVariantMap FilePresentation::progressData(std::size_t row) const
{
    const auto &file = store_.files()[row];
    const auto &publication = file.publication();
    const auto facts = progress(row);
    return {
        {QStringLiteral("undecided"), QVariant::fromValue(static_cast<qulonglong>(facts.undecided))},
        {QStringLiteral("conflicts"), QVariant::fromValue(static_cast<qulonglong>(facts.conflicts))},
        {QStringLiteral("saved"), facts.saved},
        {QStringLiteral("reviewed"), facts.reviewed},
        {QStringLiteral("resolved"), facts.resolved},
        {QStringLiteral("earlierResolved"), facts.earlierStaged},
        {QStringLiteral("settled"), facts.settled},
        {QStringLiteral("currentRevision"), qt_text::revision(file.current().id)},
        {QStringLiteral("savedRevision"),
         facts.saved && publication.checkpoint() ? qt_text::revision(*publication.checkpoint()) : QString{}},
        {QStringLiteral("reviewedRevision"),
         facts.reviewed ? qt_text::revision(publication.reviewed()->revision) : QString{}},
        {QStringLiteral("stagedRevision"),
         publication.staged() ? qt_text::revision(publication.staged()->revision) : QString{}}};
}

StatusKind FilePresentation::statusKind(std::size_t row) const
{
    const auto &id = store_.files()[row].id();
    if (store_.hasRecoveryFailure(id)) return StatusKind::RecoveryBlocked;
    if (const auto action = store_.pendingAction(id))
        return *action == SessionStore::Action::ManualSave ? StatusKind::CheckpointFailed : StatusKind::RecoveryPending;
    return static_cast<StatusKind>(store_.facts(row).status);
}

FileStates::RecoveryState FilePresentation::recoveryState(std::size_t row) const
{
    using FileStates::RecoveryState;
    const auto &id = store_.files()[row].id();
    if (store_.hasRecoveryFailure(id)) return RecoveryState::Blocked;
    if (const auto action = store_.pendingAction(id))
        return *action == SessionStore::Action::ManualSave ? RecoveryState::SaveFailed : RecoveryState::Failed;
    return store_.restored(id) ? RecoveryState::Restored : RecoveryState::Durable;
}

QString FilePresentation::status(std::size_t row) const
{
    switch (const auto kind = statusKind(row)) {
    case StatusKind::RecoveryBlocked: return tr("Recovery requires attention; work remains outstanding");
    case StatusKind::RecoveryPending: return tr("Recovery failed; pending input retained");
    default: return engine_text::status(static_cast<FileStatus>(kind));
    }
}

// In this order: a recovery problem, a file the app cannot edit, a resolved
// file, a publication or Save outcome to act on, the conflicts left, the
// undecided choices, then Mark resolved as the next step.
QString FilePresentation::statusLine(std::size_t row) const
{
    const auto &file = store_.files()[row];
    const auto kind = statusKind(row);
    if (kind == StatusKind::RecoveryBlocked || store_.pendingAction(file.id())) return status(row);
    switch (file.support()) {
    case Support::Unreadable: return tr("Load error");
    case Support::LargeFile: return tr("Large file not loaded");
    case Support::OutsideApp: return tr("Handle outside the app");
    case Support::Supported: break;
    }
    if (resolved(row)) return tr("Resolved");
    switch (kind) {
    case StatusKind::WriteFailed:
    case StatusKind::WrittenUnresolved:
    case StatusKind::CheckpointFailed:
    case StatusKind::OutcomeUncertain:
    case StatusKind::EarlierStaged: return status(row);
    default: break;
    }
    const auto facts = progress(row);
    if (facts.conflicts > 0) return tr("%n conflict(s) left", nullptr, static_cast<int>(facts.conflicts));
    if (facts.undecided > 0) return tr("%n undecided", nullptr, static_cast<int>(facts.undecided));
    if (facts.settled) return tr("Ready to mark resolved");
    return status(row);
}

QVariantList FilePresentation::chips(std::size_t row) const
{
    const auto facts = progress(row);
    QVariantList result{chipData({.icon = "", .text = status(row), .tone = "neutral"})};
    // Outside-app and unreadable files have no app draft to describe.
    if (store_.files()[row].support() == Support::Supported) {
        result.push_back(chipData(
            unsaved(row)
                ? Chip{.icon = "document-edit", .text = tr("Unsaved draft"), .tone = "warning"}
                : Chip{.icon = "document-save", .text = tr("Manual Save checkpoint retained"), .tone = "neutral"}));
    }
    const bool undecided = facts.undecided > 0;
    result.push_back(chipData({.icon = undecided ? "dialog-question" : "checkmark",
                               .text = tr("Undecided: %1").arg(static_cast<qulonglong>(facts.undecided)),
                               .tone = undecided ? "negative" : "positive"}));
    result.push_back(chipData(facts.saved
                                  ? Chip{.icon = "document-save", .text = tr("Saved"), .tone = "positive"}
                                  : Chip{.icon = "vcs-locally-modified", .text = tr("Not saved"), .tone = "warning"}));
    result.push_back(chipData(facts.reviewed
                                  ? Chip{.icon = "view-visible", .text = tr("Reviewed"), .tone = "positive"}
                                  : Chip{.icon = "view-hidden", .text = tr("Not reviewed"), .tone = "neutral"}));
    result.push_back(chipData(facts.resolved
                                  ? Chip{.icon = "checkmark", .text = tr("Resolved"), .tone = "positive"}
                                  : Chip{.icon = "vcs-conflicting", .text = tr("Not resolved"), .tone = "neutral"}));
    if (facts.earlierStaged) {
        result.push_back(chipData({.icon = "vcs-update-required",
                                   .text = tr("Earlier result staged; current draft not applied"),
                                   .tone = "warning"}));
    }
    return result;
}

QString FilePresentation::summary(std::size_t row) const
{
    const auto facts = progress(row);
    return tr("%1. Undecided: %2. %3. %4. %5.")
        .arg(status(row), QString::number(static_cast<qulonglong>(facts.undecided)),
             facts.saved ? tr("Saved") : tr("Not saved"), facts.reviewed ? tr("Reviewed") : tr("Not reviewed"),
             facts.resolved ? tr("Resolved") : tr("Not resolved"));
}

QStringList FilePresentation::blockers(std::size_t row) const
{
    const auto &id = store_.files()[row].id();
    QStringList result;
    for (const auto blocker : store_.facts(row).blockers) result.push_back(engine_text::blocker(blocker));
    if (const auto action = store_.pendingAction(id))
        result.push_back(*action == SessionStore::Action::ManualSave
                             ? tr("Draft save failed; the edited result is still available")
                             : tr("Pending input needs recovery retry"));
    if (const auto *failure = store_.recoveryFailure(id)) result.push_back(*failure);
    return result;
}

FilePresentation::Route FilePresentation::route(std::size_t row) const
{
    const auto &facts = store_.facts(row);
    const auto &id = store_.files()[row].id();
    if (store_.hasRecoveryFailure(id))
        return {HandoffCategory::Failed, NextAction::OpenRecoveryProblem, tr("Open the recovery problem")};
    if (const auto action = store_.pendingAction(id)) {
        return *action == SessionStore::Action::ManualSave
            ? Route{HandoffCategory::Failed, NextAction::RetrySave, tr("Retry Save")}
            : Route{HandoffCategory::Failed, NextAction::RetryRecovery, tr("Retry recovery")};
    }
    switch (facts.status) {
    case FileStatus::OutsideApp:
        return {HandoffCategory::OutsideApp, NextAction::ExplainOutsideApp,
                tr("See why it needs work outside Branch Truce")};
    case FileStatus::Unreadable:
        return {HandoffCategory::Failed, NextAction::Inspect, tr("See why it could not be read")};
    case FileStatus::LargeFile:
        return {HandoffCategory::Undecided, NextAction::LoadLargeFile, tr("Load the large file")};
    case FileStatus::OutcomeUncertain:
        return {HandoffCategory::Failed, NextAction::Inspect, tr("Inspect the uncertain outcome")};
    case FileStatus::CheckpointFailed: return {HandoffCategory::Failed, NextAction::RetrySave, tr("Retry Save")};
    case FileStatus::WriteFailed:
    case FileStatus::StageFailed: return {HandoffCategory::Failed, NextAction::MarkResolved, tr("Retry Mark resolved")};
    case FileStatus::EarlierStaged:
        return {HandoffCategory::NewerDraft, NextAction::ApplyOrDiscard,
                tr("Mark resolved or discard back to the staged result")};
    case FileStatus::Resolved: return {HandoffCategory::Published, NextAction::None, {}};
    case FileStatus::WrittenUnresolved:
        return {HandoffCategory::WrittenUnresolved, NextAction::CheckGit, tr("Check Git resolution")};
    case FileStatus::SavedDraft:
        return {HandoffCategory::SavedDraft, NextAction::MarkResolved, tr("Review and Mark resolved")};
    case FileStatus::Undecided: return {HandoffCategory::Undecided, NextAction::Decide, tr("Decide source choices")};
    case FileStatus::ReviewNeeded:
        return {HandoffCategory::ReviewNeeded, NextAction::MarkResolved,
                facts.unsaved ? tr("Save or Mark resolved") : tr("Review and Mark resolved")};
    }
    return {};
}
}
