#include "ApplicationController.h"
#include "CommandGuard.h"
#include "NavigationTargets.h"
#include "RecoveryRestore.h"
#include "TextPositions.h"
#include "core/Correspondence.h"
#include "git/GitAdapter.h"
#include <QDir>
#include "core/BuildInfo.h"
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

// The undecorated last path component, for syntax definition lookup.
QString fileName(const std::string &path)
{
    return text(path.substr(path.rfind('/') + 1));
}

bool scalarBoundary(const QString &value, int offset)
{
    return offset >= 0 && offset <= value.size()
        && !(offset > 0 && offset < value.size() && value.at(offset).isLowSurrogate()
             && value.at(offset - 1).isHighSurrogate());
}


}

ApplicationController::ApplicationController(QString gitExecutable, QObject *parent)
    : QObject(parent), files_(this), selectedFile_(files_, this),
      coordinator_(files_, gitExecutable, this),
      publisher_(files_, coordinator_, gitExecutable, this),
      mergetool_(publisher_, files_, coordinator_, [this] { return loader_.repository(); }, gitExecutable, this),
      editor_(files_, [this] { return selectedFileId_; },
          [this](const FileSession *file, const QString &loaded) { return firstUndecidedPlacement(file, loaded); }, this),
      loader_(files_, coordinator_, gitExecutable, SessionLoader::Owners{
          [](const std::string &path) {
              return git::displayPath(QByteArray(path.data(), static_cast<qsizetype>(path.size())));
          },
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
          [this](const QString &fileId) { return files_.displayPath(fileId.toStdString()); }}, this),
      choices_(files_, editor_, ChoicePreviewFlow::Owners{
          [this] { return selectedFileId_; },
          [this](const QString &fileId) { return files_.displayPath(fileId.toStdString()); },
          [this](Transition result) { return commandResult(result); },
          [this](const QString &message) { showMessage(message); }}, this)
{
    connect(&editor_, &ResultEditorAdapter::edited, this, &ApplicationController::editResult);
    connect(&editor_, &ResultEditorAdapter::editActionRequested, this, &ApplicationController::resultEditActionRequested);
    connect(&editor_, &ResultEditorAdapter::editRejected, this, [this] { commandResult(Transition::InvalidAnchor); });
    connect(&editor_, &ResultEditorAdapter::notice, this, &ApplicationController::showMessage);
    connect(&editor_, &ResultEditorAdapter::loadRefused, this, [this](const QString &message) {
        if (commandMessage_ != message) showMessage(message);
    });
    connect(&editor_, &ResultEditorAdapter::placed, this, &ApplicationController::editorPlaced);
    connect(&editor_, &ResultEditorAdapter::stateChanged, this, &ApplicationController::resultEditorChanged);
    connect(&navigator_, &FileNavigator::changed, this, &ApplicationController::navigationChanged);
    connect(&choices_, &ChoicePreviewFlow::changed, this, [this](ChoicePreviewFlow::Scope scope) {
        switch (scope) {
        case ChoicePreviewFlow::Scope::Choice: Q_EMIT commandPreviewChanged(); break;
        case ChoicePreviewFlow::Scope::NonConflicting: Q_EMIT nonConflictingPreviewChanged(); break;
        case ChoicePreviewFlow::Scope::WholeFile: Q_EMIT wholeFilePreviewChanged(); break;
        }
    });
    connect(&loader_, &SessionLoader::notice, this, &ApplicationController::showMessage);
    // A publication step can end the resolution and set or drop the parent
    // staging offer, which the mergetool state shows.
    connect(&publisher_, &ResolutionPublisher::changed, this, [this](const QString &message) {
        commandMessage_ = message;
        mergetool_.onPublisherChanged();
        Q_EMIT messageChanged();
        Q_EMIT resolutionChanged();
        Q_EMIT mergetoolChanged();
    });
    // A new session drops the publisher's attempt, confirmation and offer.
    connect(&coordinator_, &SessionCoordinator::sessionChanged, this, &ApplicationController::resolutionChanged);
    connect(&mergetool_, &MergetoolSession::notice, this, &ApplicationController::showMessage);
    connect(&mergetool_, &MergetoolSession::changed, this, &ApplicationController::mergetoolChanged);
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
            showMessage(discardedMessage(files_.file(selectedFileId_.toStdString())));
        }
        if (stagedDiscardPending_ && !files_.hasPendingRecovery()) {
            stagedDiscardPending_ = false;
            showMessage(stagedDiscardMessage());
        }
        Q_EMIT inventoryChanged();
        Q_EMIT selectedFileChanged();
    });
    connect(this, &ApplicationController::selectedFileChanged, this, [this] {
        selectedFile_.sync(selectedFileId_);
        const auto *file = files_.file(selectedFileId_.toStdString());
        editor_.setSyntaxFileName(file ? fileName(file->path()) : QString{});
    });
    connect(this, &ApplicationController::selectedFileChanged, &editor_, &ResultEditorAdapter::sync);
    // The marker blocker, the mergetool waiting state and the parent staging
    // path follow the selection. summaryChanged emits selectedFileChanged, so
    // a new revision of the selected file reaches the blocker too.
    connect(this, &ApplicationController::selectedFileChanged, this, &ApplicationController::messageChanged);
    connect(this, &ApplicationController::selectedFileChanged, this, &ApplicationController::mergetoolChanged);
    // The handoff reads the files, the running resolution and the launches Git still waits on.
    connect(&files_, &SessionModel::summaryChanged, this, &ApplicationController::handoffChanged);
    connect(&publisher_, &ResolutionPublisher::changed, this, &ApplicationController::handoffChanged);
    connect(&mergetool_, &MergetoolSession::changed, this, &ApplicationController::handoffChanged);
}
QString ApplicationController::version() const
{
    return QString::fromUtf8(branch_truce::version().data(),
                             static_cast<qsizetype>(branch_truce::version().size()));
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
    return files_.rowOf(selectedFileId_.toStdString());
}

