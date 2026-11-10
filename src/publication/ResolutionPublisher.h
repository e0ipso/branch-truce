// Resolution publishing writes a reviewed result and verifies its Git staging or parent handoff.
#pragma once

#include "git/TextConversion.h"
#include "publication/ParentStaging.h"
#include "session/SessionCoordinator.h"
#include "storage/DestinationWriter.h"
#include <QByteArray>
#include <QObject>
#include <QString>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace branch_truce {

// Mark resolved for one exact revision. Repository mode stages only that path.
// A parent-assigned path is never staged here; the launcher verifies the parent.
class ResolutionPublisher final : public QObject {
    Q_OBJECT
public:
    using Offer = ParentOffer;
    // The repository file a supported file publishes over, as captured before
    // any edit, or why it cannot be used, as a translated sentence.
    struct Baseline {
        std::optional<storage::DestinationSnapshot> destination;
        QString diagnostic;
    };
    using Baselines = std::map<std::string, Baseline>;
    struct StartResult {
        Transition transition = Transition::Blocked;
        QString detail;
        // The refused revision's markers, when conflict markers blocked it.
        std::optional<MarkerScan> markers;
    };

    // git builds the runner that converts results. destinations is the file
    // system repository files are captured and written through.
    ResolutionPublisher(SessionStore &files, SessionCoordinator &coordinator, const git::GitRunners &git,
                        std::shared_ptr<const storage::FileSystem> destinations = storage::systemFileSystem(),
                        QObject *parent = nullptr);

    // Blocking: reads every supported file's destination, so a load worker
    // calls it. A destination must still be the captured launch result.
    [[nodiscard]] static Baselines readBaselines(const QString &worktreeRoot, const std::vector<FileSession> &files,
                                                 const std::shared_ptr<const storage::FileSystem> &destinations);
    // Uses baselines a load read for repository's files.
    void adoptBaselines(const git::RepositoryContext &repository, Baselines baselines);
    [[nodiscard]] const std::shared_ptr<const storage::FileSystem> &destinations() const { return destinations_; }
    // Uses live, the repository file a worker just captured, as the launch
    // destination of a recovered file that git mergetool replaced by a copy.
    // It must still have the identity recovery validated and the launch bytes.
    bool adoptCopiedLaunch(const std::string &fileId, const SnapshotFileIdentity &identity,
                           const storage::DestinationCapture &live);
    // Empty path clears the assignment. The assigned path is never passed to git add.
    bool assignParentStaging(const QByteArray &path);
    [[nodiscard]] bool busy() const;
    [[nodiscard]] bool confirmationPending() const;
    [[nodiscard]] QString confirmationFileId() const;
    [[nodiscard]] QString confirmationRevision() const;
    [[nodiscard]] std::optional<Offer> parentStagingOffer(const std::string &fileId) const;
    StartResult start(const std::string &fileId, RevisionId expected, bool confirmUndecided);
    void cancelConfirmation();
    // True only when verified parent staging was recorded. A still-unmerged index stays waiting.
    bool verifyParentAcknowledgment(const std::string &fileId, RevisionId expected);

Q_SIGNALS:
    void changed(const QString &message);

private:
    enum class Step { Prepare, Discover, Index, Publish, Stage, ParentPreflight, Verify };
    // What finishes each step, and whether the step runs after the write, so
    // that a failure there is a staging failure.
    struct StepHandling {
        void (ResolutionPublisher::*task)(const TaskCompletion &) = nullptr;
        void (ResolutionPublisher::*conversion)(const git::TextConversionResult &) = nullptr;
        bool afterWrite = false;
    };
    [[nodiscard]] static const StepHandling &handling(Step step);
    struct Attempt {
        quint64 generation = 0;
        std::string fileId;
        RevisionId revision = 0;
        QByteArray path;
        QByteArray mode;
        QByteArray serialized;
        bool parentOwned = false;
        bool acknowledging = false;
        Step step = Step::Prepare;
        TaskId task = 0;
        std::optional<git::ConvertedText> prepared;
        // What the publish step's writer saw, once it ran.
        std::shared_ptr<const storage::PublicationResult> published;
    };

    [[nodiscard]] const FileSession *file(const std::string &id) const;
    [[nodiscard]] bool parentOwns(const QByteArray &path) const;
    [[nodiscard]] bool previouslyWritten(const FileSession &session, RevisionId revision) const;
    [[nodiscard]] bool preserveWrite() const;
    [[nodiscard]] static StartResult result(Transition transition, const QString &detail);
    StartResult recordWrite(const std::string &fileId, RevisionId revision, Outcome outcome, const QString &detail);
    void fail(Outcome outcome, const QString &detail, bool stage);
    void finishResolved();
    void clearAttempt();
    bool launch(Step step, git::Request request);
    bool startPrepare();
    bool startVerify();
    void prepared(const git::TextConversionResult &result);
    void verified(const git::TextConversionResult &result);
    void discovered(const TaskCompletion &completion);
    void indexed(const TaskCompletion &completion);
    void published(const TaskCompletion &completion, const storage::PublicationResult &evidence);
    void staged(const TaskCompletion &completion);
    void parentPreflight(const TaskCompletion &completion);
    void noteStale(const TaskCompletion &completion);
    [[nodiscard]] bool accept(const TaskCompletion &completion) const;

    SessionStore &files_;
    SessionCoordinator &coordinator_;
    const std::shared_ptr<const storage::FileSystem> destinations_;
    git::RepositoryContext repository_;
    git::TextConversion conversion_;
    Baselines baselines_;
    std::optional<Attempt> attempt_;
    ParentStaging parent_;
    std::optional<std::pair<std::string, RevisionId>> confirmation_;
};
}
