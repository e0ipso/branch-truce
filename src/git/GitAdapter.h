#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <memory>

namespace branch_truce::git {

enum class Operation {
    DiscoverRepository, ListUnmerged, Status, InspectIndex, ReadObject,
    ReadConfiguration, ReadAttributes, MaterializeSource, StageFile
};
enum class Outcome {
    Success, Rejected, LaunchFailure, ExitFailure, ProcessFailure, Cancelled,
    TimedOut, OutputLimit, InspectionFailure
};
// Only the selected path's index entries are observed. Changes are not attributed
// to this process rather than a concurrent writer, and do not describe the worktree.
enum class Mutation { NotAttempted, ObservedUnchanged, ObservedChanged, Uncertain };

struct Request {
    Operation operation = Operation::DiscoverRepository;
    QString directory;
    QString path; // Exact relative path, never a pathspec or an argument list.
    QByteArray objectId; // Full hexadecimal ID for object operations.
    quint64 sessionGeneration = 0;
    quint64 inputRevision = 0;
};
struct Result {
    Request request;
    Outcome outcome = Outcome::Rejected;
    Mutation mutation = Mutation::NotAttempted;
    QByteArray standardOutput;
    QByteArray standardError;
    QByteArray indexBefore;
    QByteArray indexAfter;
    QString diagnostic;
    int exitCode = -1; // Requested operation only; -1 if no normal exit occurred.
    bool outputIncomplete = false;
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
