// Resolution publishing writes a reviewed result and verifies its Git staging or parent handoff.
#include "publication/ResolutionPublisher.h"
#include "problems/ProblemText.h"
#include "recovery/SessionIdentity.h"

#include <QHash>
#include <QPointer>
#include <array>
#include <memory>
#include <utility>

namespace branch_truce {
namespace {
QString remainingMarkers(const MarkerScan &markers)
{
    const auto blocks = static_cast<int>(markers.blocks.size());
    const auto stray = static_cast<int>(markers.orphans.size());
    if (stray == 0) return ResolutionPublisher::tr("%n conflict-marker block(s) remain", nullptr, blocks);
    if (blocks == 0) return ResolutionPublisher::tr("%n stray conflict-marker line(s) remain", nullptr, stray);
    return ResolutionPublisher::tr("%1 and %2 remain")
        .arg(ResolutionPublisher::tr("%n conflict-marker block(s)", nullptr, blocks),
             ResolutionPublisher::tr("%n stray conflict-marker line(s)", nullptr, stray));
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
        && result.mutation != MutationObservation::NotAttempted)
        return Outcome::Uncertain;
    if (result.status == TaskStatus::Cancelled && result.mutation != MutationObservation::NotAttempted)
        return Outcome::Uncertain;
    return Outcome::Failed;
}
}

ResolutionPublisher::ResolutionPublisher(SessionStore &files, SessionCoordinator &coordinator,
                                         const git::GitRunners &git,
                                         std::shared_ptr<const storage::FileSystem> destinations, QObject *parent)
    : QObject(parent),
      files_(files),
      coordinator_(coordinator),
      destinations_(std::move(destinations)),
      conversion_(git, this)
{
    connect(&conversion_, &git::TextConversion::finished, this, [this](const git::TextConversionResult &result) {
        if (!attempt_) return;
        if (const auto handler = handling(attempt_->step).conversion) (this->*handler)(result);
    });
    connect(&coordinator_, &SessionCoordinator::sessionChanged, this, [this] {
        conversion_.cancel();
        attempt_.reset();
        baselines_.clear();
        parent_.clear();
        confirmation_.reset();
    });
    connect(&coordinator_, &SessionCoordinator::taskFinished, this, [this](const TaskCompletion &completion) {
        if (!attempt_ || completion.stamp.id != attempt_->task) return;
        if (completion.disposition == TaskDisposition::Current) return;
        noteStale(completion);
    });
}

ResolutionPublisher::Baselines
ResolutionPublisher::readBaselines(const QString &worktreeRoot, const std::vector<FileSession> &files,
                                   const std::shared_ptr<const storage::FileSystem> &destinations)
{
    Baselines baselines;
    for (const auto &session : files) {
        if ((session.support() != Support::Supported && session.support() != Support::LargeFile)
            || !session.snapshots())
            continue;
        Baseline baseline;
        const auto &launch = session.snapshots()->launchResult;
        const auto captured = storage::captureDestination(worktreeRoot, QByteArray::fromStdString(session.path()),
                                                          destinations, &launch.identity);
        if (!captured.destination)
            baseline.diagnostic = problem_text::text(captured.problem);
        else if (captured.destination->identity() != launch.identity
                 || captured.destination->bytes().toStdString() != launch.text.bytes) {
            baseline.diagnostic = tr("The repository file no longer matches the captured launch result.");
        } else
            baseline.destination = captured.destination;
        baselines.emplace(session.id(), std::move(baseline));
    }
    return baselines;
}

void ResolutionPublisher::adoptBaselines(const git::RepositoryContext &repository, Baselines baselines)
{
    repository_ = repository;
    baselines_ = std::move(baselines);
}

bool ResolutionPublisher::adoptCopiedLaunch(const std::string &fileId, const SnapshotFileIdentity &identity,
                                            const storage::DestinationCapture &live)
{
    const auto *session = file(fileId);
    if ((attempt_ && attempt_->fileId == fileId) || !session || session->support() != Support::Supported
        || !session->snapshots() || !live.destination || live.destination->identity() != identity
        || live.destination->bytes().toStdString() != session->snapshots()->launchResult.text.bytes)
        return false;
    baselines_.insert_or_assign(fileId, Baseline{live.destination, QString{}});
    return true;
}

