#include "ApplicationController.h"
#include "core/BuildInfo.h"
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>

namespace branch_truce {
namespace {
TextSnapshot textSnapshot(const storage::TextDocument &document)
{
    SnapshotFormat format;
    switch (document.format.encoding) {
    case storage::TextEncoding::Utf8: format.encoding = SnapshotFormat::Encoding::Utf8; break;
    case storage::TextEncoding::Utf16LittleEndian: format.encoding = SnapshotFormat::Encoding::Utf16LittleEndian; break;
    case storage::TextEncoding::Utf16BigEndian: format.encoding = SnapshotFormat::Encoding::Utf16BigEndian; break;
    }
    format.bom = document.format.bom;
    format.newline = document.format.newline == storage::Newline::CrLf
        ? SnapshotFormat::Newline::CrLf : SnapshotFormat::Newline::Lf;
    format.finalNewline = document.format.finalNewline;
    return {document.utf8.toStdString(), document.originalBytes.toStdString(), format};
}

std::optional<FileSnapshots> captureFile(const git::Conflict &file,
    const git::RepositoryContext &repository, const std::string &sessionId)
{
    if (!file.editable() || !file.result || !file.resultIdentity || !repository.supported
        || file.sources.size() != file.stages.size() || file.sources.isEmpty()) return std::nullopt;
    FileSnapshots captured;
    captured.path = file.path.toStdString();
    captured.resultMode = file.mode.toStdString();
    captured.launchResult = {textSnapshot(*file.result), *file.resultIdentity};
    auto &provenance = captured.provenance;
    provenance.sessionId = sessionId;
    provenance.worktreeRoot = repository.worktreeRoot.toStdString();
    provenance.gitDirectory = repository.gitDirectory.toStdString();
    if (repository.operation == git::RepositoryOperation::Merge) provenance.operation = SnapshotProvenance::Operation::Merge;
    else if (repository.operation == git::RepositoryOperation::Rebase) provenance.operation = SnapshotProvenance::Operation::Rebase;
    else return std::nullopt;
    provenance.stage2CommitId = repository.stage2.commitId.toStdString();
    provenance.stage3CommitId = repository.stage3.commitId.toStdString();
    if (!repository.baseCommitId.isEmpty()) provenance.baseCommitId = repository.baseCommitId.toStdString();
    provenance.headName = repository.headName.toStdString();
    provenance.originalHead = repository.originalHead.toStdString();
    provenance.onto = repository.onto.toStdString();
    const auto &policy = file.sources.front().policy;
    for (auto entry = policy.attributes.cbegin(); entry != policy.attributes.cend(); ++entry)
        captured.conversion.attributes.emplace(entry.key().toStdString(), entry.value().toStdString());
    for (auto entry = policy.configuration.cbegin(); entry != policy.configuration.cend(); ++entry)
        captured.conversion.configuration.emplace(entry.key().toStdString(), entry.value().toStdString());
    for (qsizetype i = 0; i < file.stages.size(); ++i) {
        const auto &stage = file.stages[i];
        const auto &source = file.sources[i];
        if (stage.path != file.path || source.path != file.path || source.worktreeRoot != repository.worktreeRoot
            || source.canonicalObjectId != stage.objectId || source.sourceMode != stage.mode
            || source.mode != file.mode || source.policy != policy) return std::nullopt;
        SourceSnapshot snapshot{stage.objectId.toStdString(), stage.mode.toStdString(),
            source.canonicalBytes.toStdString(), textSnapshot(source.document), std::nullopt, {}};
        if (stage.stage == 1 && !captured.earlier) {
            snapshot.commitId = provenance.baseCommitId;
            snapshot.label = "Common earlier version";
            captured.earlier = std::move(snapshot);
        } else if (stage.stage == 2 && !captured.stage2) {
            snapshot.commitId = provenance.stage2CommitId;
            snapshot.label = repository.stage2.label.toStdString();
            captured.stage2 = std::move(snapshot);
        } else if (stage.stage == 3 && !captured.stage3) {
            snapshot.commitId = provenance.stage3CommitId;
            snapshot.label = repository.stage3.label.toStdString();
            captured.stage3 = std::move(snapshot);
        } else return std::nullopt;
    }
    if (!captured.stage2 || !captured.stage3) return std::nullopt;
    return captured;
}
}

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
    const auto sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    const auto task = coordinator_.submit(anchor, 1,
        [files = std::move(conflicts), repository = repository_, sessionId, loaded](const TaskInput &, const TaskControl &control) {
            for (const auto &file : files) {
                if (control.cancellationRequested()) return TaskResult{TaskStatus::Cancelled, {}, {}, {}, {}};
                Draft draft;
                auto support = Support::OutsideApp;
                auto detail = file.reason;
                auto snapshots = captureFile(file, repository, sessionId);
                if (snapshots) {
                    support = Support::Supported;
                    draft.text = file.result->utf8.toStdString();
                    const auto lines = file.result->utf8.split('\n');
                    draft.unresolvedMarkers = static_cast<std::size_t>(std::count_if(
                        lines.cbegin(), lines.cend(), [](const auto &line) { return line.startsWith("<<<<<<<"); }));
                } else if (detail.isEmpty()) {
                    detail = QStringLiteral("Outside app: conflict inspection did not provide complete source and result snapshots; inspect again.");
                }
                loaded->emplace_back(file.path.toHex().toStdString(), file.path.toStdString(), std::move(draft),
                                     support, detail.toStdString(), std::move(snapshots));
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