QVariantMap ApplicationController::commandPreview() const
{
    return choices_.view(ChoicePreviewFlow::Scope::Choice);
}

void ApplicationController::showMessage(const QString &message)
{
    commandMessage_ = message;
    Q_EMIT messageChanged();
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
    Q_EMIT messageChanged();
    return result == Transition::Applied || result == Transition::NoChange;
}

bool ApplicationController::previewChoice(const QString &fileId, const QString &revision,
                                         const QString &target, const QString &action)
{
    return choices_.preview(ChoicePreviewFlow::Scope::Choice, fileId, revision, action, target);
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
    return choices_.preview(ChoicePreviewFlow::Scope::NonConflicting, fileId, revision, scope);
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
    return choices_.preview(ChoicePreviewFlow::Scope::WholeFile, fileId, revision, side);
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
    const auto expected = parseRevisionId(revision);
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
    const auto expected = parseRevisionId(revision);
    if (!expected || fileId != selectedFileId_) return commandResult(Transition::StaleRevision);
    if (editor_.composing()) return commandResult(Transition::Blocked);
    return commandResult(files_.undo(fileId.toStdString(), *expected));
}

bool ApplicationController::redo(const QString &fileId, const QString &revision)
{
    const auto expected = parseRevisionId(revision);
    if (!expected || fileId != selectedFileId_) return commandResult(Transition::StaleRevision);
    if (editor_.composing()) return commandResult(Transition::Blocked);
    return commandResult(files_.redo(fileId.toStdString(), *expected));
}

std::optional<CommandTarget> ApplicationController::admitCommand(const QString &fileId, const QString &revision)
{
    const auto checked = checkCommand(files_, selectedFileId_, editor_.composing(), fileId, revision,
                                      CommandCheck::CurrentSupported);
    if (const auto *refused = std::get_if<Transition>(&checked)) {
        commandResult(*refused);
        return std::nullopt;
    }
    return std::get<CommandTarget>(checked);
}

