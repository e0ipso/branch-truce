#include "SessionCoordinator.h"
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QPromise>
#include <QPointer>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
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
        git::GitAdapter *adapter = nullptr;
    };

    State(SessionCoordinator &owner, SessionModel &model, QString executable)
        : owner(owner), model(model), executable(std::move(executable)) {}

    [[nodiscard]] bool owningThread() const
    {
        return QThread::currentThread() == owner.thread() && model.thread() == owner.thread();
    }
    [[nodiscard]] TaskDisposition disposition(const Job &job) const
    {
        const auto &stamp = job.input.stamp;
        if (stamp.generation != generation || stamp.worktree != worktree || stamp.operation != operation)
            return TaskDisposition::StaleSession;
        const auto *file = model.file(stamp.fileId);
        if (!file || file->current().id != stamp.inputRevision
            || file->current().result != stamp.resultRevision
            || file->current().decisions != stamp.decisionRevision) return TaskDisposition::StaleRevision;
        if (job.control.cancellationRequested()) return TaskDisposition::Cancelled;
        return TaskDisposition::Current;
    }
    TaskId enqueue(const std::string &fileId, RevisionId expected, bool mutation,
                   Work work, Inspect inspect, Completion completion, std::optional<git::Request> request = {})
    {
        if (!owningThread() || replacing || generation == 0 || nextId == 0
            || jobs.size() >= static_cast<std::size_t>(std::numeric_limits<int>::max())) return 0;
        const auto *file = model.file(fileId);
        if (!file || file->current().id != expected) return 0;
        const auto &revision = file->current();
        const auto id = nextId++;
        auto job = std::make_shared<Job>(Job{
            {{id, generation, worktree, operation, fileId, expected, revision.result, revision.decisions},
             file->path(), revision}, TaskControl(mutation), std::move(work), std::move(inspect),
            std::move(completion), mutation, false, std::move(request), nullptr});
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
                finish(job, {TaskStatus::Cancelled, MutationObservation::NotAttempted, {},
                             QStringLiteral("Task cancelled or superseded before execution"), {}});
                if (!alive) return;
                continue;
            }
            if (job->mutation && queues.at(job->input.stamp.worktree).front() != id) continue;
            job->running = true;
            if (job->gitRequest) startGit(job);
            else startWorker(job);
            if (!alive) return;
        }
    }
    static TaskResult execute(const std::shared_ptr<Job> &job)
    {
        TaskResult result;
        try {
            if (job->control.cancellationRequested()) result.status = TaskStatus::Cancelled;
            else result = job->work(job->input, job->control);
        } catch (...) {
            result.status = TaskStatus::Failed;
            result.diagnostic = QStringLiteral("Background task threw an exception");
        }
        result.mutation = MutationObservation::NotAttempted;
        if (job->mutation && !job->control.mutationStarted() && result.status == TaskStatus::Succeeded) {
            result.status = TaskStatus::Failed;
            result.diagnostic = QStringLiteral("Mutation did not reach admission");
        }
        if (job->control.mutationStarted()) {
            result.mutation = MutationObservation::Uncertain;
            try {
                const auto observed = job->inspect(job->input);
                if (observed != MutationObservation::NotAttempted) result.mutation = observed;
            } catch (...) {
                result.diagnostic += QStringLiteral("; mutation inspection failed, outcome uncertain");
            }
        }
        if (job->control.cancellationRequested()) result.status = TaskStatus::Cancelled;
        if (result.status == TaskStatus::Succeeded && result.mutation == MutationObservation::Uncertain) {
            result.status = TaskStatus::Failed;
            result.diagnostic += QStringLiteral("; mutation outcome could not be verified");
        }
        return result;
    }
    void startWorker(const std::shared_ptr<Job> &job)
    {
        auto promise = std::make_shared<QPromise<TaskResult>>();
        auto *watcher = new QFutureWatcher<TaskResult>(&owner);
        watcher->setFuture(promise->future());
        QObject::connect(watcher, &QFutureWatcher<TaskResult>::finished, &owner, [this, job, watcher] {
            const auto result = watcher->result();
            watcher->deleteLater();
            finish(job, result);
        });
        promise->start();
        QThreadPool::globalInstance()->start([job, promise] {
            promise->addResult(execute(job));
            promise->finish();
        });
    }
    void startGit(const std::shared_ptr<Job> &job)
    {
        auto *adapter = new git::GitAdapter(executable, &owner);
        job->adapter = adapter;
        QObject::connect(adapter, &git::GitAdapter::finished, &owner, [this, job, adapter](const git::Result &value) {
            TaskResult result;
            result.status = value.outcome == git::Outcome::Success ? TaskStatus::Succeeded
                : value.outcome == git::Outcome::Cancelled ? TaskStatus::Cancelled : TaskStatus::Failed;
            switch (value.mutation) {
            case git::Mutation::NotAttempted: result.mutation = MutationObservation::NotAttempted; break;
            case git::Mutation::ObservedUnchanged: result.mutation = MutationObservation::ObservedUnchanged; break;
            case git::Mutation::ObservedChanged: result.mutation = MutationObservation::ObservedChanged; break;
            case git::Mutation::Uncertain: result.mutation = MutationObservation::Uncertain; break;
            }
            result.data = value.standardOutput;
            result.diagnostic = value.diagnostic;
            result.gitResult = value;
            job->adapter = nullptr;
            adapter->deleteLater();
            finish(job, std::move(result));
        });
        if (!adapter->start(*job->gitRequest)) {
            job->adapter = nullptr;
            adapter->deleteLater();
            finish(job, {TaskStatus::Failed, MutationObservation::NotAttempted, {},
                         QStringLiteral("Git adapter rejected task admission"), {}});
        }
    }
    void finish(const std::shared_ptr<Job> &job, TaskResult result)
    {
        if (job->control.cancellationRequested()) result.status = TaskStatus::Cancelled;
        const auto id = job->input.stamp.id;
        if (jobs.erase(id) == 0) return;
        if (job->mutation) {
            auto queue = queues.find(job->input.stamp.worktree);
            std::erase(queue->second, id);
            if (queue->second.empty()) queues.erase(queue);
        }
        TaskCompletion completion{job->input.stamp, std::move(result), disposition(*job)};
        if (job->mutation) mutations.push_back(completion);
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
    SessionModel &model;
    const QString executable;
    quint64 generation = 0;
    TaskId nextId = 1;
    QString worktree;
    QString operation;
    bool scheduled = false;
    bool replacing = false;
    std::map<TaskId, std::shared_ptr<Job>> jobs;
    std::map<QString, std::deque<TaskId>> queues;
    std::vector<TaskCompletion> mutations;
};

