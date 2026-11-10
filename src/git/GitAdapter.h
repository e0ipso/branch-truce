// The Git adapter inspects repository state and performs guarded operations for conflict resolution.
#pragma once

#include "git/GitTypes.h"
#include <QByteArray>
#include <QDeadlineTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>

namespace branch_truce::git {

enum class Operation {
    DiscoverRepository,
    InspectIndex,
    ReadObject,
    ReadAttributes,
    MaterializeSource,
    StageFile,
    // Read-only preflight before releasing a mergetool parent to stage this path.
    // It cannot prevent helpers the independent parent ran before launching us,
    // or guarantee that the parent preserves this checked configuration.
    CheckParentStaging,
    // Clean/checkout in a private object store. Does not write the repository.
    CanonicalizeText
};
// Whether the operation can change the index. Callers run at most one of these per worktree at a time.
[[nodiscard]] bool isMutation(Operation operation);
enum class Outcome {
    Success,
    Rejected,
    LaunchFailure,
    ExitFailure,
    ProcessFailure,
    Cancelled,
    TimedOut,
    OutputLimit,
    InspectionFailure
};
// Only the selected path's index entries are observed. Changes are not attributed
// to this process rather than a concurrent writer, and do not describe the worktree.
enum class Mutation { NotAttempted, ObservedUnchanged, ObservedChanged, Uncertain };

// A label only. Never convert this text back into a path for an operation.
[[nodiscard]] QString displayPath(const QByteArray &path);

struct IndexEntry {
    QByteArray path; // Repository-relative Linux filename bytes, without a NUL.
    QByteArray mode;
    QByteArray objectId;
    unsigned int stage = 0;
};

struct Request {
    Operation operation = Operation::DiscoverRepository;
    QString directory;
    QByteArray path; // Exact relative filename bytes, never display text or a pathspec.
    QByteArray objectId; // Full hexadecimal ID for object operations.
    quint64 sessionGeneration = 0;
    quint64 inputRevision = 0;
    QByteArray input = {}; // CanonicalizeText worktree bytes only, including an empty file.
};
struct ConversionPolicy {
    QHash<QByteArray, QByteArray> attributes;
    QHash<QByteArray, QByteArray> configuration;
    bool operator==(const ConversionPolicy &) const = default;
};
struct Result {
    Request request;
    Outcome outcome = Outcome::Rejected;
    Mutation mutation = Mutation::NotAttempted;
    // The operation's own output. Policy checks before it never add theirs, since a
    // configuration listing can hold credentials.
    QByteArray standardOutput;
    QByteArray standardError;
    QByteArray indexBefore;
    QByteArray indexAfter;
    QList<IndexEntry> indexEntries; // Exact stages from DiscoverRepository/InspectIndex.
    RepositoryContext repository; // Structured DiscoverRepository snapshot, even when unsupported.
    // Why the request did not succeed, or why reinspection after it failed.
    Problem problem;
    int exitCode = -1; // Requested operation only; -1 if no normal exit occurred.
    bool outputIncomplete = false;
    // A cancel request can arrive after the operation's outcome is known.
    bool cancellationRequested = false;
    ConversionPolicy conversionPolicy;
    QString worktreeRoot;
    QByteArray canonicalBytes; // CanonicalizeText only. standardOutput is checkout form.
    QByteArray canonicalObjectId;
};

// How long one request may run, from its first Git process to its result.
// Index reinspection after a mutation has its own shorter bound.
inline constexpr int kGitRequestDeadlineMs = 30000;

// The running Git processes of one owner, such as a window's session.
// stopAll() asks every one of them to stop at once, so the adapters destroyed
// after it wait for them together, within one grace period, instead of one
// grace period each. Adapters on any thread may share it.
class GitProcesses {
public:
    void stopAll();
    // For GitAdapter: a started process group, and one that ended.
    void add(qint64 group);
    void remove(qint64 group);
    // How long a destroyed adapter waits after SIGTERM: what is left of the
    // shared grace period once stopAll() ran, otherwise normalMs.
    [[nodiscard]] int graceMs(int normalMs) const;

private:
    mutable std::mutex mutex_;
    std::set<qint64> groups_;
    std::optional<QDeadlineTimer> stopping_;
};

// Which Git runs, the configuration it reads and how long a request may take.
// The composition root builds it once; requests cannot override any of it.
struct GitProgram {
    QString executable;
    GitEnvironment environment;
    int deadlineMs = kGitRequestDeadlineMs;
    // Where its adapters register their processes; none when empty.
    std::shared_ptr<GitProcesses> processes = {};
};

// What a consumer of Git requests depends on. Every implementation, fakes
// included, keeps this contract on its owning event-loop thread:
// - start() returns false while a request runs, and that refusal emits nothing;
// - every accepted request emits finished() exactly once, later, from the
//   event loop, never inside start();
// - cancel() ends a running request as Cancelled unless its outcome is
//   already known, and does nothing when idle;
// - destruction cancels and emits nothing.
class GitRunner : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual bool start(const Request &request) = 0;
    virtual void cancel() = 0;
    [[nodiscard]] virtual bool busy() const = 0;
Q_SIGNALS:
    void finished(const branch_truce::git::Result &result);
};
// Builds one runner for a consumer, as a child of parent.
using GitRunners = std::function<std::unique_ptr<GitRunner>(QObject *parent)>;

// Runs requests with real Git, supervised and bounded. Requests cannot
// override the program, its environment, or its command arguments.
class GitAdapter final : public GitRunner {
    Q_OBJECT
public:
    explicit GitAdapter(GitProgram program, QObject *parent = nullptr);
    ~GitAdapter() override;
    bool start(const Request &request) override;
    void cancel() override;
    [[nodiscard]] bool busy() const override;

private:
    class State;
    std::unique_ptr<State> d;
};

// Runners that are GitAdapters for program.
[[nodiscard]] GitRunners gitAdapters(GitProgram program);

// Runs one request to completion on a runner from runners, in a nested event
// loop, and returns its result. The runner's own deadline bounds the wait. For
// worker threads and headless callers with no event loop of their own, such
// as recovery reconciliation and the mergetool launcher. A runner that refuses
// the request gives a Rejected result that says so.
[[nodiscard]] Result runBlocking(const GitRunners &runners, const Request &request);
[[nodiscard]] Result runBlocking(const GitProgram &program, const Request &request);
} // namespace branch_truce::git
Q_DECLARE_METATYPE(branch_truce::git::Result)
