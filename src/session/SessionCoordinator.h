// The session coordinator schedules guarded save and publication work and reconciles cancelled mutations.
#pragma once

#include "app/SessionModel.h"
#include "git/GitAdapter.h"
#include <QByteArray>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>
#include <optional>

namespace branch_truce {

using TaskId = quint64;
enum class TaskStatus { Succeeded, Failed, Cancelled };
enum class MutationObservation { NotAttempted, ObservedUnchanged, ObservedChanged, Uncertain };
enum class TaskDisposition { Current, Cancelled, StaleSession, StaleRevision };

struct TaskStamp {
    TaskId id;
    quint64 generation;
    QString worktree;
    QString operation;
    std::string fileId;
    RevisionId inputRevision;
    RevisionId resultRevision;
    RevisionId decisionRevision;
};
struct TaskInput {
    TaskStamp stamp;
    std::string path;
    Revision revision;
};
struct TaskResult {
    TaskStatus status = TaskStatus::Succeeded;
    MutationObservation mutation = MutationObservation::NotAttempted;
    QByteArray data;
    QString diagnostic;
    std::optional<git::Result> gitResult;
};
struct TaskCompletion {
    TaskStamp stamp;
    TaskResult result;
    TaskDisposition disposition;
};

// Copyable worker-side control. A mutating worker must call beginMutation()
// immediately before its first side effect. Cancellation and admission are
// atomic: false means that worker must leave the destination unchanged.
class TaskControl {
public:
    [[nodiscard]] bool cancellationRequested() const;
    [[nodiscard]] bool beginMutation() const;
private:
    struct State;
    explicit TaskControl(bool mutation);
    void cancel() const;
    [[nodiscard]] bool mutationStarted() const;
    std::shared_ptr<State> state_;
    friend class SessionCoordinator;
};

// One application-owned coordinator, used only on the model's event-loop
// thread. Work/Inspect run in the thread pool on value snapshots; they must
// not capture GUI objects. Completion runs on this object's thread, only for
// an uncancelled, current session and revision. Git keeps its asynchronous
// QProcess implementation and shares the worker mutation queue.
class SessionCoordinator final : public QObject {
    Q_OBJECT
public:
    using Work = std::function<TaskResult(const TaskInput &, const TaskControl &)>;
    // Inspect must observe the destination without writing it. It runs even
    // after cancellation and holds the queue until done. Throws/NotAttempted
    // after an admitted mutation retain Uncertain, never an assumed rollback.
    using Inspect = std::function<MutationObservation(const TaskInput &)>;
    using Completion = std::function<void(const TaskCompletion &)>;

    explicit SessionCoordinator(SessionModel &model, QString gitExecutable, QObject *parent = nullptr);
    ~SessionCoordinator() override;
    // worktree is the verified repository root, not a launch subdirectory.
    // Canonicalization coalesces symlink aliases. Replacing cancels old work
    // without releasing an active mutation's queue slot prematurely.
    bool replaceSession(const QString &worktree, const QString &operation, std::vector<FileSession> files);
    // Complete a current load of the same initial placeholders, including
    // failed open commits. Captured files replace their pending placeholders;
    // pending edits and unrelated session replacements remain protected.
    bool completeSessionLoad(const TaskStamp &stamp, std::vector<FileSession> files);
    bool completeRecoveryLoad(const TaskStamp &stamp, std::vector<FileSession> observed, RecoveryRestore restore);
    [[nodiscard]] quint64 generation() const;
    [[nodiscard]] int pendingTasks() const;
    // Zero rejects invalid/stale requests. Accepted requests finish once,
    // asynchronously. Work owns expensive computation/serialization only;
    // use submitMutation for any app-owned filesystem/index side effect.
    TaskId submit(const std::string &fileId, RevisionId expected, Work work, Completion completion = {});
    TaskId submitMutation(const std::string &fileId, RevisionId expected, Work work,
                          Inspect inspect, Completion completion = {});
    TaskId submitGit(const std::string &fileId, RevisionId expected, git::Request request,
                     Completion completion = {});
    bool cancel(TaskId id);
    void cancelAll();
    // Retains outcomes for original worktrees/revisions, including discarded
    // completions. Uncertain entries require further adapter inspection.
    [[nodiscard]] const std::vector<TaskCompletion> &mutationOutcomes() const;

Q_SIGNALS:
    void pendingTasksChanged();
    void sessionChanged();
    // Audit/diagnostic notification for every outcome. It is NOT permission
    // to update the current model; only Completion has passed the fences.
    void taskFinished(const branch_truce::TaskCompletion &completion);

private:
    bool replaceSession(const QString &worktree, const QString &operation,
                        std::vector<FileSession> files, bool completingLoad, RecoveryRestore *restore = nullptr);
    class State;
    std::unique_ptr<State> d;
};
}
Q_DECLARE_METATYPE(branch_truce::TaskCompletion)
