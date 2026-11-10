// The session coordinator schedules guarded save and publication work and reconciles cancelled mutations.
#include "session/SessionCoordinator.h"
#include "problems/ProblemText.h"
#include "recovery/RecoveryRestore.h"
#include "storage/OffThread.h"
#include <QDir>
#include <QFileInfo>
#include <QFuture>
#include <QPointer>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <atomic>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace branch_truce {
namespace {
constexpr unsigned cancelled = 1;
constexpr unsigned mutating = 2;
}

struct TaskControl::State {
    explicit State(bool allowed) : mutationAllowed(allowed) {}
    const bool mutationAllowed;
    std::atomic<unsigned> flags{0};
};
TaskControl::TaskControl(bool mutation) : state_(std::make_shared<State>(mutation)) {}
bool TaskControl::cancellationRequested() const { return (state_->flags.load() & cancelled) != 0; }
void TaskControl::cancel() const { state_->flags.fetch_or(cancelled); }
bool TaskControl::mutationStarted() const { return (state_->flags.load() & mutating) != 0; }
bool TaskControl::beginMutation() const
{
    if (!state_->mutationAllowed) return false;
    auto flags = state_->flags.load();
    do {
        if ((flags & cancelled) != 0) return false;
    } while (!state_->flags.compare_exchange_weak(flags, flags | mutating));
    return true;
}

class SessionCoordinator::State {
public:
    struct Job {
        TaskInput input;
        TaskControl control;
        Work work;
        Inspect inspect;
        Completion completion;
        bool mutation;
        bool running = false;
        std::optional<git::Request> gitRequest;
        git::GitRunner *adapter = nullptr;
        // The store the intent went to, held until the observation is recorded there too.
        std::shared_ptr<storage::RecoveryLease> recoveryLease;
        std::optional<MutationFact> fact;
        std::optional<FileSession> mutationTarget;
        QFuture<TaskResult> future;
        bool observationFailed = false;
    };

    State(SessionCoordinator &owner, SessionStore &store, git::GitRunners git, QThreadPool &workers)
        : owner(owner), store(store), git(std::move(git)), workers(workers)
    {}

