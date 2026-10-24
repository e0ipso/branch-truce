#include "ApplicationController.h"
#include "core/BuildInfo.h"
#include <QStandardPaths>
#include <algorithm>

namespace branch_truce {
ApplicationController::ApplicationController(QObject *parent)
    : QObject(parent), files_(this),
      coordinator_(files_, QStandardPaths::findExecutable(QStringLiteral("git")), this),
      classifier_(QStandardPaths::findExecutable(QStringLiteral("git")), this)
{
    connect(&classifier_, &git::ConflictClassifier::finished, this, &ApplicationController::classified);
}
QString ApplicationController::version() const
{
    return QString::fromUtf8(branch_truce::version().data(),
                             static_cast<qsizetype>(branch_truce::version().size()));
}

QString ApplicationController::stage2() const
{
    if (!repository_.supported) return {};
    return repository_.stage2.label + QStringLiteral(" · ") + QString::fromLatin1(repository_.stage2.commitId);
}

QString ApplicationController::stage3() const
{
    if (!repository_.supported) return {};
    return repository_.stage3.label + QStringLiteral(" · ") + QString::fromLatin1(repository_.stage3.commitId);
}

QString ApplicationController::workflow() const
{
    return tr("Continue the operation and commit in your usual Git workflow. "
              "This session covers only the work opened at launch.");
}

void ApplicationController::setState(QString state, QString detail)
{
    state_ = std::move(state);
    detail_ = std::move(detail);
    Q_EMIT launchChanged();
}

bool ApplicationController::openRepository(const QString &directory)
{
    if (state_ != QLatin1String("idle")) return false;
    setState(QStringLiteral("opening"), tr("Inspecting the selected local repository…"));
    git::ConflictClassificationRequest request;
    request.directory = directory;
    if (!classifier_.start(request)) {
        setState(QStringLiteral("error"), tr("Could not start repository inspection."));
        return false;
    }
    return true;
}

void ApplicationController::classified(const git::ConflictClassificationResult &result)
{
    repository_ = result.repository;
    switch (repository_.operation) {
    case git::RepositoryOperation::Merge: operation_ = tr("Merge"); break;
    case git::RepositoryOperation::Rebase: operation_ = tr("Rebase"); break;
    case git::RepositoryOperation::Unsupported: operation_ = tr("Unsupported operation"); break;
    case git::RepositoryOperation::None: break;
    }
    if (result.outcome != git::Outcome::Success) operation_.clear();

    if (result.outcome != git::Outcome::Success || !repository_.supported) {
        std::vector<FileSession> files;
        for (const auto &file : result.conflicts) {
            files.emplace_back(file.path.toHex().toStdString(), file.path.toStdString(), Draft{},
                               Support::OutsideApp, file.reason.toStdString());
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
    if (result.conflicts.empty()) {
        setState(QStringLiteral("no-session"), tr("This operation has no current unmerged files. Return to your Git workflow."));
        return;
    }
    loadWorkingFiles(result.conflicts);
}

void ApplicationController::loadWorkingFiles(QList<git::Conflict> conflicts)
{
    std::vector<FileSession> pending;
    for (const auto &file : conflicts) {
        pending.emplace_back(file.path.toHex().toStdString(), file.path.toStdString(), Draft{},
                             Support::Unreadable, "Opening the classified conflict");
    }
    const auto identity = operation_ + QLatin1Char(':') + QString::fromLatin1(repository_.stage2.commitId)
        + QLatin1Char(':') + QString::fromLatin1(repository_.stage3.commitId);
    const auto root = repository_.worktreeRoot;
    if (!coordinator_.replaceSession(root, identity, std::move(pending))) {
        setState(QStringLiteral("error"), tr("Could not bind the session to the discovered worktree."));
        return;
    }
    auto loaded = std::make_shared<std::vector<FileSession>>();
    const auto anchor = conflicts.front().path.toHex().toStdString();
    const auto task = coordinator_.submit(anchor, 1,
        [files = std::move(conflicts), loaded](const TaskInput &, const TaskControl &control) {
            for (const auto &file : files) {
                if (control.cancellationRequested()) return TaskResult{TaskStatus::Cancelled, {}, {}, {}, {}};
                Draft draft;
                auto support = Support::OutsideApp;
                auto detail = file.reason;
                if (file.editable() && file.result) {
                    support = Support::Supported;
                    draft.text = file.result->utf8.toStdString();
                    const auto lines = file.result->utf8.split('\n');
                    draft.unresolvedMarkers = static_cast<std::size_t>(std::count_if(
                        lines.cbegin(), lines.cend(), [](const auto &line) { return line.startsWith("<<<<<<<"); }));
                } else if (detail.isEmpty()) {
                    detail = QStringLiteral("Outside app: conflict inspection did not provide an editable result.");
                }
                loaded->emplace_back(file.path.toHex().toStdString(), file.path.toStdString(), std::move(draft),
                                     support, detail.toStdString());
            }
            return TaskResult{};
        }, [this, root, identity, loaded](const TaskCompletion &completion) {
            if (completion.result.status != TaskStatus::Succeeded
                || !coordinator_.replaceSession(root, identity, std::move(*loaded))) {
                setState(QStringLiteral("error"), tr("Could not load the current conflict files."));
                return;
            }
            setState(QStringLiteral("ready"), tr("Current conflict files are open. Files marked outside app remain outstanding."));
        });
    if (task == 0) setState(QStringLiteral("error"), tr("Could not schedule conflict loading."));
}
}
