#include "SessionLoader.h"
#include "SourceIdentity.h"
#include "session/ConflictCapture.h"
#include "session/SessionCoordinator.h"
#include "SessionModel.h"
#include <QFileInfo>
#include <QLockFile>
#include <QUuid>
#include <algorithm>
#include <memory>

namespace branch_truce {
SessionLoader::SessionLoader(SessionModel &files, SessionCoordinator &coordinator, QString gitExecutable,
                             Owners owners, QObject *parent)
    : QObject(parent), files_(files), coordinator_(coordinator), owners_(std::move(owners)),
      gitExecutable_(std::move(gitExecutable)), classifier_(gitExecutable_, this)
{
    connect(&classifier_, &git::ConflictClassifier::finished, this, &SessionLoader::classified);
    // The fences drop a choice's read when work changes or the session is
    // replaced, so its completion never runs; the choice ends here instead.
    connect(&coordinator_, &SessionCoordinator::taskFinished, this, [this](const TaskCompletion &completion) {
        if (choiceTask_ == 0 || completion.stamp.id != choiceTask_ || completion.disposition == TaskDisposition::Current)
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

QString SessionLoader::noOperationDetail()
{
    return tr("No merge or rebase is in progress in this repository. "
              "Branch Truce resolves conflicts while a merge or rebase is in progress.");
}

void SessionLoader::setState(QString state, QString detail)
{
    state_ = std::move(state);
    detail_ = std::move(detail);
    Q_EMIT launchChanged();
}

QVariantList SessionLoader::recoveryChoices() const
{
    QVariantList result;
    for (const auto &choice : recoveryChoices_) {
        QStringList paths;
        for (const auto &path : choice.paths) paths.push_back(owners_.displayPath(path));
        result.push_back(QVariantMap{{QStringLiteral("session"), choice.session},
            {QStringLiteral("lastChanged"), choice.lastChanged}, {QStringLiteral("paths"), paths}});
    }
    return result;
}

QString SessionLoader::appEditsRefusal()
{
    return tr("This window already holds app edits, so it cannot switch to a recovery session. "
        "Leave this session, then start Branch Truce with --recovery-session followed by the session to keep.");
}

QString SessionLoader::choiceRefusal(const QString &session, const QString &state)
{
    if (state != QLatin1String("ready") && state != QLatin1String("ready-without-mergetool"))
        return tr("Choose a recovery session after the session finishes loading.");
    if (std::none_of(recoveryChoices_.cbegin(), recoveryChoices_.cend(),
            [&](const auto &choice) { return choice.session == session; }))
        return tr("That recovery session is not one of the choices for this session.");
    if (owners_.sessionBusy() || files_.hasPendingRecovery())
        return tr("Choose a recovery session after the running resolution or pending recovery input finishes.");
    // The reload replaces this window's files. Edits already recorded here
    // would be left in a store nobody chose, so they refuse the choice.
    if (files_.recoveryJournal().holdsUserWork()) return appEditsRefusal();
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
        QLockFile lease(choice.directory + QStringLiteral("/session.lock"));
        if (!lease.tryLock(0))
            return refuse(tr("Recovery session %1 is open in another Branch Truce window. Close that window, then choose again.")
                .arg(choice.session));
    }
    // Reading this window's store waits on the disk, so the worker reads it,
    // fenced by session and revision like the initial load. The window shows
    // the reload opening meanwhile.
    const auto anchor = files_.data(files_.index(0), SessionModel::FileIdRole).toString().toStdString();
    const auto *file = files_.file(anchor);
    const auto directory = files_.recoveryDirectory();
    auto own = std::make_shared<RecoveryReplay>();
    const auto task = !file ? 0 : coordinator_.submit(anchor, file->current().id,
        [directory, own](const TaskInput &, const TaskControl &) {
            *own = replayRecovery(storage::RecoveryStore(directory));
            return TaskResult{};
        }, [this, session, directory, own](const TaskCompletion &completion) {
            if (completion.result.status != TaskStatus::Succeeded) {
                own->succeeded = false;
                own->error = completion.result.diagnostic;
            }
            ownStoreRead(session, directory, *own);
        });
    if (task == 0) return refuse(tr("Choose a recovery session after the session finishes loading."));
    choiceTask_ = task;
    choiceSession_ = session;
    choiceState_ = state_;
    choiceDetail_ = detail_;
    setState(QStringLiteral("opening"), tr("Inspecting the selected local repository…"));
    return true;
}

void SessionLoader::ownStoreRead(const QString &session, const QString &directory, const RecoveryReplay &own)
{
    choiceTask_ = 0;
    // Work recorded while the store was read refuses the choice too.
    if (const auto refusal = choiceRefusal(session, choiceState_); !refusal.isEmpty()) return refuseChoice(refusal);
    if (files_.recoveryDirectory() != directory)
        return refuseChoice(tr("Choose a recovery session after the session finishes loading."));
    if (!own.succeeded)
        return refuseChoice(tr("This window's recovery store could not be read, so it cannot switch sessions: %1").arg(own.error));
    // A record this window never acknowledged, from a commit that reported
    // failure after the record reached the disk, is work as well.
    if (own.userWork) return refuseChoice(appEditsRefusal());
    if (!files_.startNewRecoveryStore())
        return refuseChoice(tr("Choose a recovery session after the pending recovery input finishes."));
    Q_EMIT notice({});
    state_ = QStringLiteral("idle");
    openRepository(repository_.worktreeRoot, owners_.selectedFileId(), session);
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
    if (state_ != QLatin1String("idle")) return false;
    preferredFileId_ = preferredFileId;
    recoverySession_ = recoverySession;
    recoveryChoices_.clear();
    setState(QStringLiteral("opening"), tr("Inspecting the selected local repository…"));
    git::ConflictClassificationRequest request;
    request.directory = directory;
    if (!classifier_.start(request)) {
        setState(QStringLiteral("error"), tr("Could not start repository inspection."));
        return false;
    }
    return true;
}

void SessionLoader::classified(const git::ConflictClassificationResult &result)
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
            setState(QStringLiteral("no-session"), noOperationDetail());
        } else {
            setState(QStringLiteral("unsupported"), repository_.diagnostic);
        }
        return;
    }
    loadWorkingFiles(result.conflicts);
}

