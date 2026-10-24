#include "ApplicationController.h"
#include "RecoveryRestore.h"
#include <QDir>
#include <QFileInfo>
#include "core/BuildInfo.h"
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>

namespace branch_truce {
namespace {
QString text(const std::string &value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QVariantMap sourceData(const std::optional<SourceSnapshot> &source)
{
    if (!source) return {};
    return {{QStringLiteral("available"), true}, {QStringLiteral("text"), text(source->worktree.utf8)},
            {QStringLiteral("label"), text(source->label)}, {QStringLiteral("objectId"), text(source->objectId)},
            {QStringLiteral("commitId"), source->commitId ? text(*source->commitId) : QString{}},
            {QStringLiteral("mode"), text(source->mode)}};
}

Support supportFor(const git::Conflict &file)
{
    return file.support == git::ConflictSupport::LoadFailure || file.support == git::ConflictSupport::InspectionFailure
        ? Support::Unreadable : Support::OutsideApp;
}

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
    connect(&files_, &SessionModel::summaryChanged, this, [this] {
        if (!files_.file(selectedFileId_.toStdString())) {
            selectedFileId_ = files_.totalFiles() > 0
                ? files_.data(files_.index(0), SessionModel::FileIdRole).toString() : QString{};
        }
        Q_EMIT inventoryChanged();
        Q_EMIT selectedFileChanged();
    });
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

QString ApplicationController::displayPath(const QString &fileId) const
{
    const auto *file = files_.file(fileId.toStdString());
    if (!file) return {};
    auto display = text(file->path());
    const QByteArray bytes(file->path().data(), static_cast<qsizetype>(file->path().size()));
    const bool exact = display.toUtf8() == bytes;
    display.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    display.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    display.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    display.replace(QLatin1Char('\t'), QStringLiteral("\\t"));
    return exact ? display : tr("%1 [path bytes: %2]").arg(display, QString::fromLatin1(bytes.toHex()));
}

QVariantMap ApplicationController::selectedFile() const
{
    const auto *file = files_.file(selectedFileId_.toStdString());
    if (!file) return {};
    QString support;
    switch (file->support()) {
    case Support::Supported: support = QStringLiteral("supported"); break;
    case Support::OutsideApp: support = QStringLiteral("outside-app"); break;
    case Support::Unreadable: support = QStringLiteral("unreadable"); break;
    }
    QVariantMap result{{QStringLiteral("fileId"), selectedFileId_},
        {QStringLiteral("path"), displayPath(selectedFileId_)},
        {QStringLiteral("pathBytes"), QByteArray(file->path().data(), static_cast<qsizetype>(file->path().size()))},
        {QStringLiteral("support"), support}, {QStringLiteral("supportDetail"), text(file->supportDetail())},
        {QStringLiteral("resultText"), text(file->current().draft.text)}};
    for (int row = 0; row < files_.totalFiles(); ++row) {
        const auto index = files_.index(row);
        if (files_.data(index, SessionModel::FileIdRole).toString() != selectedFileId_) continue;
        for (const auto role : {SessionModel::StatusRole, SessionModel::UnsavedRole, SessionModel::ReviewNeededRole,
             SessionModel::ResolvedRole, SessionModel::UndecidedRole, SessionModel::PendingInputRole,
             SessionModel::PendingTextRole, SessionModel::RecoveryStateRole, SessionModel::RecoveryErrorRole,
             SessionModel::FinishBlockersRole})
            result.insert(QString::fromLatin1(files_.roleNames().value(role)), files_.data(index, role));
        break;
    }
    if (const auto &snapshots = file->snapshots(); snapshots && file->support() == Support::Supported) {
        result.insert(QStringLiteral("stage2"), sourceData(snapshots->stage2));
        result.insert(QStringLiteral("stage3"), sourceData(snapshots->stage3));
        result.insert(QStringLiteral("earlier"), sourceData(snapshots->earlier));
        result.insert(QStringLiteral("initialResult"), QVariantMap{
            {QStringLiteral("text"), text(snapshots->launchResult.text.utf8)}});
    }
    return result;
}

bool ApplicationController::selectFile(const QString &fileId)
{
    if (!files_.file(fileId.toStdString())) return false;
    if (selectedFileId_ == fileId) return true;
    selectedFileId_ = fileId;
    Q_EMIT selectedFileChanged();
    return true;
}

int ApplicationController::outsideAppFiles() const
{
    int count = 0;
    for (int row = 0; row < files_.totalFiles(); ++row)
        if (files_.data(files_.index(row), SessionModel::SupportRole).toString() == QLatin1String("outside-app")) ++count;
    return count;
}

int ApplicationController::loadErrorFiles() const
{
    int count = 0;
    for (int row = 0; row < files_.totalFiles(); ++row)
        if (files_.data(files_.index(row), SessionModel::SupportRole).toString() == QLatin1String("unreadable")) ++count;
    return count;
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

void ApplicationController::loadWorkingFiles(QList<git::Conflict> conflicts)
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
         recoveryDirectory, recoveryInventory, executable](const TaskInput &, const TaskControl &control) {
            for (const auto &file : files) {
                if (control.cancellationRequested()) return TaskResult{TaskStatus::Cancelled, {}, {}, {}, {}};
                Draft draft;
                auto support = supportFor(file);
                auto detail = file.reason;
                auto snapshots = captureFile(file, repository, sessionId);
                if (snapshots) {
                    support = Support::Supported;
                    draft.text = file.result->utf8.toStdString();
                    const auto lines = file.result->utf8.split('\n');
                    draft.unresolvedMarkers = static_cast<std::size_t>(std::count_if(
                        lines.cbegin(), lines.cend(), [](const auto &line) { return line.startsWith("<<<<<<<"); }));
                } else if (detail.isEmpty()) {
                    support = Support::Unreadable;
                    detail = QStringLiteral("Load error: conflict inspection did not provide complete source and result snapshots; reopen the session.");
                }
                loaded->emplace_back(file.path.toHex().toStdString(), file.path.toStdString(), std::move(draft),
                                     support, detail.toStdString(), std::move(snapshots));
            }
            *recovery = restoreRecovery(recoveryInventory, recoveryDirectory, repository, *loaded, executable);
            return TaskResult{};
        }, [this, loaded, recovery](const TaskCompletion &completion) {
            const auto diagnostics = recovery->diagnostics.join(QLatin1Char('\n'));
            const bool recovered = recovery->found;
            if (completion.result.status != TaskStatus::Succeeded
                || !coordinator_.completeRecoveryLoad(completion.stamp, std::move(*loaded), std::move(*recovery))) {
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
            auto detail = recovered ? tr("Recovered app drafts and decisions. Manual Save, review and Git resolution keep their recorded status.")
                : tr("Current conflict files are open. Files marked outside app remain outstanding.");
            if (!diagnostics.isEmpty()) detail += QLatin1Char('\n') + diagnostics;
            setState(QStringLiteral("ready"), detail);
        });
    if (task == 0) setState(QStringLiteral("error"), tr("Could not schedule conflict loading."));
}
}
