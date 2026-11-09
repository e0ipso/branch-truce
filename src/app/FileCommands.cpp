// File commands change the selected file's result, Save it, discard it or Mark it resolved.
#include "FileCommands.h"
#include "CommandFeedback.h"
#include "NavigationTargets.h"
#include "QtText.h"
#include "ResultEditorAdapter.h"
#include "Selection.h"
#include "mergetool/MergetoolSession.h"
#include "publication/ResolutionPublisher.h"
#include "session/SessionStore.h"

namespace branch_truce {
using qt_text::utf8;
namespace {

bool scalarBoundary(const QString &value, int offset)
{
    return offset >= 0 && offset <= value.size()
        && !(offset > 0 && offset < value.size() && value.at(offset).isLowSurrogate()
             && value.at(offset - 1).isHighSurrogate());
}
}

FileCommands::FileCommands(SessionStore &store, const Selection &selection, ResultEditorAdapter &editor,
                           ResolutionPublisher &publisher, const MergetoolSession &mergetool, CommandFeedback &feedback,
                           QObject *parent)
    : QObject(parent),
      store_(store),
      selection_(selection),
      editor_(editor),
      publisher_(publisher),
      mergetool_(mergetool),
      feedback_(feedback)
{
    connect(&editor_, &ResultEditorAdapter::edited, this, &FileCommands::editResult);
    connect(&editor_, &ResultEditorAdapter::editRejected, this,
            [this] { feedback_.report(Transition::InvalidAnchor); });
    // A publication step can end the resolution or ask for confirmation.
    connect(&publisher_, &ResolutionPublisher::changed, this, &FileCommands::resolutionChanged);
    // Replacing the files drops their pending input, so a discard waiting on
    // it never completes and must not report a completion.
    connect(&store_, &SessionStore::aboutToReplace, this, [this] {
        discardPendingFileId_.reset();
        stagedDiscardPendingFileId_.reset();
    });
    connect(&store_, &SessionStore::replaced, this, &FileCommands::resolutionChanged);
    connect(&store_, &SessionStore::fileChanged, this,
            [this](int row) { pendingDiscardSettled(store_.files()[static_cast<std::size_t>(row)].id()); });
}

bool FileCommands::resolutionConfirmation() const { return publisher_.confirmationPending(); }
QString FileCommands::resolutionFileId() const { return publisher_.confirmationFileId(); }
QString FileCommands::resolutionRevision() const { return publisher_.confirmationRevision(); }
bool FileCommands::resolutionBusy() const { return publisher_.busy(); }

std::optional<CommandTarget> FileCommands::admit(const QString &fileId, const QString &revision, CommandCheck check)
{
    const auto checked = checkCommand(store_, selection_.fileId(), editor_.composing(), fileId, revision, check);
    if (const auto *refused = std::get_if<Transition>(&checked)) {
        feedback_.report(*refused);
        return std::nullopt;
    }
    return std::get<CommandTarget>(checked);
}

bool FileCommands::editResult(const QString &fileId, const QString &revision, int begin, int end,
                              const QString &replacement)
{
    const auto target = admit(fileId, revision, CommandCheck::Edit);
    if (!target) return false;
    const auto old = utf8(target->file->current().draft.text);
    if (begin > end || !scalarBoundary(old, begin) || !scalarBoundary(old, end)
        || QString::fromUtf8(replacement.toUtf8()) != replacement)
        return feedback_.report(Transition::InvalidAnchor);
    return feedback_.report(store_.editResult(fileId.toStdString(), target->revision,
                                              {{static_cast<std::size_t>(old.left(begin).toUtf8().size()),
                                                static_cast<std::size_t>(old.left(end).toUtf8().size())},
                                               replacement.toUtf8().toStdString()}));
}

bool FileCommands::undo(const QString &fileId, const QString &revision)
{
    const auto target = admit(fileId, revision, CommandCheck::Current);
    return target && feedback_.report(store_.undo(fileId.toStdString(), target->revision));
}

bool FileCommands::redo(const QString &fileId, const QString &revision)
{
    const auto target = admit(fileId, revision, CommandCheck::Current);
    return target && feedback_.report(store_.redo(fileId.toStdString(), target->revision));
}

bool FileCommands::saveDraft(const QString &fileId, const QString &revision)
{
    const auto target = admit(fileId, revision, CommandCheck::CurrentSupported);
    if (!target) return false;
    const auto &[file, expected] = *target;
    const auto saved = file->checkpoint() && file->checkpoint()->id == expected
        && file->checkpoint()->result == file->current().result
        && file->checkpoint()->decisions == file->current().decisions && file->checkpointOutcome()
        && file->checkpointOutcome()->outcome == Outcome::Succeeded && file->checkpointOutcome()->revision == expected;
    if (saved) return feedback_.report(Transition::NoChange);
    // The recovery commit inside this call is the app checkpoint. It does not
    // write the worktree or stage a path; publication stays on Mark resolved.
    const auto result = store_.recordCheckpoint(fileId.toStdString(), expected, Outcome::Succeeded);
    if (result == Transition::Blocked && store_.pendingAction(fileId.toStdString())) {
        feedback_.show(tr("Draft save failed. The edited result is still available."));
        return false;
    }
    if (result != Transition::Applied) return feedback_.report(result);
    feedback_.show(tr("Draft saved in Branch Truce; repository file unchanged."));
    return true;
}

bool FileCommands::markResolved(const QString &fileId, const QString &revision, bool confirmUndecided)
{
    const auto target = admit(fileId, revision, CommandCheck::CurrentSupported);
    if (!target) return false;
    const auto &[session, expected] = *target;
    if (mergetool_.blocks(QByteArray::fromStdString(session->path()))) {
        feedback_.show(tr("Waiting for Git to launch this path. It is not resolved."));
        return false;
    }
    const auto started = publisher_.start(fileId.toStdString(), expected, confirmUndecided);
    if (started.markers && started.markers->first())
        feedback_.showMarkers(started.detail, fileId.toStdString(), expected, started.markers->count(),
                              started.markers->first()->line + 1);
    else
        feedback_.show(started.detail);
    // Starting drops the parent staging offer and can ask for confirmation.
    Q_EMIT resolutionChanged();
    return started.transition == Transition::Applied || started.transition == Transition::NoChange;
}

void FileCommands::cancelResolution() { publisher_.cancelConfirmation(); }

bool FileCommands::showFirstMarker()
{
    const auto *file = store_.file(selection_.id());
    // A pending draft in the editor has other offsets than the current revision.
    if (!file || feedback_.markerBlocker().isEmpty() || !editor_.attached() || store_.pendingDraft(file->id()))
        return false;
    const auto first = file->markers().first();
    const int cursor = first ? utf16Offset(file->current().draft.text, first->bytes.begin) : -1;
    if (cursor < 0) return false;
    editor_.setCursorPosition(cursor);
    return true;
}

QString FileCommands::discardedMessage(const FileSession &file) const
{
    return !file.facts().unsaved
        ? tr("Discarded unsaved changes. Draft saved in Branch Truce; repository file unchanged.")
        : tr("Discarded unsaved changes and restored the initial result. It is still unsaved, and the repository file "
             "is unchanged.");
}

QString FileCommands::stagedDiscardMessage()
{
    return tr("Discarded the draft. The result staged in Git is current again; the repository file is unchanged.");
}

void FileCommands::pendingDiscardSettled(const std::string &fileId)
{
    if (discardPendingFileId_ == fileId && !store_.pendingDraft(fileId)) {
        discardPendingFileId_.reset();
        if (const auto *discarded = store_.file(fileId)) feedback_.show(discardedMessage(*discarded));
    }
    if (stagedDiscardPendingFileId_ == fileId && !store_.pendingDraft(fileId)) {
        stagedDiscardPendingFileId_.reset();
        if (store_.file(fileId)) feedback_.show(stagedDiscardMessage());
    }
}

bool FileCommands::discardUnsaved(const QString &fileId, const QString &revision)
{
    const auto target = admit(fileId, revision, CommandCheck::CurrentSupported);
    if (!target) return false;
    // Applied means the restored text and decisions are already in the journal.
    const auto id = fileId.toStdString();
    const auto result = store_.discardToCheckpoint(id, target->revision);
    if (result == Transition::Blocked && store_.pendingAction(id)) {
        // The editor shows the pending restored result. The acknowledged edit
        // stays in place until this same discard commits.
        discardPendingFileId_ = id;
        feedback_.show(tr("Discard was not completed. The restored result is shown until retry finishes it."));
        return false;
    }
    discardPendingFileId_.reset();
    if (result != Transition::Applied) return feedback_.report(result);
    feedback_.show(discardedMessage(*store_.file(id)));
    return true;
}

bool FileCommands::discardToStaged(const QString &fileId, const QString &revision)
{
    const auto target = admit(fileId, revision, CommandCheck::CurrentSupported);
    if (!target) return false;
    // Applied means the staged revision is current again and the journal holds it.
    const auto id = fileId.toStdString();
    const auto result = store_.discardToStaged(id, target->revision);
    if (result == Transition::Blocked && store_.pendingAction(id)) {
        // The acknowledged draft stays current until retry commits this same discard.
        stagedDiscardPendingFileId_ = id;
        feedback_.show(tr("Discard was not completed. The draft is kept until retry finishes the discard."));
        return false;
    }
    stagedDiscardPendingFileId_.reset();
    if (result == Transition::Blocked) {
        feedback_.show(
            tr("Discard was not completed. No verified result staged in Git to return to, so the draft is kept."));
        return false;
    }
    if (result != Transition::Applied) return feedback_.report(result);
    feedback_.show(stagedDiscardMessage());
    return true;
}

bool FileCommands::loadLargeFile(const QString &fileId)
{
    const auto *file = store_.file(fileId.toStdString());
    const auto target = admit(fileId, file ? QString::number(file->current().id) : QString{}, CommandCheck::Current);
    return target && feedback_.report(store_.loadLargeFile(fileId.toStdString()));
}

bool FileCommands::retryPendingRecovery(const QString &fileId)
{
    if (editor_.composing()) return feedback_.report(Transition::Blocked);
    return store_.retryRecovery(fileId.toStdString()) == Transition::Applied;
}
}