    [[nodiscard]] bool owningThread() const
    {
        return QThread::currentThread() == owner.thread() && store.thread() == owner.thread();
    }
    [[nodiscard]] TaskDisposition disposition(const TaskStamp &stamp) const
    {
        if (stamp.generation != generation || stamp.worktree != worktree || stamp.operation != operation)
            return TaskDisposition::StaleSession;
        const auto *file = store.file(stamp.fileId);
        if (!file || file->current().id != stamp.inputRevision || file->current().result != stamp.resultRevision
            || file->current().decisions != stamp.decisionRevision)
            return TaskDisposition::StaleRevision;
        return TaskDisposition::Current;
    }
    [[nodiscard]] TaskDisposition disposition(const Job &job) const
    {
        const auto current = disposition(job.input.stamp);
        if (current != TaskDisposition::Current) return current;
        if (job.control.cancellationRequested()) return TaskDisposition::Cancelled;
        return TaskDisposition::Current;
    }
    TaskId enqueue(const std::string &fileId, RevisionId expected, bool mutation, Work work, Inspect inspect,
                   Completion completion, std::optional<git::Request> request = {})
    {
        if (!owningThread() || replacing || generation == 0 || nextId == 0
            || jobs.size() >= static_cast<std::size_t>(std::numeric_limits<int>::max()))
            return 0;
        const auto *file = store.file(fileId);
        if (!file || file->current().id != expected) return 0;
        const auto &revision = file->current();
        const auto id = nextId++;
        auto job = std::make_shared<Job>(
            Job{{{id, generation, worktree, operation, fileId, expected, revision.result, revision.decisions},
                 file->path(),
                 revision},
                TaskControl(mutation),
                std::move(work),
                std::move(inspect),
                std::move(completion),
                mutation,
                false,
                std::move(request),
                nullptr,
                {},
                {},
                {},
                {}});
        jobs.emplace(id, job);
        if (mutation) queues[worktree].push_back(id);
        schedule();
        Q_EMIT owner.pendingTasksChanged();
        return id;
    }
    void schedule()
    {
        if (scheduled) return;
        scheduled = true;
        QTimer::singleShot(0, &owner, [this] {
            scheduled = false;
            dispatch();
        });
    }
    void dispatch()
    {
        const QPointer<SessionCoordinator> alive(&owner);
        // Callbacks can enqueue, cancel or replace a session. Traverse a value
        // copy and recheck membership before starting any work.
        std::vector<std::shared_ptr<Job>> pending;
        for (const auto &[id, job] : jobs) {
            (void)id;
            if (!job->running) pending.push_back(job);
        }
        for (const auto &job : pending) {
            const auto id = job->input.stamp.id;
            if (!jobs.contains(id) || job->running) continue;
            if (disposition(*job) != TaskDisposition::Current) {
                finish(job,
                       {TaskStatus::Cancelled, MutationObservation::NotAttempted, {}, ProblemCode::TaskSuperseded, {}});
                if (!alive) return;
                continue;
            }
            if (job->mutation && queues.at(job->input.stamp.worktree).front() != id) continue;
            if (!job->mutation && store.recording()) continue;
            job->running = true;
            if (job->mutation) {
                store.recordTyping();
                const auto *file = store.file(job->input.stamp.fileId);
                if (!file || store.hasPendingRecovery() || store.hasRecoveryFailure(file->id())) {
                    finish(job,
                           {TaskStatus::Failed,
                            MutationObservation::NotAttempted,
                            {},
                            ProblemCode::TaskRecoveryBlocked,
                            {}});
                    if (!alive) return;
                    continue;
                }
                const MutationFact intent{QUuid::createUuid().toString(QUuid::WithoutBraces),
                                          job->gitRequest ? MutationKind::Staging : MutationKind::Publication,
                                          {}};
                job->mutationTarget = *file;
                store.commitRecord(mutationRecoveryTransaction(*file, intent))
                    .then(&owner, [this, job, intent](const RecoveryJournal::Receipt &receipt) {
                        const auto &committed = receipt.commit;
                        if (!committed.succeeded) {
                            if (disposition(job->input.stamp) == TaskDisposition::Current)
                                store.recordRecoveryFailure(job->input.stamp.fileId, committed.problem);
                            finish(job,
                                   {TaskStatus::Failed, MutationObservation::NotAttempted, {}, committed.problem, {}});
                            return;
                        }
                        job->recoveryLease = receipt.lease;
                        job->fact = intent;
                        if (disposition(*job) != TaskDisposition::Current) {
                            observeAndFinish(job,
                                             {TaskStatus::Cancelled,
                                              MutationObservation::NotAttempted,
                                              {},
                                              ProblemCode::TaskSuperseded,
                                              {}});
                            return;
                        }
                        start(job);
                    });
            } else {
                start(job);
            }
            if (!alive) return;
        }
    }
    void start(const std::shared_ptr<Job> &job)
    {
        if (job->gitRequest)
            startGit(job);
        else
            startWorker(job);
    }
    static TaskResult execute(const std::shared_ptr<Job> &job)
    {
        TaskResult result;
        try {
            if (job->control.cancellationRequested())
                result.status = TaskStatus::Cancelled;
            else
                result = job->work(job->input, job->control);
        } catch (...) {
            result.status = TaskStatus::Failed;
            result.problem = ProblemCode::TaskThrew;
        }
        result.mutation = MutationObservation::NotAttempted;
        if (job->mutation && !job->control.mutationStarted() && result.status == TaskStatus::Succeeded) {
            result.status = TaskStatus::Failed;
            result.problem = ProblemCode::TaskNotAdmitted;
        }
        if (job->control.mutationStarted()) {
            result.mutation = MutationObservation::Uncertain;
            try {
                const auto observed = job->inspect(job->input);
                if (observed != MutationObservation::NotAttempted) result.mutation = observed;
            } catch (...) {
                result.problem = Problem{ProblemCode::TaskInspectionFailed}.wrapping(result.problem);
            }
        }
        if (job->control.cancellationRequested()) result.status = TaskStatus::Cancelled;
        if (result.status == TaskStatus::Succeeded && result.mutation == MutationObservation::Uncertain) {
            result.status = TaskStatus::Failed;
            result.problem = Problem{ProblemCode::TaskUnverified}.wrapping(result.problem);
        }
        return result;
    }
    void startWorker(const std::shared_ptr<Job> &job)
    {
        job->future = runOffThread(
            workers, &owner,
            [job] {
                auto result = execute(job);
                recordObservation(*job, result);
                return result;
            },
            [this, job](const TaskResult &result) { finish(job, result); });
    }
    void startGit(const std::shared_ptr<Job> &job)
    {
        // The coordinator owns the runner as its child until the result is in.
        auto *adapter = git(&owner).release();
        job->adapter = adapter;
        QObject::connect(adapter, &git::GitRunner::finished, &owner, [this, job, adapter](const git::Result &value) {
            TaskResult result;
            result.status = value.outcome == git::Outcome::Success ? TaskStatus::Succeeded
                : value.outcome == git::Outcome::Cancelled         ? TaskStatus::Cancelled
                                                                   : TaskStatus::Failed;
            switch (value.mutation) {
            case git::Mutation::NotAttempted: result.mutation = MutationObservation::NotAttempted; break;
            case git::Mutation::ObservedUnchanged: result.mutation = MutationObservation::ObservedUnchanged; break;
            case git::Mutation::ObservedChanged: result.mutation = MutationObservation::ObservedChanged; break;
            case git::Mutation::Uncertain: result.mutation = MutationObservation::Uncertain; break;
            }
            result.data = value.standardOutput;
            result.problem = value.problem;
            result.gitResult = value;
            job->adapter = nullptr;
            adapter->deleteLater();
            observeAndFinish(job, std::move(result));
        });
        if (!adapter->start(*job->gitRequest)) {
            job->adapter = nullptr;
            adapter->deleteLater();
            observeAndFinish(
                job, {TaskStatus::Failed, MutationObservation::NotAttempted, {}, ProblemCode::GitStartRefused, {}});
        }
    }
    // Records an admitted mutation's observation under the lease its intent
    // was committed under, even when the session has since moved to another
    // store. A failed record makes the result a failure with an uncertain outcome.
    static void recordObservation(Job &job, TaskResult &result)
    {
        if (!job.mutationTarget || !job.fact || !job.recoveryLease) return;
        storage::RecoveryCommit committed;
        try {
            auto observed = *job.fact;
            observed.observation = result.mutation;
            committed = job.recoveryLease->commit(mutationRecoveryTransaction(*job.mutationTarget, observed));
        } catch (...) {
            committed = {false, ProblemCode::RecoveryCommitFailed};
        }
        if (committed.succeeded) return;
        result.status = TaskStatus::Failed;
        if (result.mutation != MutationObservation::NotAttempted) result.mutation = MutationObservation::Uncertain;
        result.problem = Problem{ProblemCode::TaskOutcomeNotRetained, {problem_text::text(committed.problem)}}.wrapping(
            result.problem);
        job.observationFailed = true;
    }
    void observeAndFinish(const std::shared_ptr<Job> &job, TaskResult result)
    {
        job->future = runOffThread(
            workers, &owner,
            [job, result = std::move(result)]() mutable {
                recordObservation(*job, result);
                return result;
            },
            [this, job](const TaskResult &observed) { finish(job, observed); });
    }
    void finish(const std::shared_ptr<Job> &job, TaskResult result)
    {
        if (job->control.cancellationRequested()) result.status = TaskStatus::Cancelled;
        if (job->observationFailed && job->input.stamp.worktree == worktree
            && job->recoveryLease == store.recoveryLease())
            store.recordRecoveryFailure(job->input.stamp.fileId, result.problem);
        const auto id = job->input.stamp.id;
        if (jobs.erase(id) == 0) return;
        if (job->mutation) {
            auto queue = queues.find(job->input.stamp.worktree);
            std::erase(queue->second, id);
            if (queue->second.empty()) queues.erase(queue);
        }
        TaskCompletion completion{job->input.stamp, std::move(result), disposition(*job)};
        schedule();
        // Apply before emitting any notifications that could replace the
        // session or edit a revision. Discarded mutation evidence stays above.
        const QPointer<SessionCoordinator> alive(&owner);
        if (completion.disposition == TaskDisposition::Current && job->completion) job->completion(completion);
        if (!alive) return;
        Q_EMIT alive->taskFinished(completion);
        if (alive) Q_EMIT alive->pendingTasksChanged();
    }

