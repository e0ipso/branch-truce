// Private Git adapter state tracks each bounded request and its supervised Git process.
#pragma once

// Private to the branch_truce_git library. GitAdapter's sources share the
// adapter state through this header; nothing outside src/git includes it.

#include "GitAdapter.h"
#include "RepositoryDiscovery.h"
#include "core/ResourceLimits.h"

#include <QHash>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>
#include <sys/stat.h>
#include <vector>

namespace branch_truce::git {
namespace detail {
// Measured Git process-output budget. Every phase starts a fresh counter so
// preflight cannot consume the operation or a later representation.
inline constexpr qsizetype outputLimit = static_cast<qsizetype>(branch_truce::kMaxGitReadBytes);
inline constexpr qsizetype inspectionLimit = 1024 * 1024;
inline constexpr int deadlineMs = 30000;
inline constexpr int inspectionMs = 5000;
inline constexpr int killGraceMs = 200;

bool exactPath(const QString &path);
bool secureExecutable(const QString &path, struct stat &identity);
} // namespace detail

class GitAdapter::State {
public:
    enum class Phase { Version, Configuration, PolicyRoot, ObjectDirectory, TrackedPaths, Attributes,
                       GitDirectory, Hooks, Baseline, Operation, Discovery, Inspection,
                       CanonicalSize, CanonicalObject, CanonicalCheckout };
    // Process control (GitAdapterProcess.cpp).
    explicit State(GitAdapter *owner, const QString &selected);
    ~State();
    void signalGroup(int signal);
    void closeLaunchPipe();
    void observeNativeLaunch();
    void launch(Phase next, const QList<QByteArray> &arguments, bool configuration = false,
                const QByteArray &input = {});
    void stop(Outcome outcome, const QString &message);
    void inspectionFailed(const QString &message);
    void drain(bool error);
    void inspectMutation();

    // Request lifecycle (GitAdapter.cpp).
    void begin();
    void complete();
    void reject(const QString &message);
    void reserveMutation(const QString &key);
    void releaseMutation();

    // Operation and conversion policy (GitAdapterPolicy.cpp).
    bool isMutation() const;
    bool stagingPolicy() const;
    bool objectPolicy() const;
    bool filterPolicy() const;
    QList<QByteArray> indexArguments() const;
    QList<QByteArray> operationArguments() const;
    bool validIndex(const QByteArray &bytes, bool resolved) const;
    bool checkConfiguration();
    bool hasFilterConfiguration() const;
    bool checkFilter(const QByteArray &path, const QByteArray &driver);
    bool consumeStatusAttributes(const QByteArray &chunk);
    bool captureAttributes();
    bool checkFilters();

    // Phase sequence (GitAdapterPhases.cpp). afterProcess runs the checks
    // shared by every phase, then hands the output to the phase's handler.
    void afterProcess(bool normal, int code);
    void afterPolicy();
    void handleVersion();
    void handleConfiguration();
    void handlePolicyRoot();
    void handleObjectDirectory();
    void handleTrackedPaths();
    void handleAttributes();
    void handleGitDirectory();
    void handleHooks();
    void handleBaseline();
    void handleOperation();
    void handleDiscovery(int code);
    void handleInspection(bool normal, int code);
    void handleCanonicalSize();
    void handleCanonicalObject();
    void handleCanonicalCheckout();

    GitAdapter *q;
    QProcess process;
    QTimer deadline;
    QTimer escalation;
    QString executable;
    QString directory;
    QString mutationKey;
    QString originalObjectDirectory;
    struct stat identity {};
    Result result;
    std::unique_ptr<detail::RepositoryDiscovery> discovery;
    std::unique_ptr<QTemporaryDir> temporaryObjects;
    QByteArray phaseOutput;
    QByteArray phaseInput;
    QHash<QByteArray, QByteArray> configuration;
    QList<QByteArray> nativeArguments;
    QList<QByteArray> nativeEnvironment;
    std::vector<char *> nativeArgv;
    std::vector<char *> nativeEnvp;
    QByteArray nativeDirectory;
    int launchPipe[2]{-1, -1};
    Phase phase = Phase::Version;
    qsizetype bytesRead = 0;
    qint64 pid = 0;
    bool executableValid = false;
    bool active = false;
    bool stopping = false;
    bool mutationStarted = false;
    bool nativeLaunchFailed = false;
};
} // namespace branch_truce::git