bool ApplicationController::saveDraft(const QString &fileId, const QString &revision)
{
    const auto target = admitCommand(fileId, revision);
    if (!target) return false;
    const auto &[file, expected] = *target;
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
        showMessage(tr("Draft save failed. The edited result is still available."));
        return false;
    }
    if (result != Transition::Applied) return commandResult(result);
    showMessage(tr("Draft saved in Branch Truce; repository file unchanged."));
    return true;
}

QString ApplicationController::parentStagingPath() const
{
    if (!publisher_.parentStagingOffer(selectedFileId_.toStdString())) return {};
    return files_.displayPath(selectedFileId_.toStdString());
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
    const auto target = admitCommand(fileId, revision);
    if (!target) return false;
    const auto &[session, expected] = *target;
    if (mergetool_.blocks(QByteArray::fromStdString(session->path()))) {
        showMessage(tr("Waiting for Git to launch this path. It is not resolved."));
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
    Q_EMIT messageChanged();
    // Starting drops the parent staging offer and can ask for confirmation.
    Q_EMIT resolutionChanged();
    Q_EMIT mergetoolChanged();
    Q_EMIT handoffChanged();
    return started.transition == Transition::Applied || started.transition == Transition::NoChange;
}

QVariantMap ApplicationController::markerBlocker() const
{
    const auto *file = files_.file(selectedFileId_.toStdString());
    if (markerBlocker_.isEmpty() || !file || markerBlocker_.value(QStringLiteral("fileId")).toString() != selectedFileId_
        || markerBlocker_.value(QStringLiteral("message")).toString() != commandMessage_
        || parseRevisionId(markerBlocker_.value(QStringLiteral("revision")).toString()) != file->current().id)
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
    const auto expected = parseRevisionId(revision);
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
        showMessage(tr("Work remains outstanding, so this session cannot finish cleanly. "
                       "Open an outstanding file or leave deliberately."));
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
        if (!blocker.isEmpty()) showMessage(blocker);
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
    const auto target = admitCommand(fileId, revision);
    if (!target) return false;
    const auto expected = target->revision;
    // Applied means the restored text and decisions are already in the journal.
    const auto result = files_.discardToCheckpoint(fileId.toStdString(), expected);
    if (result == Transition::Blocked && files_.hasPendingRecovery()) {
        // The editor shows the pending restored result. The acknowledged edit
        // stays in place until this same discard commits.
        discardPending_ = true;
        showMessage(tr("Discard was not completed. The restored result is shown until retry finishes it."));
        return false;
    }
    discardPending_ = false;
    if (result != Transition::Applied) return commandResult(result);
    showMessage(discardedMessage(files_.file(fileId.toStdString())));
    return true;
}

QString ApplicationController::stagedDiscardMessage()
{
    return tr("Discarded the draft. The result staged in Git is current again; the repository file is unchanged.");
}

bool ApplicationController::discardToStaged(const QString &fileId, const QString &revision)
{
    const auto target = admitCommand(fileId, revision);
    if (!target) return false;
    const auto expected = target->revision;
    // Applied means the staged revision is current again and the journal holds it.
    const auto result = files_.discardToStaged(fileId.toStdString(), expected);
    if (result == Transition::Blocked && files_.hasPendingRecovery()) {
        // The acknowledged draft stays current until retry commits this same discard.
        stagedDiscardPending_ = true;
        showMessage(tr("Discard was not completed. The draft is kept until retry finishes the discard."));
        return false;
    }
    stagedDiscardPending_ = false;
    if (result == Transition::Blocked) {
        showMessage(tr("Discard was not completed. No verified result staged in Git to return to, so the draft is kept."));
        return false;
    }
    if (result != Transition::Applied) return commandResult(result);
    showMessage(stagedDiscardMessage());
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
    return files_.supportCount(Support::OutsideApp);
}

int ApplicationController::loadErrorFiles() const
{
    return files_.supportCount(Support::Unreadable);
}
}
