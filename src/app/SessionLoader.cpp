// Session loading discovers conflicted files and offers recoverable work without silently replacing it.
#include "SessionLoader.h"
#include "Selection.h"
#include "SessionCompletion.h"
#include "SourceIdentity.h"
#include "problems/ProblemText.h"
#include "publication/ResolutionPublisher.h"
#include "session/ConflictCapture.h"
#include "session/SessionCoordinator.h"
#include "session/SessionStore.h"
#include <QDateTime>
#include <QFileInfo>
#include <QLocale>
#include <QUuid>
#include <algorithm>
#include <memory>

namespace branch_truce {
SessionLoader::SessionLoader(Session session, git::GitRunners git, QObject *parent,
                             std::shared_ptr<const CorrespondenceAlgorithm> correspondence)
    : QObject(parent),
      session_(session),
      git_(std::move(git)),
      correspondence_(std::move(correspondence)),
      classifier_(git_, this)
{
    connect(&classifier_, &git::ConflictClassifier::finished, this, &SessionLoader::classified);
    // The fences drop a choice's read when work changes or the session is
    // replaced, so its completion never runs; the choice ends here instead.
    connect(&session_.coordinator, &SessionCoordinator::taskFinished, this, [this](const TaskCompletion &completion) {
        if (choiceTask_ == 0 || completion.stamp.id != choiceTask_
            || completion.disposition == TaskDisposition::Current)
            return;
        const auto refusal = choiceRefusal(choiceSession_, choiceState_);
        refuseChoice(refusal.isEmpty() ? tr("Choose a recovery session after the session finishes loading.") : refusal);
    });
}

QString SessionLoader::stage2() const
{
    if (!repository_.supported) return {};
    return sourceRole(repository_.stage2.label, QString::fromLatin1(repository_.stage2.commitId));
}

QString SessionLoader::operation() const
{
    if (!classified_) return {};
    switch (repository_.operation) {
    case git::RepositoryOperation::Merge: return tr("Merge");
    case git::RepositoryOperation::Rebase: return tr("Rebase");
    case git::RepositoryOperation::Unsupported: return tr("Unsupported operation");
    case git::RepositoryOperation::None: break;
    }
    return {};
}

QString SessionLoader::operationDescription() const
{
    if (!repository_.supported) return operation();
    const auto left = stage2Name().isEmpty() ? sourceIdentity(stage2(), stage2CommitId()) : stage2Name();
    const auto right = stage3Name().isEmpty() ? sourceIdentity(stage3(), stage3CommitId()) : stage3Name();
    if (repository_.operation == git::RepositoryOperation::Merge) return tr("Merging %1 into %2").arg(right, left);
    if (repository_.operation == git::RepositoryOperation::Rebase) {
        const auto branch =
            repository_.headName.startsWith("refs/heads/") ? git::displayPath(repository_.headName.mid(11)) : right;
        const auto onto =
            repository_.ontoName.isEmpty() ? shortId(QString::fromLatin1(repository_.onto)) : repository_.ontoName;
        return tr("Rebasing %1 onto %2").arg(branch, onto);
    }
    return operation();
}

QString SessionLoader::stage3() const
{
    if (!repository_.supported) return {};
    return sourceRole(repository_.stage3.label, QString::fromLatin1(repository_.stage3.commitId));
}

QString SessionLoader::stage2CommitId() const
{
    if (!repository_.supported) return {};
    return QString::fromLatin1(repository_.stage2.commitId);
}

QString SessionLoader::stage3CommitId() const
{
    if (!repository_.supported) return {};
    return QString::fromLatin1(repository_.stage3.commitId);
}

QString SessionLoader::stage2ShortCommitId() const { return shortId(stage2CommitId()); }
QString SessionLoader::stage3ShortCommitId() const { return shortId(stage3CommitId()); }

QString SessionLoader::workflow() const
{
    return tr("Continue the operation and commit in your usual Git workflow. "
              "This session covers only the work opened at launch.");
}

QString SessionLoader::launchSizeText(const QString &fileId) const
{
    const auto found = launchSizes_.constFind(fileId);
    return found == launchSizes_.cend() ? QString{} : QLocale().formattedDataSize(*found);
}

QString SessionLoader::noOperationDetail()
{
    return tr("No merge or rebase is in progress in this repository. "
              "Branch Truce resolves conflicts while a merge or rebase is in progress.");
}

void SessionLoader::setState(StartupState state, QString detail)
{
    state_ = state;
    detail_ = std::move(detail);
    Q_EMIT launchChanged();
}

QVariantList SessionLoader::recoveryChoices() const
{
    QVariantList result;
    for (const auto &choice : recoveryChoices_) {
        QStringList paths;
        for (const auto &path : choice.paths)
            paths.push_back(git::displayPath(QByteArray(path.data(), static_cast<qsizetype>(path.size()))));
        result.push_back(QVariantMap{
            {QStringLiteral("session"), choice.session},
            {QStringLiteral("lastChanged"), QLocale().toString(choice.lastChanged.toLocalTime(), QLocale::ShortFormat)},
            {QStringLiteral("paths"), paths}});
    }
    return result;
}

QString SessionLoader::appEditsRefusal()
{
    return tr("This window already holds app edits, so it cannot switch to a recovery session. "
              "Leave this session, then start Branch Truce with --recovery-session followed by the session to keep.");
}

QString SessionLoader::choiceRefusal(const QString &session, StartupState state)
{
    if (state != StartupState::Ready && state != StartupState::ReadyWithoutMergetool)
        return tr("Choose a recovery session after the session finishes loading.");
    if (std::none_of(recoveryChoices_.cbegin(), recoveryChoices_.cend(),
                     [&](const auto &choice) { return choice.session == session; }))
        return tr("That recovery session is not one of the choices for this session.");
    if (session_.completion.ended() || session_.publisher.busy() || session_.store.hasPendingRecovery())
        return tr("Choose a recovery session after the running resolution or pending recovery input finishes.");
    // The reload replaces this window's files. Edits already recorded here
    // would be left in a store nobody chose, so they refuse the choice.
    if (session_.store.holdsUserWork()) return appEditsRefusal();
    return {};
}

bool SessionLoader::chooseRecoverySession(const QString &session)
{
    const auto refuse = [this](const QString &message) {
        Q_EMIT notice(message);
        return false;
    };
    if (const auto refusal = choiceRefusal(session, state_); !refusal.isEmpty()) return refuse(refusal);
    // These locks are released before the reload. If another window takes the
    // chosen session in between, the restore adopts no other edited session.
    for (const auto &choice : recoveryChoices_) {
        if (!storage::RecoveryLease::acquire(choice.directory))
            return refuse(
                tr("Recovery session %1 is open in another Branch Truce window. Close that window, then choose again.")
                    .arg(choice.session));
    }
    // Reading this window's store waits on the disk, so the worker reads it,
    // fenced by session and revision like the initial load. The window shows
    // the reload opening meanwhile.
    const auto *file = session_.store.size() > 0 ? &session_.store.files().front() : nullptr;
    const auto anchor = file ? file->id() : std::string{};
    const auto directory = session_.store.recoveryDirectory();
    auto own = std::make_shared<RecoveryReplay>();
    const auto task = !file
        ? 0
        : session_.coordinator.submit(
              anchor, file->current().id,
              [directory, own, correspondence = correspondence_](const TaskInput &, const TaskControl &) {
                  *own = replayRecovery(storage::RecoveryStore(directory), correspondence);
                  return TaskResult{};
              },
              [this, session, directory, own](const TaskCompletion &completion) {
                  if (completion.result.status != TaskStatus::Succeeded) {
                      own->succeeded = false;
                      own->problem = completion.result.problem;
                  }
                  ownStoreRead(session, directory, *own);
              });
    if (task == 0) return refuse(tr("Choose a recovery session after the session finishes loading."));
    choiceTask_ = task;
    choiceSession_ = session;
    choiceState_ = state_;
    choiceDetail_ = detail_;
    setState(StartupState::Opening, tr("Inspecting the selected local repository…"));
    return true;
}

void SessionLoader::ownStoreRead(const QString &session, const QString &directory, const RecoveryReplay &own)
{
    choiceTask_ = 0;
    // Work recorded while the store was read refuses the choice too.
    if (const auto refusal = choiceRefusal(session, choiceState_); !refusal.isEmpty()) return refuseChoice(refusal);
    if (session_.store.recoveryDirectory() != directory)
        return refuseChoice(tr("Choose a recovery session after the session finishes loading."));
    if (!own.succeeded)
        return refuseChoice(tr("This window's recovery store could not be read, so it cannot switch sessions: %1")
                                .arg(problem_text::text(own.problem)));
    // A record this window never acknowledged, from a commit that reported
    // failure after the record reached the disk, is work as well.
    if (own.userWork) return refuseChoice(appEditsRefusal());
    if (!session_.store.startNewRecoveryStore())
        return refuseChoice(tr("Choose a recovery session after the pending recovery input finishes."));
    Q_EMIT notice({});
    state_ = StartupState::Idle;
    openRepository(repository_.worktreeRoot, session_.selection.fileId(), session);
}

void SessionLoader::refuseChoice(const QString &message)
{
    choiceTask_ = 0;
    setState(choiceState_, choiceDetail_);
    Q_EMIT notice(message);
}

bool SessionLoader::openRepository(const QString &directory, const QString &preferredFileId,
                                   const QString &recoverySession)
{
    if (state_ != StartupState::Idle) return false;
    // A reload passes repository_.worktreeRoot, which the reset below clears.
    const auto launchDirectory = directory;
    session_.selection.prefer(preferredFileId);
    recoverySession_ = recoverySession;
    recoveryChoices_.clear();
    // A reload must not show or key the session by what an earlier open classified.
    repository_ = {};
    classified_ = false;
    launchSizes_.clear();
    setState(StartupState::Opening, tr("Inspecting the selected local repository…"));
    git::ConflictClassificationRequest request;
    request.directory = launchDirectory;
    if (!classifier_.start(request)) {
        setState(StartupState::Error, tr("Could not start repository inspection."));
        return false;
    }
    return true;
}

void SessionLoader::classified(const git::ConflictClassificationResult &result)
{
    repository_ = result.repository;
    classified_ = result.outcome == git::Outcome::Success;

    // Even an ended/unsupported operation can have expected recovery work.
    // Inspect its inventory so identity rejection is explicit, without applying it.
    if (result.outcome == git::Outcome::Success && !repository_.supported && !repository_.worktreeRoot.isEmpty()) {
        loadWorkingFiles(result.conflicts);
        return;
    }
    if (result.outcome != git::Outcome::Success || !repository_.supported) {
        std::vector<FileSession> files;
        for (const auto &file : result.conflicts) {
            files.emplace_back(
                file.path.toHex().toStdString(), file.path.toStdString(), Draft{},
                FileLaunch{.support = supportFor(file),
                           .supportDetail =
                               (file.reason.isEmpty() ? result.problem : file.reason).encoded().toStdString()});
        }
        if (!files.empty())
            session_.coordinator.replaceSession(repository_.worktreeRoot, QStringLiteral("unsupported"),
                                                std::move(files));
        if (result.outcome != git::Outcome::Success) {
            setState(StartupState::Error,
                     tr("Could not open the selected work: %1").arg(problem_text::text(result.problem)));
        } else if (repository_.operation == git::RepositoryOperation::None) {
            setState(StartupState::NoSession, noOperationDetail());
        } else {
            setState(StartupState::Unsupported, problem_text::text(repository_.problem));
        }
        return;
    }
    loadWorkingFiles(result.conflicts);
}

void SessionLoader::loadWorkingFiles(QList<git::Conflict> conflicts)
{
    launchSizes_.clear();
    for (const auto &file : conflicts)
        if (file.launchSize >= 0) launchSizes_.insert(QString::fromLatin1(file.path.toHex()), file.launchSize);
    std::vector<FileSession> pending;
    for (const auto &file : conflicts) {
        pending.emplace_back(file.path.toHex().toStdString(), file.path.toStdString(), Draft{},
                             FileLaunch{.support = Support::Unreadable,
                                        .supportDetail = Problem(ProblemCode::LoadOpening).encoded().toStdString()});
    }
    if (pending.empty())
        pending.emplace_back(
            "recovery-inventory", "recovery-inventory", Draft{},
            FileLaunch{.support = Support::Unreadable,
                       .supportDetail = Problem(ProblemCode::LoadInspectingRecovery).encoded().toStdString()});
    // The fence key names the operation by its enum, never by display text.
    const auto identity = QString::number(static_cast<int>(repository_.operation)) + QLatin1Char(':')
        + QString::fromLatin1(repository_.stage2.commitId) + QLatin1Char(':')
        + QString::fromLatin1(repository_.stage3.commitId);
    const auto root = repository_.worktreeRoot;
    if (!session_.coordinator.replaceSession(root, identity, std::move(pending))) {
        setState(StartupState::Error, tr("Could not bind the session to the discovered worktree."));
        return;
    }
    auto loaded = std::make_shared<std::vector<FileSession>>();
    auto recovery = std::make_shared<RecoveryRestore>();
    auto baselines = std::make_shared<ResolutionPublisher::Baselines>();
    const auto anchor =
        conflicts.empty() ? std::string("recovery-inventory") : conflicts.front().path.toHex().toStdString();
    const auto recoveryDirectory = session_.store.recoveryDirectory();
    const auto recoveryInventory = QFileInfo(recoveryDirectory).absolutePath();
    const auto sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    const auto task = session_.coordinator.submit(
        anchor, 1,
        [files = std::move(conflicts), repository = repository_, sessionId, loaded, recovery, recoveryDirectory,
         recoveryInventory, git = git_, correspondence = correspondence_, chosen = recoverySession_, baselines,
         destinations = session_.publisher.destinations()](const TaskInput &, const TaskControl &control) {
            for (const auto &file : files) {
                if (control.cancellationRequested()) return TaskResult{TaskStatus::Cancelled, {}, {}, {}, {}};
                Draft draft;
                auto support = supportFor(file);
                auto detail = file.reason;
                auto snapshots = captureFile(file, repository, sessionId);
                if (snapshots) {
                    support = Support::Supported;
                    // FileSession counts the markers in this text.
                    draft.text = file.result->utf8.toStdString();
                } else if (detail.isEmpty()) {
                    support = Support::Unreadable;
                    detail = ProblemCode::LoadIncomplete;
                }
                loaded->emplace_back(file.path.toHex().toStdString(), file.path.toStdString(), std::move(draft),
                                     FileLaunch{.support = support,
                                                .supportDetail = detail.encoded().toStdString(),
                                                .snapshots = std::move(snapshots),
                                                .initializeChoices = true,
                                                .algorithm = correspondence});
            }
            *recovery = restoreRecovery(recoveryInventory, recoveryDirectory, repository, *loaded, git, chosen,
                                        {correspondence, QDateTime::currentDateTimeUtc()});
            // Reading every destination waits on the disk, so it happens here too.
            if (repository.supported)
                *baselines = ResolutionPublisher::readBaselines(
                    repository.worktreeRoot, recovery->found ? recovery->files : *loaded, destinations);
            return TaskResult{};
        },
        [this, loaded, recovery, baselines](const TaskCompletion &completion) {
            QStringList lines;
            for (const auto &diagnostic : recovery->diagnostics) lines.push_back(problem_text::text(diagnostic));
            const auto diagnostics = lines.join(QLatin1Char('\n'));
            const bool recovered = recovery->found;
            auto choices = recovery->choices;
            session_.mergetool.clearCopiedLaunches();
            auto copiedLaunches = recovery->copiedLaunches;
            const bool replaced = completion.result.status == TaskStatus::Succeeded
                && session_.coordinator.completeRecoveryLoad(completion.stamp, std::move(*loaded),
                                                             std::move(*recovery));
            session_.mergetool.reassign();
            if (!replaced) {
                setState(StartupState::Error, tr("Could not load the current conflict files."));
                return;
            }
            if (!repository_.supported && repository_.operation != git::RepositoryOperation::None) {
                auto detail = problem_text::text(repository_.problem);
                if (!diagnostics.isEmpty()) detail += QLatin1Char('\n') + diagnostics;
                setState(StartupState::Unsupported, detail);
                return;
            }
            if (session_.store.size() == 0) {
                if (!diagnostics.isEmpty())
                    setState(StartupState::Error, diagnostics);
                else if (repository_.operation == git::RepositoryOperation::None)
                    setState(StartupState::NoSession, noOperationDetail());
                else
                    setState(StartupState::NoSession,
                             tr("This operation has no current unmerged files. Return to your Git workflow."));
                return;
            }
            auto listen = MergetoolSession::Listen::Listening;
            QString listenError;
            if (repository_.supported) {
                session_.publisher.adoptBaselines(repository_, std::move(*baselines));
                session_.mergetool.adoptCopiedLaunches(std::move(copiedLaunches));
                // A launcher can already wait here after an in-window recovery choice.
                session_.mergetool.acceptCopiedLaunches();
                listen = session_.mergetool.listen(repository_, &listenError);
            }
            auto detail = recovered
                ? tr("Recovered app drafts and decisions. Manual Save, review and Git resolution keep their recorded "
                     "status.")
                : tr("Current conflict files are open. Files marked outside app remain outstanding.");
            if (listen == MergetoolSession::Listen::Elsewhere)
                detail += QLatin1Char('\n')
                    + tr("Another Branch Truce window receives git mergetool launches for this operation.");
            if (listen == MergetoolSession::Listen::Unavailable)
                detail = tr("Mergetool attach is unavailable: %1").arg(listenError) + QLatin1Char('\n') + detail;
            if (!diagnostics.isEmpty()) detail += QLatin1Char('\n') + diagnostics;
            recoveryChoices_ = std::move(choices);
            setState(listen == MergetoolSession::Listen::Unavailable ? StartupState::ReadyWithoutMergetool
                                                                     : StartupState::Ready,
                     detail);
        });
    if (task == 0) setState(StartupState::Error, tr("Could not schedule conflict loading."));
}
}
