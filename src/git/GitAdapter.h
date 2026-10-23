#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <memory>

namespace branch_truce::git {

enum class Operation {
    DiscoverRepository, ListUnmerged, Status, InspectIndex, ReadObject,
    ReadConfiguration, ReadAttributes, MaterializeSource, StageFile,
    // Read-only preflight before releasing a mergetool parent to stage this path.
    // It cannot prevent helpers the independent parent ran before launching us,
    // or guarantee that the parent preserves this checked configuration.
    CheckParentStaging,
    // Clean/checkout in a private object store. Does not write the repository.
    CanonicalizeText
};
enum class Outcome {
    Success, Rejected, LaunchFailure, ExitFailure, ProcessFailure, Cancelled,
    TimedOut, OutputLimit, InspectionFailure
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
};
struct RepositoryContext {
    QString worktreeRoot;
    QString gitDirectory; // Canonical per-worktree directory, including linked worktrees.
    RepositoryOperation operation = RepositoryOperation::None;
    // Operation/stage identity only. File types and conversion policy must still
    // be checked before editing or publishing. Reinspect before any mutation.
    bool supported = false;
    QString diagnostic;
    SourceIdentity stage2;
    SourceIdentity stage3;
    QByteArray baseCommitId; // Empty for an unavailable common earlier version.
    // Validated rebase context; these names do not label either index side.
    QByteArray headName;
    QByteArray originalHead;
    QByteArray onto;
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
    QByteArray standardOutput;
    QByteArray standardError;
    QByteArray indexBefore;
    QByteArray indexAfter;
    QList<IndexEntry> indexEntries; // Exact stages from DiscoverRepository/ListUnmerged/InspectIndex.
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

// Use on the owning event-loop thread. The application selects the executable;
// requests cannot override it, its environment, or its command arguments.
class GitAdapter final : public QObject {
    Q_OBJECT
public:
    explicit GitAdapter(const QString &executable, QObject *parent = nullptr);
    ~GitAdapter() override;
    // Busy returns false without a result. Every accepted request completes once,
    // asynchronously. Destruction cancels without publishing a result.
    bool start(const Request &request);
    void cancel();
    [[nodiscard]] bool busy() const;
Q_SIGNALS:
    void finished(const branch_truce::git::Result &result);
private:
    class State;
    std::unique_ptr<State> d;
};
} // namespace branch_truce::git
Q_DECLARE_METATYPE(branch_truce::git::Result)
