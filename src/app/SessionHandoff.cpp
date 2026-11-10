// The session handoff says which files remain outstanding and whether the session may finish or be left.
#include "SessionHandoff.h"
#include "QtText.h"
#include "git/GitAdapter.h"
#include "session/SessionStore.h"
#include <QMetaEnum>

namespace branch_truce {
using qt_text::utf8;
namespace {
using FileStates::HandoffCategory;

constexpr HandoffCategory order[] = {HandoffCategory::Undecided,         HandoffCategory::SavedDraft,
                                     HandoffCategory::ReviewNeeded,      HandoffCategory::NewerDraft,
                                     HandoffCategory::WrittenUnresolved, HandoffCategory::Failed,
                                     HandoffCategory::OutsideApp,        HandoffCategory::Published};

QVariantMap entryData(const HandoffEntry &entry)
{
    return {{QStringLiteral("fileId"), utf8(entry.fileId)},
            {QStringLiteral("path"), entry.path},
            {QStringLiteral("category"), FilePresentation::categoryKey(entry.category)},
            {QStringLiteral("icon"), FilePresentation::categoryIcon(entry.category)},
            {QStringLiteral("status"), entry.status},
            {QStringLiteral("blockers"), entry.blockers},
            {QStringLiteral("unsaved"), entry.unsaved},
            {QStringLiteral("undecided"), QVariant::fromValue(static_cast<qulonglong>(entry.undecided))},
            {QStringLiteral("outstanding"), entry.outstanding},
            {QStringLiteral("action"), QVariant::fromValue(entry.action)},
            {QStringLiteral("actionLabel"), entry.actionLabel}};
}
}

QVariantMap Handoff::toVariant() const
{
    QVariantList groupData;
    QVariantMap counts;
    for (const auto &group : groups) {
        QVariantList entries;
        for (const auto &entry : group.entries) entries.push_back(entryData(entry));
        const auto key = FilePresentation::categoryKey(group.category);
        counts.insert(key, static_cast<int>(group.entries.size()));
        groupData.push_back(QVariantMap{{QStringLiteral("category"), key},
                                        {QStringLiteral("title"), FilePresentation::categoryTitle(group.category)},
                                        {QStringLiteral("entries"), entries}});
    }
    return {{QStringLiteral("clean"), clean},
            {QStringLiteral("totalFiles"), totalFiles},
            {QStringLiteral("outstandingFiles"), outstandingFiles},
            {QStringLiteral("counts"), counts},
            {QStringLiteral("groups"), groupData},
            {QStringLiteral("outcome"), outcome},
            {QStringLiteral("sessionBlockers"), sessionBlockers},
            {QStringLiteral("canLeave"), canLeave},
            {QStringLiteral("leaveBlocker"), leaveBlocker}};
}

Handoff SessionHandoff::build(const SessionStore &store, const SessionActivity &activity)
{
    const FilePresentation presentation(store);
    Handoff result;
    result.totalFiles = static_cast<int>(store.size());
    for (const auto category : order) result.groups.push_back({category, {}});
    bool filesResolved =
        store.size() > 0 && !store.hasPendingRecovery() && !store.hasRecoveryFailures() && !store.recording();
    for (std::size_t row = 0; row < store.size(); ++row) {
        const auto &file = store.files()[row];
        const auto route = presentation.route(row);
        HandoffEntry entry{file.id(),
                           git::displayPath(QByteArray(file.path().data(), static_cast<qsizetype>(file.path().size()))),
                           route.category,
                           presentation.status(row),
                           presentation.blockers(row),
                           presentation.unsaved(row),
                           store.facts(row).undecided,
                           !presentation.resolved(row),
                           route.action,
                           route.actionLabel};
        if (entry.outstanding) {
            ++result.outstandingFiles;
            filesResolved = false;
        }
        for (auto &group : result.groups)
            if (group.category == route.category) group.entries.push_back(std::move(entry));
    }
    if (activity.resolutionRunning)
        result.sessionBlockers.push_back(tr("A resolution is still running; its outcome is not known yet."));
    if (activity.unreleasedLaunches > 0)
        result.sessionBlockers.push_back(
            tr("Git is waiting on %n launched path(s) with no decision yet.", nullptr, activity.unreleasedLaunches));
    result.clean = filesResolved && result.sessionBlockers.isEmpty();
    if (store.size() == 0)
        result.outcome = tr("No files are loaded in this session.");
    else if (result.clean)
        result.outcome = tr("%n file(s) in this session are written and resolved in Git. Continue and commit in your "
                            "usual workflow; Branch Truce has not checked the code.",
                            nullptr, result.totalFiles);
    else if (filesResolved)
        result.outcome = tr("Every file is resolved, but this session is not finished.");
    else
        result.outcome = tr("%1 of %n file(s) in this session remain outstanding. This session is not finished.",
                            nullptr, result.totalFiles)
                             .arg(result.outstandingFiles);
    // Leaving must not lose input that never reached the recovery store, or
    // a publication whose outcome is still unknown.
    if (store.recording() && !store.hasPendingRecovery())
        result.leaveBlocker = tr("Recovery is still being written. Wait before leaving.");
    else if (store.hasPendingRecovery())
        result.leaveBlocker =
            tr("Pending input is not in recovery yet. Retry recovery before leaving so the draft is kept.");
    else if (activity.resolutionRunning)
        result.leaveBlocker = tr("Wait for the running resolution to finish before leaving.");
    result.canLeave = result.leaveBlocker.isEmpty() && !activity.ended;
    return result;
}
}
