// The Git adapter inspects repository state and performs guarded operations for conflict resolution.
#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>

class QProcessEnvironment;

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

enum class RepositoryOperation { None, Merge, Rebase, Unsupported };
struct SourceIdentity {
    QByteArray commitId;
    QString label;
    QString name = {}; // A unique local branch observed pointing exactly at commitId.
};
struct RepositoryContext {
    QString worktreeRoot;
    QString gitDirectory; // Canonical per-worktree directory, including linked worktrees.
    RepositoryOperation operation = RepositoryOperation::None;
    // Operation identity only. Check unsupportedPaths, file types and conversion
    // policy before editing or publishing. Reinspect before any mutation.
    bool supported = false;
    QString diagnostic;
    // Exact conflict paths whose stages cannot be attributed to the operation.
    // Keep their index entries visible without rejecting other paths.
    QHash<QByteArray, QString> unsupportedPaths;
    SourceIdentity stage2;
    SourceIdentity stage3;
    QByteArray baseCommitId; // Empty for an unavailable common earlier version.
    // Validated rebase context; these names do not label either index side.
    QByteArray headName;
    QByteArray originalHead;
    QByteArray onto;
    QString ontoName = {};
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
    QString diagnostic;
    int exitCode = -1; // Requested operation only; -1 if no normal exit occurred.
    bool outputIncomplete = false;
    // A cancel request can arrive after the operation's outcome is known.
    bool cancellationRequested = false;
    ConversionPolicy conversionPolicy;
    QString worktreeRoot;
    QByteArray canonicalBytes; // CanonicalizeText only. standardOutput is checkout form.
    QByteArray canonicalObjectId;
};

// The variables that choose which configuration files Git reads. The Git the
// adapter starts reads the same files as the user's own Git and a `git
// mergetool` parent started from the same environment. Every other variable of
// the Git child is fixed by the adapter.
struct GitEnvironment {
    QString home; // HOME
    QString configHome; // XDG_CONFIG_HOME; empty lets Git use $HOME/.config
    QString globalConfig; // GIT_CONFIG_GLOBAL; empty when unset
    QString systemConfig; // GIT_CONFIG_SYSTEM; empty when unset
    QString noSystem; // GIT_CONFIG_NOSYSTEM as given; empty when unset
    bool operator==(const GitEnvironment &) const = default;
    // Takes each path only when it is absolute. HOME falls back to the
    // account's home directory, as before a desktop session sets it.
    [[nodiscard]] static GitEnvironment from(const QProcessEnvironment &environment);
    [[nodiscard]] static GitEnvironment fromProcess();
};

// How long one request may run, from its first Git process to its result.
// Index reinspection after a mutation has its own shorter bound.
inline constexpr int kGitRequestDeadlineMs = 30000;

// Which Git runs, the configuration it reads and how long a request may take.
// The composition root builds it once; requests cannot override any of it.
struct GitProgram {
    QString executable;
    GitEnvironment environment;
    int deadlineMs = kGitRequestDeadlineMs;
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
    // Reads the configuration this process's environment selects. For callers
    // that hold only the executable.
    explicit GitAdapter(const QString &executable, QObject *parent = nullptr);
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

// Runs one request to completion in a nested event loop and returns its
// result. The adapter's own deadline bounds the wait. For headless callers
// with no event loop of their own, such as the mergetool launcher.
[[nodiscard]] Result runBlocking(const GitProgram &program, const Request &request);
} // namespace branch_truce::git
Q_DECLARE_METATYPE(branch_truce::git::Result)