void SessionLoader::loadWorkingFiles(QList<git::Conflict> conflicts)
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
    const auto sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    const auto task = coordinator_.submit(anchor, 1,
        [files = std::move(conflicts), repository = repository_, sessionId, loaded, recovery,
         recoveryDirectory, recoveryInventory, executable = gitExecutable_, chosen = recoverySession_](const TaskInput &, const TaskControl &control) {
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
                    detail = QStringLiteral("Load error: conflict inspection did not provide complete source and result snapshots; reopen the session.");
                }
                loaded->emplace_back(file.path.toHex().toStdString(), file.path.toStdString(), std::move(draft),
                                     support, detail.toStdString(), std::move(snapshots), true);
            }
            *recovery = restoreRecovery(recoveryInventory, recoveryDirectory, repository, *loaded, executable, chosen);
            return TaskResult{};
        }, [this, loaded, recovery](const TaskCompletion &completion) {
            const auto diagnostics = recovery->diagnostics.join(QLatin1Char('\n'));
            const bool recovered = recovery->found;
            auto choices = recovery->choices;
            owners_.clearCopiedLaunches();
            auto copiedLaunches = recovery->copiedLaunches;
            const bool replaced = completion.result.status == TaskStatus::Succeeded
                && coordinator_.completeRecoveryLoad(completion.stamp, std::move(*loaded), std::move(*recovery));
            owners_.reassignMergetool();
            if (!replaced) {
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
                if (!diagnostics.isEmpty()) setState(QStringLiteral("error"), diagnostics);
                else if (repository_.operation == git::RepositoryOperation::None) setState(QStringLiteral("no-session"), noOperationDetail());
                else setState(QStringLiteral("no-session"),
                    tr("This operation has no current unmerged files. Return to your Git workflow."));
                return;
            }
            auto listen = MergetoolSession::Listen::Listening;
            QString listenError;
            if (repository_.supported) listen = owners_.startSession(repository_, std::move(copiedLaunches), &listenError);
            auto detail = recovered ? tr("Recovered app drafts and decisions. Manual Save, review and Git resolution keep their recorded status.")
                : tr("Current conflict files are open. Files marked outside app remain outstanding.");
            if (listen == MergetoolSession::Listen::Elsewhere)
                detail += QLatin1Char('\n') + tr("Another Branch Truce window receives git mergetool launches for this operation.");
            if (listen == MergetoolSession::Listen::Unavailable)
                detail = tr("Mergetool attach is unavailable: %1").arg(listenError) + QLatin1Char('\n') + detail;
            if (!diagnostics.isEmpty()) detail += QLatin1Char('\n') + diagnostics;
            recoveryChoices_ = std::move(choices);
            setState(listen == MergetoolSession::Listen::Unavailable ? QStringLiteral("ready-without-mergetool")
                                                                     : QStringLiteral("ready"), detail);
        });
    if (task == 0) setState(QStringLiteral("error"), tr("Could not schedule conflict loading."));
}
}