SessionCoordinator::SessionCoordinator(SessionModel &model, QString gitExecutable, QObject *parent)
    : QObject(parent), d(std::make_unique<State>(*this, model, std::move(gitExecutable))) {}
SessionCoordinator::~SessionCoordinator()
{
    // No GUI-thread wait. Workers own values/control only; watcher connections
    // disappear with this QObject, and cannot call a destroyed coordinator.
    for (const auto &[id, job] : d->jobs) {
        (void)id;
        job->control.cancel();
    }
}

bool SessionCoordinator::replaceSession(const QString &worktree, const QString &operation, std::vector<FileSession> files)
{
    if (!d->owningThread() || d->replacing || operation.isEmpty()
        || d->generation == std::numeric_limits<quint64>::max()
        || files.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
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
    d->model.replaceFiles(std::move(files));
    d->replacing = false;
    Q_EMIT sessionChanged();
    return true;
}
quint64 SessionCoordinator::generation() const { return d->generation; }
int SessionCoordinator::pendingTasks() const { return static_cast<int>(d->jobs.size()); }
const std::vector<TaskCompletion> &SessionCoordinator::mutationOutcomes() const { return d->mutations; }
TaskId SessionCoordinator::submit(const std::string &fileId, RevisionId expected, Work work, Completion completion)
{
    if (!work) return 0;
    return d->enqueue(fileId, expected, false, std::move(work), {}, std::move(completion));
}
TaskId SessionCoordinator::submitMutation(const std::string &fileId, RevisionId expected, Work work,
                                          Inspect inspect, Completion completion)
{
    if (!work || !inspect) return 0;
    return d->enqueue(fileId, expected, true, std::move(work), std::move(inspect), std::move(completion));
}
TaskId SessionCoordinator::submitGit(const std::string &fileId, RevisionId expected, git::Request request,
                                     Completion completion)
{
    if (!d->owningThread() || d->replacing) return 0;
    const auto *file = d->model.file(fileId);
    const bool mutation = request.operation == git::Operation::StageFile;
    if (!file || (mutation && request.path.toUtf8() != QByteArray::fromStdString(file->path()))) return 0;
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
