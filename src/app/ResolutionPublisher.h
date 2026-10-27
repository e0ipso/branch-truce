#pragma once

#include "SessionCoordinator.h"
#include "git/TextConversion.h"
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
    struct Offer {
        std::string fileId;
        QByteArray path;
        RevisionId revision = 0;
        QByteArray mode;
        QByteArray canonicalObjectId;
        QString worktreeRoot;
        git::ConvertedText prepared;
    };
    struct StartResult {
        Transition transition = Transition::Blocked;
        QString detail;
        // The refused revision's markers, when conflict markers blocked it.
        std::optional<MarkerScan> markers;
    };

    ResolutionPublisher(SessionModel &files, SessionCoordinator &coordinator,
                        QString gitExecutable, QObject *parent = nullptr);

    void captureBaselines(const git::RepositoryContext &repository);
    // Uses the current repository file as the launch destination of a
    // recovered file that git mergetool replaced by a copy. The file must
    // still have the identity recovery validated and the launch bytes.
    bool adoptCopiedLaunch(const std::string &fileId, const SnapshotFileIdentity &identity);
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
    struct Baseline {
        std::optional<storage::DestinationSnapshot> destination;
        QString diagnostic;
    };
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
    };

    [[nodiscard]] const FileSession *file(const std::string &id) const;
    [[nodiscard]] bool parentOwns(const QByteArray &path) const;
    [[nodiscard]] bool previouslyWritten(const FileSession &session, RevisionId revision) const;
    [[nodiscard]] bool preserveWrite() const;
    StartResult refuse(Transition transition, const QString &detail);
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
    void published(const TaskCompletion &completion);
    void staged(const TaskCompletion &completion);
    void parentPreflight(const TaskCompletion &completion);
    void noteStale(const TaskCompletion &completion);
    [[nodiscard]] bool accept(const TaskCompletion &completion) const;

    SessionModel &files_;
    SessionCoordinator &coordinator_;
    const QString executable_;
    git::RepositoryContext repository_;
    git::TextConversion conversion_;
    std::map<std::string, Baseline> baselines_;
    std::optional<Attempt> attempt_;
    std::optional<Offer> offer_;
    QByteArray parentPath_;
    std::optional<std::pair<std::string, RevisionId>> confirmation_;
    std::shared_ptr<storage::PublicationResult> publication_;
};
}