bool ResolutionPublisher::assignParentStaging(const QByteArray &path)
{
    if (attempt_) return false;
    if (path.isEmpty()) {
        parent_.clear();
        Q_EMIT changed(tr("Parent staging is not assigned."));
        return true;
    }
    for (const auto &file : files_.files()) {
        const auto *session = &file;
        if (session->support() != Support::Supported || QByteArray::fromStdString(session->path()) != path) continue;
        parent_.assign(path);
        Q_EMIT changed(tr("Git will stage this path. Branch Truce will not stage it."));
        return true;
    }
    return false;
}

bool ResolutionPublisher::busy() const { return attempt_ != nullptr; }
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
    return parent_.offerFor(fileId);
}

const ResolutionPublisher::StepHandling &ResolutionPublisher::handling(Step step)
{
    // In the order of Step. Publish finishes in its own mutation callback.
    static const std::array<StepHandling, 8> steps{{
        {.conversion = &ResolutionPublisher::prepared},
        {.task = &ResolutionPublisher::discovered},
        {.task = &ResolutionPublisher::indexed},
        {},
        {.task = &ResolutionPublisher::staged, .afterWrite = true},
        {.task = &ResolutionPublisher::parentPreflight, .afterWrite = true},
        {.conversion = &ResolutionPublisher::verified, .afterWrite = true},
        {},
    }};
    return steps.at(static_cast<std::size_t>(step));
}

const FileSession *ResolutionPublisher::file(const std::string &id) const { return files_.file(id); }
bool ResolutionPublisher::parentOwns(const QByteArray &path) const { return parent_.owns(path); }
bool ResolutionPublisher::previouslyWritten(const FileSession &session, RevisionId revision) const
{
    return session.publication().writtenAt(revision);
}
bool ResolutionPublisher::preserveWrite() const
{
    const auto *session = attempt_ ? file(attempt_->fileId) : nullptr;
    return session && previouslyWritten(*session, attempt_->revision);
}

ResolutionPublisher::StartResult ResolutionPublisher::result(Transition transition, const QString &detail)
{
    return {transition, detail, {}};
}

void ResolutionPublisher::fail(Outcome outcome, const QString &detail, bool stage)
{
    if (!attempt_) return;
    const auto attempt = attempt_;
    const auto id = attempt_->fileId;
    const auto revision = attempt_->revision;
    auto recorded = stage ? files_.recordStage(id, revision, outcome, {}, {}, detail.toStdString())
                          : files_.recordWrite(id, revision, outcome, detail.toStdString());
    parent_.withdraw();
    recorded.then(this, [this, attempt, detail](Transition result) {
        if (attempt_ != attempt) return;
        clearAttempt();
        Q_EMIT changed(result == Transition::Applied
                           ? detail
                           : tr("The outcome could not be recorded. The draft is still available."));
    });
}

void ResolutionPublisher::finishResolved()
{
    if (!attempt_ || !attempt_->prepared) return;
    const auto attempt = attempt_;
    const auto id = attempt_->fileId;
    files_
        .recordStage(id, attempt_->revision, Outcome::Succeeded, attempt_->prepared->canonicalObjectId.toStdString(),
                     attempt_->prepared->mode.toStdString())
        .then(this, [this, attempt](Transition result) {
            if (attempt_ != attempt) return;
            const auto *current = file(attempt->fileId);
            const bool resolved = current && current->current().id == attempt->revision && current->facts().resolved;
            parent_.withdraw();
            clearAttempt();
            Q_EMIT changed(result != Transition::Applied
                               ? tr("Git has the reviewed result, but the resolution could not be recorded.")
                               : resolved
                               ? tr("Written and resolved.")
                               : tr("The reviewed revision was written, then the draft changed. It is not resolved."));
        });
}

void ResolutionPublisher::clearAttempt() { attempt_.reset(); }

