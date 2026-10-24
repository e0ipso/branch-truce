#include "ResolutionPublisher.h"

#include <QHash>
#include <QPointer>
#include <memory>
#include <utility>

namespace branch_truce {
namespace {
bool succeeded(const std::optional<OperationOutcome> &outcome, RevisionId revision)
{
    return outcome && outcome->revision == revision && outcome->outcome == Outcome::Succeeded;
}

storage::TextFormat launchFormat(const SnapshotFormat &format)
{
    storage::TextFormat result;
    switch (format.encoding) {
    case SnapshotFormat::Encoding::Utf8: result.encoding = storage::TextEncoding::Utf8; break;
    case SnapshotFormat::Encoding::Utf16LittleEndian: result.encoding = storage::TextEncoding::Utf16LittleEndian; break;
    case SnapshotFormat::Encoding::Utf16BigEndian: result.encoding = storage::TextEncoding::Utf16BigEndian; break;
    }
    result.bom = format.bom;
    result.newline = format.newline == SnapshotFormat::Newline::CrLf ? storage::Newline::CrLf : storage::Newline::Lf;
    result.finalNewline = format.finalNewline;
    return result;
}

std::optional<QByteArray> writtenRevisionBytes(const FileSession &session)
{
    const auto *snapshots = session.snapshots();
    if (!snapshots || !session.written()) return std::nullopt;
    const auto serialized = storage::serializeText(QByteArray::fromStdString(session.written()->draft.text),
        launchFormat(snapshots->launchResult.text.format));
    if (!serialized.document) return std::nullopt;
    return serialized.document->originalBytes;
}

bool samePairs(const std::map<std::string, std::string> &expected, const QHash<QByteArray, QByteArray> &live)
{
    if (expected.size() != static_cast<std::size_t>(live.size())) return false;
    for (auto it = live.cbegin(); it != live.cend(); ++it) {
        const auto found = expected.find(it.key().toStdString());
        if (found == expected.end() || found->second != it.value().toStdString()) return false;
    }
    return true;
}

bool samePolicy(const ConversionSnapshot &expected, const git::ConversionPolicy &live)
{
    return samePairs(expected.attributes, live.attributes) && samePairs(expected.configuration, live.configuration);
}

bool sameOperation(const SnapshotProvenance &provenance, const git::RepositoryContext &repository)
{
    if (!repository.supported || repository.worktreeRoot.toStdString() != provenance.worktreeRoot
        || repository.gitDirectory.toStdString() != provenance.gitDirectory) return false;
    if (repository.operation == git::RepositoryOperation::Merge)
        return provenance.operation == SnapshotProvenance::Operation::Merge
            && repository.stage2.commitId.toStdString() == provenance.stage2CommitId
            && repository.stage3.commitId.toStdString() == provenance.stage3CommitId;
    if (repository.operation == git::RepositoryOperation::Rebase)
        return provenance.operation == SnapshotProvenance::Operation::Rebase
            && repository.stage2.commitId.toStdString() == provenance.stage2CommitId
            && repository.stage3.commitId.toStdString() == provenance.stage3CommitId;
    return false;
}

QList<git::IndexEntry> capturedStages(const FileSnapshots &snapshots)
{
    QList<git::IndexEntry> stages;
    const auto path = QByteArray::fromStdString(snapshots.path);
    auto add = [&](unsigned int stage, const std::optional<SourceSnapshot> &source) {
        if (!source) return;
        stages.push_back({path, QByteArray::fromStdString(source->mode),
            QByteArray::fromStdString(source->objectId), stage});
    };
    add(1, snapshots.earlier);
    add(2, snapshots.stage2);
    add(3, snapshots.stage3);
    return stages;
}

bool originalConflict(const QList<git::IndexEntry> &entries, const FileSnapshots &snapshots)
{
    const int expected = (snapshots.earlier ? 1 : 0) + 2;
    if (entries.size() != expected || !snapshots.stage2 || !snapshots.stage3) return false;
    bool seen[4]{};
    for (const auto &entry : entries) {
        const auto &source = entry.stage == 1 ? snapshots.earlier
            : entry.stage == 2 ? snapshots.stage2 : snapshots.stage3;
        if (entry.stage < 1 || entry.stage > 3 || seen[entry.stage] || !source) return false;
        if (entry.path.toStdString() != snapshots.path || entry.objectId.toStdString() != source->objectId
            || entry.mode.toStdString() != source->mode) return false;
        seen[entry.stage] = true;
    }
    return seen[2] && seen[3] && (!snapshots.earlier || seen[1]);
}

bool stageZero(const QList<git::IndexEntry> &entries, const QByteArray &path, const QByteArray &mode,
               const QByteArray &objectId)
{
    return entries.size() == 1 && entries.front().stage == 0 && entries.front().path == path
        && entries.front().mode == mode && (objectId.isEmpty() || entries.front().objectId == objectId);
}

Outcome failedObservation(const TaskResult &result)
{
    if (result.mutation == MutationObservation::Uncertain) return Outcome::Uncertain;
    if (result.gitResult && result.gitResult->outcome == git::Outcome::TimedOut
        && result.mutation != MutationObservation::NotAttempted) return Outcome::Uncertain;
    if (result.status == TaskStatus::Cancelled && result.mutation != MutationObservation::NotAttempted)
        return Outcome::Uncertain;
    return Outcome::Failed;
}
}

ResolutionPublisher::ResolutionPublisher(SessionModel &files, SessionCoordinator &coordinator,
                                         QString gitExecutable, QObject *parent)
    : QObject(parent), files_(files), coordinator_(coordinator), executable_(std::move(gitExecutable)),
      conversion_(executable_, this)
{
    connect(&conversion_, &git::TextConversion::finished, this, [this](const git::TextConversionResult &result) {
        if (!attempt_) return;
        if (attempt_->step == Step::Prepare) prepared(result);
        else if (attempt_->step == Step::Verify) verified(result);
    });
    connect(&coordinator_, &SessionCoordinator::sessionChanged, this, [this] {
        conversion_.cancel();
        attempt_.reset();
        baselines_.clear();
        offer_.reset();
        parentPath_.clear();
        confirmation_.reset();
    });
    connect(&coordinator_, &SessionCoordinator::taskFinished, this, [this](const TaskCompletion &completion) {
        if (!attempt_ || completion.stamp.id != attempt_->task) return;
        if (completion.disposition == TaskDisposition::Current) return;
        noteStale(completion);
    });
}

void ResolutionPublisher::captureBaselines(const git::RepositoryContext &repository)
{
    repository_ = repository;
    baselines_.clear();
    for (int row = 0; row < files_.totalFiles(); ++row) {
        const auto id = files_.data(files_.index(row), SessionModel::FileIdRole).toString().toStdString();
        const auto *session = files_.file(id);
        if (!session || session->support() != Support::Supported || !session->snapshots()) continue;
        Baseline baseline;
        const auto captured = storage::captureDestination(repository.worktreeRoot,
            QByteArray::fromStdString(session->path()));
        const auto &launch = session->snapshots()->launchResult;
        if (!captured.destination) baseline.diagnostic = captured.diagnostic;
        else if (captured.destination->identity() != launch.identity
            || captured.destination->bytes().toStdString() != launch.text.bytes) {
            baseline.diagnostic = tr("The repository file no longer matches the captured launch result.");
        } else baseline.destination = captured.destination;
        baselines_.emplace(id, std::move(baseline));
    }
}

bool ResolutionPublisher::assignParentStaging(const QByteArray &path)
{
    if (attempt_) return false;
    if (path.isEmpty()) {
        parentPath_.clear();
        offer_.reset();
        Q_EMIT changed(tr("Parent staging is not assigned."));
        return true;
    }
    for (int row = 0; row < files_.totalFiles(); ++row) {
        const auto id = files_.data(files_.index(row), SessionModel::FileIdRole).toString().toStdString();
        const auto *session = files_.file(id);
        if (!session || session->support() != Support::Supported || QByteArray::fromStdString(session->path()) != path)
            continue;
        parentPath_ = path;
        offer_.reset();
        Q_EMIT changed(tr("Git will stage this path. Branch Truce will not stage it."));
        return true;
    }
    return false;
}

bool ResolutionPublisher::busy() const { return attempt_.has_value(); }
bool ResolutionPublisher::confirmationPending() const { return confirmation_.has_value(); }
QString ResolutionPublisher::confirmationFileId() const
{
    return confirmation_ ? QString::fromStdString(confirmation_->first) : QString{};
}
QString ResolutionPublisher::confirmationRevision() const
{
    return confirmation_ ? QString::number(confirmation_->second) : QString{};
}
std::optional<ResolutionPublisher::Offer> ResolutionPublisher::parentStagingOffer(const std::string &fileId) const
{
    if (!offer_ || offer_->fileId != fileId) return std::nullopt;
    return offer_;
}

const FileSession *ResolutionPublisher::file(const std::string &id) const { return files_.file(id); }
bool ResolutionPublisher::parentOwns(const QByteArray &path) const
{
    return !parentPath_.isEmpty() && parentPath_ == path;
}
bool ResolutionPublisher::previouslyWritten(const FileSession &session, RevisionId revision) const
{
    return session.written() && session.written()->id == revision && succeeded(session.writeOutcome(), revision);
}
bool ResolutionPublisher::preserveWrite() const
{
    const auto *session = attempt_ ? file(attempt_->fileId) : nullptr;
    return session && previouslyWritten(*session, attempt_->revision);
}

ResolutionPublisher::StartResult ResolutionPublisher::refuse(Transition transition, const QString &detail)
{
    return {transition, detail};
}

ResolutionPublisher::StartResult ResolutionPublisher::recordWrite(const std::string &fileId, RevisionId revision,
                                                                  Outcome outcome, const QString &detail)
{
    const auto recorded = files_.recordWrite(fileId, revision, outcome, detail.toStdString());
    publication_.reset();
    clearAttempt();
    if (recorded != Transition::Applied)
        return refuse(recorded, tr("The repository write could not be recorded. The draft is still available."));
    return refuse(Transition::Blocked, detail);
}

void ResolutionPublisher::fail(Outcome outcome, const QString &detail, bool stage)
{
    if (!attempt_) return;
    const auto id = attempt_->fileId;
    const auto revision = attempt_->revision;
    const auto recorded = stage
        ? files_.recordStage(id, revision, outcome, {}, {}, detail.toStdString())
        : files_.recordWrite(id, revision, outcome, detail.toStdString());
    offer_.reset();
    publication_.reset();
    clearAttempt();
    Q_EMIT changed(recorded == Transition::Applied ? detail
        : tr("The outcome could not be recorded. The draft is still available."));
}

void ResolutionPublisher::finishResolved()
{
    if (!attempt_ || !attempt_->prepared) return;
    const auto recorded = files_.recordStage(attempt_->fileId, attempt_->revision, Outcome::Succeeded,
        attempt_->prepared->canonicalObjectId.toStdString(), attempt_->prepared->mode.toStdString());
    offer_.reset();
    clearAttempt();
    Q_EMIT changed(recorded == Transition::Applied ? tr("Written and resolved.")
        : tr("Git has the reviewed result, but the resolution could not be recorded."));
}

void ResolutionPublisher::clearAttempt()
{
    attempt_.reset();
}

bool ResolutionPublisher::launch(Step step, git::Request request)
{
    if (!attempt_) return false;
    attempt_->step = step;
    QPointer<ResolutionPublisher> self(this);
    const auto id = coordinator_.submitGit(attempt_->fileId, attempt_->revision, request,
        [self](const TaskCompletion &completion) {
            if (!self || !self->accept(completion)) return;
            switch (self->attempt_->step) {
            case Step::Discover: self->discovered(completion); break;
            case Step::Index: self->indexed(completion); break;
            case Step::Stage: self->staged(completion); break;
            case Step::ParentPreflight: self->parentPreflight(completion); break;
            case Step::Prepare:
            case Step::Publish:
            case Step::Verify: break;
            }
        });
    if (!attempt_) return false;
    attempt_->task = id;
    if (id != 0) return true;
    const auto *session = file(attempt_->fileId);
    const bool stage = session && previouslyWritten(*session, attempt_->revision);
    fail(Outcome::Failed, tr("Publication could not start. The draft is still available."), stage);
    return false;
}

bool ResolutionPublisher::accept(const TaskCompletion &completion) const
{
    return attempt_ && completion.stamp.id == attempt_->task
        && completion.disposition == TaskDisposition::Current;
}

ResolutionPublisher::StartResult ResolutionPublisher::start(const std::string &fileId, RevisionId expected,
                                                           bool confirmUndecided)
{
    if (attempt_) return refuse(Transition::Blocked, tr("Mark resolved is already working on a file."));
    const auto *session = file(fileId);
    if (!session || session->current().id != expected)
        return refuse(Transition::StaleRevision, tr("The result changed. Mark resolved was not applied."));
    const auto uncertain = [&](const std::optional<OperationOutcome> &outcome) {
        return outcome && outcome->revision == expected && outcome->outcome == Outcome::Uncertain;
    };
    if (uncertain(session->checkpointOutcome()) || uncertain(session->writeOutcome()) || uncertain(session->stageOutcome()))
        return refuse(Transition::Blocked, tr("Outcome uncertain. Inspect the repository file and index before retrying."));
    if (session->facts().resolved)
        return refuse(Transition::NoChange, tr("Written and resolved."));
    confirmation_.reset();
    const auto review = files_.beginMarkResolved(fileId, expected, confirmUndecided);
    if (review == Transition::ConfirmationRequired) {
        confirmation_ = std::make_pair(fileId, expected);
        return refuse(Transition::ConfirmationRequired,
            tr("Confirm that this result is intentional. Some source choices are still undecided."));
    }
    if (review != Transition::Applied)
        return refuse(review, tr("Mark resolved is blocked. The repository file is unchanged."));
    session = file(fileId);
    if (!session) return refuse(Transition::StaleRevision, tr("The result changed. Mark resolved was not applied."));
    const bool checkpointed = session->checkpoint() && session->checkpoint()->id == expected
        && session->checkpoint()->result == session->current().result
        && session->checkpoint()->decisions == session->current().decisions
        && succeeded(session->checkpointOutcome(), expected);
    if (!checkpointed) {
        const auto saved = files_.recordCheckpoint(fileId, expected, Outcome::Succeeded);
        if (saved != Transition::Applied)
            return refuse(saved, tr("Draft save failed. The edited result is still available, and the repository file is unchanged."));
    }
    session = file(fileId);
    if (!session || !session->snapshots())
        return recordWrite(fileId, expected, Outcome::Failed, tr("Repository write failed. The saved draft is still available."));
    const auto serialized = storage::serializeText(QByteArray::fromStdString(session->current().draft.text),
        launchFormat(session->snapshots()->launchResult.text.format));
    if (!serialized.document)
        return recordWrite(fileId, expected, Outcome::Failed, serialized.diagnostic.isEmpty()
            ? tr("Repository write failed. The saved draft is still available.") : serialized.diagnostic);
    auto baseline = baselines_.find(fileId);
    if (baseline == baselines_.end() || !baseline->second.destination) {
        const auto missingLaunch = [&] {
            return recordWrite(fileId, expected, Outcome::Failed,
                baseline == baselines_.end() || baseline->second.diagnostic.isEmpty()
                    ? tr("Repository write failed. No launch destination is available, and the file is unresolved.")
                    : baseline->second.diagnostic);
        };
        // Restart only has the launch snapshot. A successful write already replaced
        // that file, so keep the write when the live bytes are still that revision.
        if (!previouslyWritten(*session, expected)) return missingLaunch();
        const auto live = storage::captureDestination(repository_.worktreeRoot,
            QByteArray::fromStdString(session->path()));
        const auto writtenBytes = writtenRevisionBytes(*session);
        if (!live.destination || !writtenBytes || live.destination->bytes() != *writtenBytes
            || *writtenBytes != serialized.document->originalBytes) {
            return recordWrite(fileId, expected, Outcome::Failed, live.destination
                ? tr("The repository file no longer matches the written revision.")
                : (live.diagnostic.isEmpty()
                    ? tr("Repository write failed. No launch destination is available, and the file is unresolved.")
                    : live.diagnostic));
        }
        if (baseline == baselines_.end())
            baselines_.emplace(fileId, Baseline{std::move(live.destination), QString{}});
        else {
            baseline->second.destination = std::move(live.destination);
            baseline->second.diagnostic.clear();
        }
    }
    offer_.reset();
    attempt_ = Attempt{};
    attempt_->generation = coordinator_.generation();
    attempt_->fileId = fileId;
    attempt_->revision = expected;
    attempt_->path = QByteArray::fromStdString(session->path());
    attempt_->mode = QByteArray::fromStdString(session->snapshots()->resultMode);
    attempt_->serialized = serialized.document->originalBytes;
    attempt_->parentOwned = parentOwns(attempt_->path);
    if (!startPrepare()) return refuse(Transition::Blocked, tr("Publication could not start. The draft is still available."));
    return refuse(Transition::Applied, tr("Publishing the reviewed result."));
}

void ResolutionPublisher::cancelConfirmation()
{
    if (!confirmation_) return;
    confirmation_.reset();
    Q_EMIT changed(tr("Resolution cancelled. The text and choices are unchanged."));
}

bool ResolutionPublisher::verifyParentAcknowledgment(const std::string &fileId, RevisionId expected)
{
    if (attempt_ || !offer_ || offer_->fileId != fileId || offer_->revision != expected) return false;
    const auto *session = file(fileId);
    if (!session || session->current().id != expected || !previouslyWritten(*session, expected)) return false;
    if (parentPath_ != offer_->path) return false;
    attempt_ = Attempt{};
    attempt_->generation = coordinator_.generation();
    attempt_->fileId = fileId;
    attempt_->revision = expected;
    attempt_->path = offer_->path;
    attempt_->mode = offer_->mode;
    attempt_->parentOwned = true;
    attempt_->acknowledging = true;
    attempt_->prepared = offer_->prepared;
    git::Request request;
    request.operation = git::Operation::InspectIndex;
    request.path = attempt_->path;
    return launch(Step::Index, request);
}

bool ResolutionPublisher::startPrepare()
{
    if (!attempt_) return false;
    const auto *session = file(attempt_->fileId);
    if (!session || !session->snapshots()) return false;
    attempt_->step = Step::Prepare;
    git::TextConversionRequest request;
    request.operation = git::TextOperation::PrepareResult;
    request.directory = repository_.worktreeRoot;
    request.path = attempt_->path;
    request.stages = capturedStages(*session->snapshots());
    request.launchResultFormat = launchFormat(session->snapshots()->launchResult.text.format);
    request.editedUtf8 = QByteArray::fromStdString(session->current().draft.text);
    request.sessionGeneration = attempt_->generation;
    request.inputRevision = attempt_->revision;
    if (conversion_.start(request)) return true;
    fail(Outcome::Failed, tr("Publication could not start. The draft is still available."), false);
    return false;
}

bool ResolutionPublisher::startVerify()
{
    if (!attempt_ || !attempt_->prepared) return false;
    attempt_->step = Step::Verify;
    git::TextConversionRequest request;
    request.operation = git::TextOperation::VerifyPublished;
    request.directory = repository_.worktreeRoot;
    request.path = attempt_->path;
    request.expected = attempt_->prepared;
    request.sessionGeneration = coordinator_.generation();
    request.inputRevision = attempt_->revision;
    if (conversion_.start(request)) return true;
    fail(Outcome::Failed, tr("Written, unresolved. The staged result could not be verified."), true);
    return false;
}

void ResolutionPublisher::prepared(const git::TextConversionResult &result)
{
    if (!attempt_ || attempt_->step != Step::Prepare) return;
    const auto *session = file(attempt_->fileId);
    if (!session || !session->snapshots() || result.outcome != git::Outcome::Success || !result.text
        || result.text->mode != attempt_->mode || result.text->document.utf8.toStdString() != session->current().draft.text
        || !samePolicy(session->snapshots()->conversion, result.text->policy)) {
        const auto uncertain = result.outcome == git::Outcome::TimedOut;
        fail(uncertain ? Outcome::Uncertain : Outcome::Failed, result.diagnostic.isEmpty()
            ? tr("Repository write failed. The saved draft is still available, and the file is unresolved.")
            : result.diagnostic, preserveWrite());
        return;
    }
    attempt_->prepared = *result.text;
    git::Request request;
    request.operation = git::Operation::DiscoverRepository;
    launch(Step::Discover, request);
}

void ResolutionPublisher::discovered(const TaskCompletion &completion)
{
    if (!attempt_ || !completion.result.gitResult) {
        fail(failedObservation(completion.result), tr("The repository operation could not be validated. Nothing was published."), preserveWrite());
        return;
    }
    const auto *session = file(attempt_->fileId);
    const auto &gitResult = *completion.result.gitResult;
    if (!session || !session->snapshots() || gitResult.outcome != git::Outcome::Success
        || !sameOperation(session->snapshots()->provenance, gitResult.repository)) {
        fail(gitResult.outcome == git::Outcome::TimedOut ? Outcome::Uncertain : Outcome::Failed,
            tr("The repository operation changed. The reviewed result was not published."), preserveWrite());
        return;
    }
    git::Request request;
    request.operation = git::Operation::InspectIndex;
    request.path = attempt_->path;
    launch(Step::Index, request);
}

void ResolutionPublisher::indexed(const TaskCompletion &completion)
{
    if (!attempt_ || !completion.result.gitResult || !attempt_->prepared) {
        fail(failedObservation(completion.result), tr("The index could not be inspected. Nothing new was staged."), preserveWrite());
        return;
    }
    const auto *session = file(attempt_->fileId);
    const auto &gitResult = *completion.result.gitResult;
    if (!session || !session->snapshots() || gitResult.outcome != git::Outcome::Success) {
        fail(gitResult.outcome == git::Outcome::TimedOut ? Outcome::Uncertain : Outcome::Failed,
            tr("The index could not be inspected. The draft is still available."), preserveWrite());
        return;
    }
    const auto &entries = gitResult.indexEntries;
    const bool conflict = originalConflict(entries, *session->snapshots());
    const bool resolved = stageZero(entries, attempt_->path, attempt_->mode, attempt_->prepared->canonicalObjectId);
    if (attempt_->acknowledging) {
        if (conflict) {
            clearAttempt();
            Q_EMIT changed(tr("Written, unresolved. Waiting for Git to stage this path."));
            return;
        }
        if (resolved) {
            startVerify();
            return;
        }
        fail(Outcome::Failed, tr("Written, unresolved. The index does not contain the reviewed result."), true);
        return;
    }
    if (resolved && previouslyWritten(*session, attempt_->revision)) {
        startVerify();
        return;
    }
    if (!conflict) {
        fail(Outcome::Failed, tr("The index no longer matches this conflict. The repository file was not published."), preserveWrite());
        return;
    }
    if (previouslyWritten(*session, attempt_->revision)
        && baselines_.contains(attempt_->fileId) && baselines_.at(attempt_->fileId).destination
        && baselines_.at(attempt_->fileId).destination->bytes() == attempt_->serialized) {
        if (attempt_->parentOwned) {
            git::Request request;
            request.operation = git::Operation::CheckParentStaging;
            request.path = attempt_->path;
            launch(Step::ParentPreflight, request);
        } else {
            git::Request request;
            request.operation = git::Operation::StageFile;
            request.path = attempt_->path;
            launch(Step::Stage, request);
        }
        return;
    }
    auto evidence = std::make_shared<storage::PublicationResult>();
    const auto baseline = baselines_.at(attempt_->fileId).destination;
    const auto bytes = attempt_->serialized;
    const auto mode = attempt_->mode;
    QPointer<ResolutionPublisher> self(this);
    attempt_->step = Step::Publish;
    const auto id = coordinator_.submitMutation(attempt_->fileId, attempt_->revision,
        [evidence, baseline, bytes, mode](const TaskInput &, const TaskControl &control) {
            *evidence = storage::publishDestination(*baseline, bytes, mode, [&] { return control.beginMutation(); });
            TaskResult result;
            result.diagnostic = evidence->diagnostic;
            if (evidence->state == storage::PublicationState::Published && evidence->failure == storage::DestinationFailure::None
                && evidence->destination) result.status = TaskStatus::Succeeded;
            else result.status = TaskStatus::Failed;
            if (evidence->state == storage::PublicationState::Uncertain)
                result.diagnostic = evidence->diagnostic;
            return result;
        },
        [baseline, bytes](const TaskInput &) {
            const auto observed = storage::captureDestination(baseline->worktreeRoot(), baseline->path());
            if (!observed.destination) return MutationObservation::Uncertain;
            if (observed.destination->bytes() == bytes) return MutationObservation::ObservedChanged;
            if (observed.destination->bytes() == baseline->bytes()) return MutationObservation::ObservedUnchanged;
            return MutationObservation::Uncertain;
        },
        [self, evidence](const TaskCompletion &completion) {
            if (!self || !self->accept(completion)) return;
            if (evidence->state == storage::PublicationState::Published
                && evidence->failure == storage::DestinationFailure::None && evidence->destination)
                self->baselines_[self->attempt_->fileId].destination = evidence->destination;
            self->publication_ = evidence;
            self->published(completion);
        });
    if (!attempt_) return;
    attempt_->task = id;
    if (id == 0) fail(Outcome::Failed, tr("Publication could not start. The draft is still available."), preserveWrite());
}

void ResolutionPublisher::published(const TaskCompletion &completion)
{
    if (!attempt_) return;
    const auto published = publication_;
    const bool writerUncertain = published && (published->state == storage::PublicationState::Uncertain
        || (published->state == storage::PublicationState::Published && published->failure != storage::DestinationFailure::None));
    const auto uncertain = writerUncertain || completion.result.mutation == MutationObservation::Uncertain;
    if (completion.result.status != TaskStatus::Succeeded || uncertain || !published
        || published->state != storage::PublicationState::Published || !published->destination
        || (completion.result.mutation != MutationObservation::ObservedChanged
            && completion.result.mutation != MutationObservation::ObservedUnchanged)) {
        fail(uncertain ? Outcome::Uncertain : Outcome::Failed,
            completion.result.diagnostic.isEmpty()
                ? tr("Repository write failed. The saved draft is still available, and the file is unresolved.")
                : completion.result.diagnostic, false);
        return;
    }
    const auto recorded = files_.recordWrite(attempt_->fileId, attempt_->revision, Outcome::Succeeded);
    if (recorded != Transition::Applied) {
        clearAttempt();
        Q_EMIT changed(tr("The repository write could not be recorded. The draft is still available."));
        return;
    }
    if (attempt_->parentOwned) {
        git::Request request;
        request.operation = git::Operation::CheckParentStaging;
        request.path = attempt_->path;
        launch(Step::ParentPreflight, request);
        return;
    }
    git::Request request;
    request.operation = git::Operation::StageFile;
    request.path = attempt_->path;
    launch(Step::Stage, request);
}

void ResolutionPublisher::staged(const TaskCompletion &completion)
{
    if (!attempt_) return;
    // Parent-owned paths never reach git add. This callback is repository mode only.
    if (attempt_->parentOwned) {
        fail(Outcome::Failed, tr("Written, unresolved. This path is staged by Git, not by Branch Truce."), true);
        return;
    }
    const auto outcome = failedObservation(completion.result);
    if (completion.result.status != TaskStatus::Succeeded || outcome == Outcome::Uncertain) {
        fail(outcome, completion.result.diagnostic.isEmpty()
            ? tr("Written, unresolved.") : completion.result.diagnostic, true);
        return;
    }
    startVerify();
}

void ResolutionPublisher::parentPreflight(const TaskCompletion &completion)
{
    if (!attempt_ || !attempt_->prepared) return;
    if (completion.result.status != TaskStatus::Succeeded || !completion.result.gitResult
        || completion.result.gitResult->outcome != git::Outcome::Success) {
        fail(failedObservation(completion.result), completion.result.diagnostic.isEmpty()
            ? tr("Written, unresolved. Git cannot stage this path yet.") : completion.result.diagnostic, true);
        return;
    }
    Offer offer;
    offer.fileId = attempt_->fileId;
    offer.path = attempt_->path;
    offer.revision = attempt_->revision;
    offer.mode = attempt_->mode;
    offer.canonicalObjectId = attempt_->prepared->canonicalObjectId;
    offer.worktreeRoot = attempt_->prepared->worktreeRoot;
    offer.prepared = *attempt_->prepared;
    offer_ = std::move(offer);
    clearAttempt();
    Q_EMIT changed(tr("Written, unresolved. Waiting for Git to stage this path."));
}

void ResolutionPublisher::verified(const git::TextConversionResult &result)
{
    if (!attempt_ || attempt_->step != Step::Verify) return;
    if (result.outcome == git::Outcome::Success && result.text
        && result.text->canonicalObjectId == attempt_->prepared->canonicalObjectId
        && result.text->mode == attempt_->mode) {
        finishResolved();
        return;
    }
    fail(result.outcome == git::Outcome::TimedOut ? Outcome::Uncertain : Outcome::Failed,
        result.diagnostic.isEmpty() ? tr("Written, unresolved. The staged result does not match the reviewed revision.")
                                    : result.diagnostic, true);
}

void ResolutionPublisher::noteStale(const TaskCompletion &completion)
{
    if (!attempt_ || completion.stamp.id != attempt_->task) return;
    if (completion.disposition == TaskDisposition::StaleSession) {
        clearAttempt();
        return;
    }
    const auto outcome = failedObservation(completion.result);
    const bool wrote = completion.result.mutation == MutationObservation::ObservedChanged
        && attempt_->step == Step::Publish && completion.result.status == TaskStatus::Succeeded;
    if (wrote) {
        // Exchange already replaced the launch inode. Adopt the live destination
        // only while it still holds the bytes this publish wrote. Different bytes
        // keep the previous baseline, so a later resolution fails closed.
        auto live = storage::captureDestination(repository_.worktreeRoot, attempt_->path);
        if (live.destination && live.destination->bytes() == attempt_->serialized) {
            auto baseline = baselines_.find(attempt_->fileId);
            if (baseline == baselines_.end())
                baselines_.emplace(attempt_->fileId, Baseline{std::move(live.destination), QString{}});
            else {
                baseline->second.destination = std::move(live.destination);
                baseline->second.diagnostic.clear();
            }
        }
        const auto recorded = files_.recordWrite(attempt_->fileId, attempt_->revision, Outcome::Succeeded);
        clearAttempt();
        Q_EMIT changed(recorded == Transition::Applied
            ? tr("The reviewed revision was written, then the draft changed. It is not resolved.")
            : tr("The repository write finished after the draft changed."));
        return;
    }
    const bool stage = attempt_->step == Step::Stage || attempt_->step == Step::ParentPreflight
        || attempt_->step == Step::Verify || preserveWrite();
    fail(outcome == Outcome::Failed && completion.result.mutation == MutationObservation::NotAttempted
        ? Outcome::Failed : outcome,
        tr("The result changed before publication finished. The draft is still available."), stage && attempt_->step != Step::Publish);
}
}
