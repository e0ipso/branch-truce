#include "SessionLoader.h"
#include "ConflictCapture.h"
#include "SessionCoordinator.h"
#include "SessionModel.h"
#include <QFileInfo>
#include <QLockFile>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>
#include <memory>

namespace branch_truce {
SessionLoader::SessionLoader(SessionModel &files, SessionCoordinator &coordinator, Owners owners, QObject *parent)
    : QObject(parent), files_(files), coordinator_(coordinator), owners_(std::move(owners)),
      classifier_(QStandardPaths::findExecutable(QStringLiteral("git")), this)
{
    connect(&classifier_, &git::ConflictClassifier::finished, this, &SessionLoader::classified);
}

QString SessionLoader::stage2() const
{
    if (!repository_.supported) return {};
    return repository_.stage2.label;
}

QString SessionLoader::stage3() const
{
    if (!repository_.supported) return {};
    return repository_.stage3.label;
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

QString SessionLoader::workflow() const
{
    return tr("Continue the operation and commit in your usual Git workflow. "
              "This session covers only the work opened at launch.");
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

bool SessionLoader::chooseRecoverySession(const QString &session)
{
    const auto refuse = [this](const QString &message) {
        Q_EMIT notice(message);
        return false;
    };
    const auto chosen = std::find_if(recoveryChoices_.cbegin(), recoveryChoices_.cend(),
        [&](const auto &choice) { return choice.session == session; });
    if (state_ != QLatin1String("ready") && state_ != QLatin1String("ready-without-mergetool"))
        return refuse(tr("Choose a recovery session after the session finishes loading."));
    if (chosen == recoveryChoices_.cend())
        return refuse(tr("That recovery session is not one of the choices for this session."));
    if (owners_.sessionBusy() || files_.hasPendingRecovery())
        return refuse(tr("Choose a recovery session after the running resolution or pending recovery input finishes."));
    // The reload replaces this window's files. Edits already recorded here
    // would be left in a store nobody chose, so they refuse the choice.
    const auto own = replayRecovery(storage::RecoveryStore(files_.recoveryDirectory()));
    if (!own.succeeded)
        return refuse(tr("This window's recovery store could not be read, so it cannot switch sessions: %1").arg(own.error));
    if (own.userWork)
        return refuse(tr("This window already holds app edits, so it cannot switch to a recovery session. "
            "Leave this session, then start Branch Truce with --recovery-session followed by the session to keep."));
    // These locks are released before the reload. If another window takes the
    // chosen session in between, the restore adopts no other edited session.
    for (const auto &choice : recoveryChoices_) {
        QLockFile lease(choice.directory + QStringLiteral("/session.lock"));
        if (!lease.tryLock(0))
            return refuse(tr("Recovery session %1 is open in another Branch Truce window. Close that window, then choose again.")
                .arg(choice.session));
    }
    if (!files_.startNewRecoveryStore())
        return refuse(tr("Choose a recovery session after the pending recovery input finishes."));
    Q_EMIT notice({});
    state_ = QStringLiteral("idle");
    return openRepository(repository_.worktreeRoot, owners_.selectedFileId(), session);
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
            setState(QStringLiteral("no-session"), tr("No merge or rebase is in progress in this repository."));
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
    const auto executable = QStandardPaths::findExecutable(QStringLiteral("git"));
    const auto sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    const auto task = coordinator_.submit(anchor, 1,
        [files = std::move(conflicts), repository = repository_, sessionId, loaded, recovery,
         recoveryDirectory, recoveryInventory, executable, chosen = recoverySession_](const TaskInput &, const TaskControl &control) {
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
                setState(diagnostics.isEmpty() ? QStringLiteral("no-session") : QStringLiteral("error"),
                    diagnostics.isEmpty() ? tr("This operation has no current unmerged files. Return to your Git workflow.") : diagnostics);
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