bool ResolutionPublisher::launch(Step step, git::Request request)
{
    if (!attempt_) return false;
    attempt_->step = step;
    QPointer<ResolutionPublisher> self(this);
    const auto id = coordinator_.submitGit(attempt_->fileId, attempt_->revision, std::move(request),
                                           [self](const TaskCompletion &completion) {
                                               if (!self || !self->accept(completion)) return;
                                               if (const auto handler = handling(self->attempt_->step).task)
                                                   (self.data()->*handler)(completion);
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
    return attempt_ && completion.stamp.id == attempt_->task && completion.disposition == TaskDisposition::Current;
}

ResolutionPublisher::StartResult ResolutionPublisher::start(const std::string &fileId, RevisionId expected,
                                                            bool confirmUndecided)
{
    if (attempt_) return result(Transition::Blocked, tr("Mark resolved is already working on a file."));
    const auto *session = file(fileId);
    if (!session || session->current().id != expected)
        return result(Transition::StaleRevision, tr("The result changed. Mark resolved was not applied."));
    const auto uncertain = [&](const std::optional<OperationOutcome> &outcome) {
        return outcome && outcome->revision == expected && outcome->outcome == Outcome::Uncertain;
    };
    const auto &publication = session->publication();
    if (uncertain(publication.checkpointOutcome()) || uncertain(publication.writeOutcome())
        || uncertain(publication.stageOutcome()))
        return result(Transition::Blocked,
                      tr("Outcome uncertain. Inspect the repository file and index before retrying."));
    if (session->facts().resolved) return result(Transition::NoChange, tr("Written and resolved."));
    confirmation_.reset();
    attempt_ = std::make_shared<Attempt>();
    attempt_->generation = coordinator_.generation();
    attempt_->fileId = fileId;
    attempt_->revision = expected;
    auto review = files_.beginMarkResolved(fileId, expected, confirmUndecided);
    if (review.isFinished()) return reviewed(review.result());
    const auto attempt = attempt_;
    review.then(this, [this, attempt](Transition result) {
        if (attempt_ != attempt) return;
        const auto next = reviewed(result);
        Q_EMIT changed(next.detail);
        if (next.markers && next.markers->first())
            Q_EMIT markersBlocked(QString::fromStdString(attempt->fileId), attempt->revision, next.markers->count(),
                                  next.markers->first()->line + 1, next.detail);
    });
    return result(Transition::Applied, tr("Preparing the reviewed result. Waiting for durable recovery."));
}

ResolutionPublisher::StartResult ResolutionPublisher::reviewed(Transition review)
{
    if (!attempt_) return result(Transition::StaleRevision, {});
    const auto fileId = attempt_->fileId;
    const auto expected = attempt_->revision;
    const auto *session = file(fileId);
    if (!session || session->current().id != expected) {
        clearAttempt();
        return result(Transition::StaleRevision, tr("The result changed. Mark resolved was not applied."));
    }
    if (review == Transition::ConfirmationRequired) {
        confirmation_ = std::make_pair(fileId, expected);
        clearAttempt();
        return result(Transition::ConfirmationRequired,
                      tr("Confirm that this result is intentional. Some source choices are still undecided."));
    }
    session = file(fileId);
    if (review == Transition::Blocked && session && session->current().draft.unresolvedMarkers != 0) {
        auto markers = session->markers();
        const auto first = markers.first();
        if (!first) {
            clearAttempt();
            return result(review, tr("Mark resolved is blocked. The repository file is unchanged."));
        }
        auto refused = result(review,
                              tr("Mark resolved is blocked: %1; first at line %2. The repository file is unchanged.")
                                  .arg(remainingMarkers(markers))
                                  .arg(static_cast<qulonglong>(first->line) + 1));
        refused.markers = std::move(markers);
        clearAttempt();
        return refused;
    }
    if (review != Transition::Applied) {
        clearAttempt();
        return result(review, tr("Mark resolved is blocked. The repository file is unchanged."));
    }
    const auto attempt = attempt_;
    files_.recordCheckpoint(fileId, expected, Outcome::Succeeded).then(this, [this, attempt](Transition saved) {
        if (attempt_ != attempt) return;
        if (saved != Transition::Applied && saved != Transition::NoChange) {
            clearAttempt();
            Q_EMIT changed(
                tr("Draft save failed. The edited result is still available, and the repository file is unchanged."));
            return;
        }
        preparePublication();
    });
    return result(Transition::Applied, tr("Publishing the reviewed result."));
}

void ResolutionPublisher::preparePublication()
{
    if (!attempt_) return;
    const auto fileId = attempt_->fileId;
    const auto expected = attempt_->revision;
    const auto *session = file(fileId);
    if (!session || session->current().id != expected || !session->snapshots()) {
        fail(Outcome::Failed, tr("Repository write failed. The saved draft is still available."), false);
        return;
    }
    const auto serialized = storage::serializeText(QByteArray::fromStdString(session->current().draft.text),
                                                   session->snapshots()->launchResult.text.format);
    if (!serialized.document) {
        fail(Outcome::Failed,
             serialized.problem.isEmpty() ? tr("Repository write failed. The saved draft is still available.")
                                          : problem_text::text(serialized.problem),
             false);
        return;
    }
    attempt_->path = QByteArray::fromStdString(session->path());
    attempt_->mode = QByteArray::fromStdString(session->snapshots()->resultMode);
    attempt_->serialized = serialized.document->originalBytes;
    attempt_->parentOwned = parentOwns(attempt_->path);
    parent_.withdraw();
    const auto baseline = baselines_.find(fileId);
    if (baseline != baselines_.end() && baseline->second.destination) {
        startPrepare();
        return;
    }
    if (!previouslyWritten(*session, expected)) {
        fail(Outcome::Failed,
             baseline == baselines_.end() || baseline->second.diagnostic.isEmpty()
                 ? tr("Repository write failed. No launch destination is available, and the file is unresolved.")
                 : baseline->second.diagnostic,
             false);
        return;
    }
    inspectDestination([this](storage::DestinationCapture live) {
        if (!live.destination || live.destination->bytes() != attempt_->serialized) {
            fail(Outcome::Failed,
                 live.problem.isEmpty() ? tr("The repository file no longer matches the written revision.")
                                        : problem_text::text(live.problem),
                 false);
            return;
        }
        baselines_.insert_or_assign(attempt_->fileId, Baseline{live.destination, {}});
        startPrepare();
    });
}

void ResolutionPublisher::inspectDestination(std::function<void(storage::DestinationCapture)> inspected)
{
    if (!attempt_) return;
    auto capture = std::make_shared<storage::DestinationCapture>();
    QPointer<ResolutionPublisher> self(this);
    attempt_->step = Step::Inspect;
    attempt_->task = coordinator_.submit(
        attempt_->fileId, attempt_->revision,
        [capture, root = repository_.worktreeRoot, path = attempt_->path, files = destinations_](const TaskInput &,
                                                                                                 const TaskControl &) {
            *capture = storage::captureDestination(root, path, files);
            return TaskResult{};
        },
        [self, capture, inspected = std::move(inspected)](const TaskCompletion &completion) {
            if (!self || !self->accept(completion)) return;
            if (completion.result.status != TaskStatus::Succeeded) {
                self->fail(Outcome::Failed, problem_text::text(completion.result.problem), self->preserveWrite());
                return;
            }
            inspected(*capture);
        });
    if (attempt_->task == 0)
        fail(Outcome::Failed, tr("Publication could not start. The draft is still available."), preserveWrite());
}

void ResolutionPublisher::cancelConfirmation()
{
    if (!confirmation_) return;
    confirmation_.reset();
    Q_EMIT changed(tr("Resolution cancelled. The text and choices are unchanged."));
}

bool ResolutionPublisher::verifyParentAcknowledgment(const std::string &fileId, RevisionId expected)
{
    const auto offer = parent_.offerFor(fileId);
    if (attempt_ || !offer || offer->revision != expected) return false;
    const auto *session = file(fileId);
    if (!session || session->current().id != expected || !previouslyWritten(*session, expected)) return false;
    if (!parent_.owns(offer->path)) return false;
    attempt_ = std::make_shared<Attempt>();
    attempt_->generation = coordinator_.generation();
    attempt_->fileId = fileId;
    attempt_->revision = expected;
    attempt_->path = offer->path;
    attempt_->mode = offer->mode;
    attempt_->parentOwned = true;
    attempt_->acknowledging = true;
    attempt_->prepared = offer->prepared;
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
    request.launchResultFormat = session->snapshots()->launchResult.text.format;
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
        || result.text->mode != attempt_->mode
        || result.text->document.utf8.toStdString() != session->current().draft.text
        || !samePolicy(session->snapshots()->conversion, result.text->policy)) {
        const auto uncertain = result.outcome == git::Outcome::TimedOut;
        fail(uncertain ? Outcome::Uncertain : Outcome::Failed,
             result.problem.isEmpty()
                 ? tr("Repository write failed. The saved draft is still available, and the file is unresolved.")
                 : problem_text::text(result.problem),
             preserveWrite());
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
        fail(failedObservation(completion.result),
             tr("The repository operation could not be validated. Nothing was published."), preserveWrite());
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
        fail(failedObservation(completion.result), tr("The index could not be inspected. Nothing new was staged."),
             preserveWrite());
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
    const bool conflict = matchesCapturedConflict(entries, *session->snapshots());
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
        fail(Outcome::Failed, tr("The index no longer matches this conflict. The repository file was not published."),
             preserveWrite());
        return;
    }
    inspectDestination([this](storage::DestinationCapture capture) { inspectedIndex(std::move(capture)); });
}

bool ResolutionPublisher::matchesWrittenBaseline(const FileSession &session, const Attempt &attempt) const
{
    if (!previouslyWritten(session, attempt.revision) || !baselines_.contains(attempt.fileId)) return false;
    const auto &destination = baselines_.at(attempt.fileId).destination;
    return destination && destination->bytes() == attempt.serialized;
}

void ResolutionPublisher::inspectedIndex(storage::DestinationCapture current)
{
    if (!attempt_) return;
    const auto *session = file(attempt_->fileId);
    if (!session || !session->snapshots()) return;
    const bool written = matchesWrittenBaseline(*session, *attempt_);
    if (written && !attempt_->parentOwned
        && (!current.destination || current.destination->bytes() != attempt_->serialized)) {
        fail(Outcome::Failed,
             current.problem.isEmpty() ? tr("The repository file no longer matches the written revision.")
                                       : problem_text::text(current.problem),
             false);
        return;
    }
    if (written && current.destination && current.destination->bytes() == attempt_->serialized) {
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
    // Git mergetool moves the worktree file aside and copies it back before the
    // tool runs. The launch inode is then the backup. Publish onto the copy when
    // its bytes are still the captured launch bytes; a different file is refused.
    // A failed exit moves that backup back over a written file, so the launch
    // bytes are accepted after a write as well.
    if (attempt_->parentOwned) {
        auto live = current;
        auto baseline = baselines_.find(attempt_->fileId);
        if (!live.destination || baseline == baselines_.end() || !baseline->second.destination
            || (live.destination->bytes() != baseline->second.destination->bytes()
                && live.destination->bytes().toStdString() != session->snapshots()->launchResult.text.bytes)) {
            fail(Outcome::Failed,
                 live.problem.isEmpty()
                     ? tr("The repository file changed while Git was waiting. The draft was not published.")
                     : problem_text::text(live.problem),
                 false);
            return;
        }
        baseline->second.destination = std::move(live.destination);
        baseline->second.diagnostic.clear();
    }
    auto evidence = std::make_shared<storage::PublicationResult>();
    const auto baseline = baselines_.at(attempt_->fileId).destination;
    const auto bytes = attempt_->serialized;
    const auto mode = attempt_->mode;
    QPointer<ResolutionPublisher> self(this);
    attempt_->step = Step::Publish;
    attempt_->published = evidence;
    const auto id = coordinator_.submitMutation(
        attempt_->fileId, attempt_->revision,
        [evidence, baseline, bytes, mode](const TaskInput &, const TaskControl &control) {
            *evidence = storage::publishDestination(*baseline, bytes, mode, [&] { return control.beginMutation(); });
            TaskResult result;
            result.problem = evidence->problem;
            if (evidence->state == storage::PublicationState::Published
                && evidence->failure == storage::DestinationFailure::None && evidence->destination)
                result.status = TaskStatus::Succeeded;
            else
                result.status = TaskStatus::Failed;
            return result;
        },
        [baseline, bytes, files = destinations_](const TaskInput &) {
            const auto observed = storage::captureDestination(baseline->worktreeRoot(), baseline->path(), files);
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
            self->published(completion, *evidence);
        });
    if (!attempt_) return;
    attempt_->task = id;
    if (id == 0)
        fail(Outcome::Failed, tr("Publication could not start. The draft is still available."), preserveWrite());
}

void ResolutionPublisher::published(const TaskCompletion &completion, const storage::PublicationResult &published)
{
    if (!attempt_) return;
    const bool writerUncertain = published.state == storage::PublicationState::Uncertain
        || (published.state == storage::PublicationState::Published
            && published.failure != storage::DestinationFailure::None);
    const auto uncertain = writerUncertain || completion.result.mutation == MutationObservation::Uncertain;
    if (completion.result.status != TaskStatus::Succeeded || uncertain
        || published.state != storage::PublicationState::Published || !published.destination
        || (completion.result.mutation != MutationObservation::ObservedChanged
            && completion.result.mutation != MutationObservation::ObservedUnchanged)) {
        fail(uncertain ? Outcome::Uncertain : Outcome::Failed,
             completion.result.problem.isEmpty()
                 ? tr("Repository write failed. The saved draft is still available, and the file is unresolved.")
                 : problem_text::text(completion.result.problem),
             false);
        return;
    }
    const auto attempt = attempt_;
    const auto id = attempt_->fileId;
    files_.recordWrite(id, attempt_->revision, Outcome::Succeeded).then(this, [this, attempt](Transition recorded) {
        if (attempt_ != attempt) return;
        if (recorded != Transition::Applied) {
            clearAttempt();
            Q_EMIT changed(tr("The repository write could not be recorded. The draft is still available."));
            return;
        }
        git::Request request;
        request.operation = attempt_->parentOwned ? git::Operation::CheckParentStaging : git::Operation::StageFile;
        request.path = attempt_->path;
        launch(attempt_->parentOwned ? Step::ParentPreflight : Step::Stage, request);
    });
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
        fail(outcome,
             completion.result.problem.isEmpty() ? tr("Written, unresolved.")
                                                 : problem_text::text(completion.result.problem),
             true);
        return;
    }
    startVerify();
}

void ResolutionPublisher::parentPreflight(const TaskCompletion &completion)
{
    if (!attempt_ || !attempt_->prepared) return;
    if (completion.result.status != TaskStatus::Succeeded || !completion.result.gitResult
        || completion.result.gitResult->outcome != git::Outcome::Success) {
        fail(failedObservation(completion.result),
             completion.result.problem.isEmpty() ? tr("Written, unresolved. Git cannot stage this path yet.")
                                                 : problem_text::text(completion.result.problem),
             true);
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
    parent_.offer(std::move(offer));
    clearAttempt();
    Q_EMIT changed(tr("Written, unresolved. Waiting for Git to stage this path."));
}

void ResolutionPublisher::verified(const git::TextConversionResult &result)
{
    if (!attempt_ || attempt_->step != Step::Verify) return;
    if (result.outcome == git::Outcome::Success && result.text && attempt_->prepared
        && result.text->canonicalObjectId == attempt_->prepared->canonicalObjectId
        && result.text->mode == attempt_->mode) {
        finishResolved();
        return;
    }
    fail(result.outcome == git::Outcome::TimedOut ? Outcome::Uncertain : Outcome::Failed,
         result.problem.isEmpty() ? tr("Written, unresolved. The staged result does not match the reviewed revision.")
                                  : problem_text::text(result.problem),
         true);
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
        // Exchange already replaced the launch inode. Adopt the destination the
        // writer saw only while it holds the bytes this publish wrote, which the
        // worker's inspection observed. Different bytes keep the previous
        // baseline, so a later resolution fails closed.
        const auto &live = attempt_->published->destination;
        if (live && live->bytes() == attempt_->serialized)
            baselines_.insert_or_assign(attempt_->fileId, Baseline{live, QString{}});
        const auto attempt = attempt_;
        const auto id = attempt_->fileId;
        files_.recordWrite(id, attempt_->revision, Outcome::Succeeded).then(this, [this, attempt](Transition recorded) {
            if (attempt_ != attempt) return;
            clearAttempt();
            Q_EMIT changed(recorded == Transition::Applied
                               ? tr("The reviewed revision was written, then the draft changed. It is not resolved.")
                               : tr("The repository write finished after the draft changed."));
        });
        return;
    }
    const bool stage = handling(attempt_->step).afterWrite || preserveWrite();
    fail(outcome == Outcome::Failed && completion.result.mutation == MutationObservation::NotAttempted ? Outcome::Failed
                                                                                                       : outcome,
         tr("The result changed before publication finished. The draft is still available."),
         stage && attempt_->step != Step::Publish);
}
}
