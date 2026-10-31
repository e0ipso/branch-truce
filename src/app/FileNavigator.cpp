// File and conflict navigation keeps the selected file and result cursor on the requested work.
#include "FileNavigator.h"
#include "NavigationTargets.h"
#include "ResultEditorAdapter.h"
#include "SessionModel.h"
#include <QScopedValueRollback>
#include <vector>

namespace branch_truce {
namespace {
QString modeName(FileNavigator::Mode mode)
{
    switch (mode) {
    case FileNavigator::Mode::Difference: return QStringLiteral("difference");
    case FileNavigator::Mode::Undecided: return QStringLiteral("undecided");
    case FileNavigator::Mode::Conflict: return QStringLiteral("conflict");
    case FileNavigator::Mode::Unfinished: return QStringLiteral("unfinished");
    case FileNavigator::Mode::SavedOrReviewed: return QStringLiteral("saved-or-reviewed");
    }
    return {};
}
}
FileNavigator::FileNavigator(SessionModel &files, ResultEditorAdapter &editor, Owners owners, QObject *parent)
    : QObject(parent), files_(files), editor_(editor), owners_(std::move(owners))
{
}

bool FileNavigator::navigate(Mode mode, int direction)
{
    if (direction != -1 && direction != 1) return false;
    if (editor_.composing()) return false;
    if (mode == Mode::Difference || mode == Mode::Undecided || mode == Mode::Conflict)
        return navigateWithinFile(mode, direction);
    if (mode == Mode::Unfinished || mode == Mode::SavedOrReviewed)
        return navigateAcrossFiles(mode, direction);
    return false;
}

void FileNavigator::selectionChanged()
{
    if (!selecting_) clear();
}

void FileNavigator::clear()
{
    mode_.reset();
    fileId_.clear();
    revision_ = 0;
    index_ = -1;
    cursor_ = -1;
    selectionStart_ = -1;
    selectionEnd_ = -1;
    byteBegin_ = 0;
    byteEnd_ = 0;
    contribution_.clear();
    resultRange_ = false;
    if (target_.isEmpty()) return;
    target_.clear();
    Q_EMIT changed();
}

bool FileNavigator::navigateWithinFile(Mode mode, int direction)
{
    const auto selectedFileId = owners_.selectedFileId();
    const auto *file = files_.file(selectedFileId.toStdString());
    const auto fail = [&](const QString &label) {
        target_ = {{QStringLiteral("mode"), modeName(mode)},
            {QStringLiteral("fileId"), selectedFileId},
            {QStringLiteral("count"), 0},
            {QStringLiteral("label"), label}};
        mode_.reset();
        index_ = -1;
        Q_EMIT changed();
        return false;
    };
    if (!file || file->support() != Support::Supported)
        return fail(mode == Mode::Conflict ? tr("No conflicts to step through")
                                           : tr("This file has no editor differences."));
    const auto targets = mode == Mode::Difference ? differenceTargets(*file)
        : mode == Mode::Conflict ? conflictTargets(*file) : undecidedTargets(*file);
    if (targets.empty()) {
        return fail(mode == Mode::Difference
            ? tr("No differences in this file.") : mode == Mode::Conflict
            ? tr("No conflicts to step through") : tr("No undecided work in this file."));
    }
    const auto &result = file->current().draft.text;
    const int count = static_cast<int>(targets.size());
    const auto caret = editor_.caret();
    const auto at = [&](int index) -> const ScopeTarget & { return targets[static_cast<std::size_t>(index)]; };
    const bool continuing = mode_ == mode && fileId_ == selectedFileId
        && revision_ == file->current().id && index_ >= 0 && index_ < count
        && at(index_).byteBegin == byteBegin_ && at(index_).byteEnd == byteEnd_
        && at(index_).contributionId == contribution_ && at(index_).resultRange == resultRange_
        && (!editor_.attached() || (caret.cursor == cursor_ && caret.selectionStart == selectionStart_
            && caret.selectionEnd == selectionEnd_));
    const auto utfBegin = [&](const ScopeTarget &target) {
        return target.resultRange ? utf16Offset(result, target.byteBegin) : -1;
    };
    const auto utfEnd = [&](const ScopeTarget &target) {
        return target.resultRange ? utf16Offset(result, target.byteEnd) : -1;
    };
    int index = -1;
    if (continuing) {
        index = (index_ + direction + count) % count;
    } else {
        // Only targets the editor can select count. One pass in the walk's
        // direction finds the first of them, the first past the cursor, and
        // the one after the selected target, where the selected target is
        // the last in result order the editor selection covers exactly.
        int first = -1;
        int pastCursor = -1;
        int selected = -1;
        int afterSelected = -1;
        for (int step = 0; step < count; ++step) {
            const int candidate = direction > 0 ? step : count - 1 - step;
            const int begin = utfBegin(at(candidate));
            if (begin < 0) continue;
            if (first < 0) first = candidate;
            if (pastCursor < 0 && (direction > 0 ? begin >= caret.cursor : begin < caret.cursor))
                pastCursor = candidate;
            if (selected >= 0 && afterSelected < 0) afterSelected = candidate;
            const int end = utfEnd(at(candidate));
            if (candidate > selected && end >= 0 && caret.selectionStart == begin && caret.selectionEnd == end) {
                selected = candidate;
                afterSelected = -1;
            }
        }
        // Past the last selectable target the walk wraps to the first.
        if (selected >= 0) index = afterSelected >= 0 ? afterSelected : first;
        else if (pastCursor >= 0) index = pastCursor;
        else if (first >= 0) index = first;
        else index = direction > 0 ? 0 : count - 1;
    }
    const auto &target = at(index);
    const int begin = utfBegin(target);
    const int end = utfEnd(target);
    if (target.resultRange && editor_.attached()) {
        if (begin < 0 || end < 0 || !editor_.select(begin, end))
            return fail(tr("The difference could not be selected."));
    }
    mode_ = mode;
    fileId_ = selectedFileId;
    revision_ = file->current().id;
    index_ = index;
    byteBegin_ = target.byteBegin;
    byteEnd_ = target.byteEnd;
    contribution_ = target.contributionId;
    resultRange_ = target.resultRange;
    if (editor_.attached()) {
        const auto placed = editor_.caret();
        cursor_ = placed.cursor;
        selectionStart_ = placed.selectionStart;
        selectionEnd_ = placed.selectionEnd;
    } else {
        cursor_ = -1;
        selectionStart_ = -1;
        selectionEnd_ = -1;
    }
    const auto side = target.side == SourceSide::Stage2 ? tr("Left") : tr("Right");
    QString label;
    if (mode == Mode::Difference) {
        label = tr("Difference %1 of %2 · result bytes %3-%4")
            .arg(index + 1).arg(count)
            .arg(static_cast<qulonglong>(target.byteBegin))
            .arg(static_cast<qulonglong>(target.byteEnd));
    } else if (mode == Mode::Conflict) {
        label = tr("Conflict %1 of %2 · result bytes %3-%4")
            .arg(index + 1).arg(count)
            .arg(static_cast<qulonglong>(target.byteBegin))
            .arg(static_cast<qulonglong>(target.byteEnd));
    } else if (target.resultRange) {
        label = tr("Undecided %1 of %2 · %3 · result bytes %4-%5")
            .arg(index + 1).arg(count).arg(side)
            .arg(static_cast<qulonglong>(target.byteBegin))
            .arg(static_cast<qulonglong>(target.byteEnd));
    } else {
        label = tr("Undecided %1 of %2 · %3 · source bytes %4-%5, not mapped in the result")
            .arg(index + 1).arg(count).arg(side)
            .arg(static_cast<qulonglong>(target.byteBegin))
            .arg(static_cast<qulonglong>(target.byteEnd));
    }
    target_ = {{QStringLiteral("mode"), modeName(mode)},
        {QStringLiteral("fileId"), selectedFileId},
        {QStringLiteral("scope"), target.resultRange ? QStringLiteral("result") : QStringLiteral("unmapped-source")},
        {QStringLiteral("byteBegin"), QVariant::fromValue(static_cast<qulonglong>(target.byteBegin))},
        {QStringLiteral("byteEnd"), QVariant::fromValue(static_cast<qulonglong>(target.byteEnd))},
        {QStringLiteral("utf16Begin"), begin},
        {QStringLiteral("utf16End"), end},
        {QStringLiteral("contributionId"), QString::fromStdString(target.contributionId)},
        {QStringLiteral("index"), index},
        {QStringLiteral("count"), count},
        {QStringLiteral("label"), label}};
    Q_EMIT changed();
    return true;
}

bool FileNavigator::navigateAcrossFiles(Mode mode, int direction)
{
    struct Stop {
        int row = 0;
        QString id;
        QString path;
        QString support;
        bool saved = false;
        bool reviewed = false;
        bool resolved = false;
        bool earlierResolved = false;
        bool outside = false;
    };
    std::vector<Stop> stops;
    const bool unfinished = mode == Mode::Unfinished;
    for (int row = 0; row < files_.totalFiles(); ++row) {
        const auto *file = files_.fileAt(row);
        if (!file) continue;
        const auto id = QString::fromStdString(file->id());
        const auto progress = files_.progress(row);
        const bool outside = file->support() != Support::Supported;
        if (unfinished) {
            if (progress.resolved && !outside) continue;
        } else if (!progress.saved && !progress.reviewed) continue;
        QString support = QStringLiteral("supported");
        if (file->support() == Support::OutsideApp) support = QStringLiteral("outside-app");
        else if (file->support() == Support::Unreadable) support = QStringLiteral("unreadable");
        stops.push_back({row, id, owners_.displayPath(id), support, progress.saved, progress.reviewed, progress.resolved,
            progress.earlierStaged, outside});
    }
    if (stops.empty()) {
        target_ = {{QStringLiteral("mode"), modeName(mode)},
            {QStringLiteral("count"), 0},
            {QStringLiteral("label"), unfinished ? tr("No unfinished files.") : tr("No saved or reviewed files.")}};
        Q_EMIT changed();
        return false;
    }
    // The first stop past the selected row in the walk's direction, wrapping
    // to the first stop in that direction.
    const int current = owners_.selectedRow();
    const int size = static_cast<int>(stops.size());
    int chosen = direction > 0 ? 0 : size - 1;
    for (int step = 0; step < size; ++step) {
        const int candidate = direction > 0 ? step : size - 1 - step;
        if ((stops[static_cast<std::size_t>(candidate)].row - current) * direction > 0) {
            chosen = candidate;
            break;
        }
    }
    const auto &stop = stops[static_cast<std::size_t>(chosen)];
    if (stop.id != owners_.selectedFileId()) {
        QScopedValueRollback<bool> guard(selecting_, true);
        if (!owners_.selectFile(stop.id)) return false;
    }
    const auto scope = stop.outside ? QStringLiteral("outside-editor") : QStringLiteral("file");
    const QString label = unfinished && stop.outside
        ? tr("Unfinished file · outside the editor · %1").arg(stop.path)
        : unfinished ? tr("Unfinished file · %1").arg(stop.path)
                     : tr("Saved or reviewed file · %1").arg(stop.path);
    target_ = {{QStringLiteral("mode"), modeName(mode)},
        {QStringLiteral("fileId"), stop.id},
        {QStringLiteral("scope"), scope},
        {QStringLiteral("support"), stop.support},
        {QStringLiteral("path"), stop.path},
        {QStringLiteral("saved"), stop.saved},
        {QStringLiteral("reviewed"), stop.reviewed},
        {QStringLiteral("resolved"), stop.resolved},
        {QStringLiteral("earlierResolved"), stop.earlierResolved},
        {QStringLiteral("index"), chosen},
        {QStringLiteral("count"), size},
        {QStringLiteral("label"), label}};
    mode_ = mode;
    fileId_ = stop.id;
    index_ = chosen;
    Q_EMIT changed();
    return true;
}
}