    SessionCoordinator &owner;
    SessionStore &store;
    const git::GitRunners git;
    QThreadPool &workers;
    quint64 generation = 0;
    TaskId nextId = 1;
    QString worktree;
    QString operation;
    bool scheduled = false;
    bool replacing = false;
    std::map<TaskId, std::shared_ptr<Job>> jobs;
    std::map<QString, std::deque<TaskId>> queues;
};

SessionCoordinator::SessionCoordinator(SessionStore &store, git::GitRunners git, QThreadPool &workers, QObject *parent)
    : QObject(parent), d(std::make_unique<State>(*this, store, std::move(git), workers))
{
    connect(&store, &SessionStore::fileChanged, this, [this](int) { d->schedule(); });
}
SessionCoordinator::~SessionCoordinator()
{
    // Cancellation refuses work not yet admitted. An admitted worker finishes
    // mutation, inspection and durable observation before its own future ends.
    // Drain only these jobs, never the supplied pool or unrelated work. Shutdown
    // has no artificial deadline; interrupted Git intents reconcile on restart.
    for (const auto &[id, job] : d->jobs) {
        (void)id;
        job->control.cancel();
        if (job->adapter) {
            QObject::disconnect(job->adapter, nullptr, this, nullptr);
            job->adapter->cancel();
        }
    }
    for (const auto &[id, job] : d->jobs) {
        (void)id;
        if (job->future.isValid()) job->future.waitForFinished();
    }
}

bool SessionCoordinator::replaceSession(const QString &worktree, const QString &operation,
                                        std::vector<FileSession> files)
{
    return replaceSession(worktree, operation, std::move(files), false);
}

bool SessionCoordinator::completeRecoveryLoad(const TaskStamp &stamp, std::vector<FileSession> observed,
                                              RecoveryRestore restore)
{
    if (!d->owningThread() || d->replacing || d->disposition(stamp) != TaskDisposition::Current
        || !d->store.canRestoreLoad())
        return false;
    const bool recovered = restore.found;
    auto files = recovered ? std::move(restore.files) : std::move(observed);
    if (!replaceSession(d->worktree, d->operation, std::move(files), true, recovered ? &restore : nullptr))
        return false;
    // Inventory diagnostics describe stores we did not adopt. Only an adopted
    // store's per-file failures may block the loaded files' recovery.
    return true;
}

bool SessionCoordinator::replaceSession(const QString &worktree, const QString &operation,
                                        std::vector<FileSession> files, bool completingLoad, RecoveryRestore *restore)
{
    if (!d->owningThread() || d->replacing) return false;
    // Typed edits reach the journal before the session can move on, and a
    // failed one keeps it here as pending input.
    d->store.recordTyping();
    if (((!completingLoad && d->store.hasPendingRecovery()) || d->store.recording()) || operation.isEmpty()
        || d->generation == std::numeric_limits<quint64>::max()
        || files.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return false;
    const QFileInfo root(worktree);
    const auto canonical = root.canonicalFilePath();
    if (!root.isDir() || canonical.isEmpty()) return false;
    std::set<std::string> ids;
    for (const auto &file : files) {
        if (file.id().empty() || !ids.insert(file.id()).second) return false;
    }
    d->replacing = true;
    ++d->generation;
    d->worktree = QDir::cleanPath(canonical);
    d->operation = operation;
    cancelAll();
    d->store.replaceFiles(std::move(files), restore);
    d->replacing = false;
    Q_EMIT sessionChanged();
    return true;
}
quint64 SessionCoordinator::generation() const { return d->generation; }
int SessionCoordinator::pendingTasks() const { return static_cast<int>(d->jobs.size()); }
TaskId SessionCoordinator::submit(const std::string &fileId, RevisionId expected, Work work, Completion completion)
{
    if (!work) return 0;
    return d->enqueue(fileId, expected, false, std::move(work), {}, std::move(completion));
}
TaskId SessionCoordinator::submitMutation(const std::string &fileId, RevisionId expected, Work work, Inspect inspect,
                                          Completion completion)
{
    if (!work || !inspect) return 0;
    return d->enqueue(fileId, expected, true, std::move(work), std::move(inspect), std::move(completion));
}
TaskId SessionCoordinator::submitGit(const std::string &fileId, RevisionId expected, git::Request request,
                                     Completion completion)
{
    if (!d->owningThread() || d->replacing) return 0;
    const auto *file = d->store.file(fileId);
    const bool mutation = git::isMutation(request.operation);
    if (!file || (mutation && request.path != QByteArray::fromStdString(file->path()))) return 0;
    request.directory = d->worktree;
    request.sessionGeneration = d->generation;
    request.inputRevision = expected;
    return d->enqueue(fileId, expected, mutation, {}, {}, std::move(completion), std::move(request));
}
bool SessionCoordinator::cancel(TaskId id)
{
    if (!d->owningThread()) return false;
    const auto found = d->jobs.find(id);
    if (found == d->jobs.end()) return false;
    const auto job = found->second;
    job->control.cancel();
    if (job->adapter) job->adapter->cancel();
    d->schedule();
    return true;
}
void SessionCoordinator::cancelAll()
{
    if (!d->owningThread()) return;
    std::vector<TaskId> ids;
    for (const auto &[id, job] : d->jobs) {
        (void)job;
        ids.push_back(id);
    }
    for (const auto id : ids) cancel(id);
}
}
