// Session loading discovers conflicted files and offers recoverable work without silently replacing it.
#pragma once
#include "core/Correspondence.h"
#include "git/ConflictClassifier.h"
#include "mergetool/MergetoolSession.h"
#include "recovery/RecoveryRestore.h"
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace branch_truce {
class ResolutionPublisher;
class Selection;
class SessionCompletion;
class SessionCoordinator;
class SessionStore;

// Opens the launched repository: classifies its conflicts, loads the working
// files and their recovery records off the GUI thread, and reloads with a
// recovery session the user chooses. It owns the launch state the window shows
// until the session is ready. A finished load ends the previous load's copied
// launches, replaces the session, reassigns waiting launchers and then, for a
// supported operation with files, captures the publication baselines, adopts
// this load's copied launches and starts listening for launchers.
class SessionLoader : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by the application")
    Q_PROPERTY(branch_truce::SessionLoader::StartupState startupState READ startupState NOTIFY launchChanged)
    Q_PROPERTY(bool opening READ opening NOTIFY launchChanged)
    Q_PROPERTY(QString repositoryRoot READ repositoryRoot NOTIFY launchChanged)
    Q_PROPERTY(QString operation READ operation NOTIFY launchChanged)
    Q_PROPERTY(QString operationDescription READ operationDescription NOTIFY launchChanged)
    Q_PROPERTY(QString stage2Name READ stage2Name NOTIFY launchChanged)
    Q_PROPERTY(QString stage3Name READ stage3Name NOTIFY launchChanged)
    Q_PROPERTY(QString detail READ detail NOTIFY launchChanged)
    Q_PROPERTY(QVariantList recoveryChoices READ recoveryChoices NOTIFY launchChanged)
    Q_PROPERTY(QString stage2 READ stage2 NOTIFY launchChanged)
    Q_PROPERTY(QString stage3 READ stage3 NOTIFY launchChanged)
    Q_PROPERTY(QString stage2CommitId READ stage2CommitId NOTIFY launchChanged)
    Q_PROPERTY(QString stage3CommitId READ stage3CommitId NOTIFY launchChanged)
    Q_PROPERTY(QString stage2ShortCommitId READ stage2ShortCommitId NOTIFY launchChanged)
    Q_PROPERTY(QString stage3ShortCommitId READ stage3ShortCommitId NOTIFY launchChanged)
    Q_PROPERTY(QString workflow READ workflow CONSTANT)
public:
    // Where opening the launched repository stands. ReadyWithoutMergetool is
    // ready, but no git mergetool launcher can attach.
    enum class StartupState { Idle, Opening, Ready, ReadyWithoutMergetool, NoSession, Unsupported, Error };
    Q_ENUM(StartupState)
    // The collaborators a load reports to.
    struct Session {
        SessionStore &store;
        SessionCoordinator &coordinator;
        Selection &selection;
        ResolutionPublisher &publisher;
        MergetoolSession &mergetool;
        const SessionCompletion &completion;
    };
    // git builds the runners that classify the conflicts and reconcile
    // recovery. Loaded files align their sources with `correspondence`, or
    // with the engine's default when it is null.
    SessionLoader(Session session, git::GitRunners git, QObject *parent = nullptr,
                  std::shared_ptr<const CorrespondenceAlgorithm> correspondence = {});

    [[nodiscard]] StartupState startupState() const { return state_; }
    [[nodiscard]] bool opening() const { return state_ == StartupState::Opening; }
    [[nodiscard]] QString repositoryRoot() const { return repository_.worktreeRoot; }
    // The classified operation's name, empty before or without a successful classification.
    [[nodiscard]] QString operation() const;
    [[nodiscard]] QString operationDescription() const;
    [[nodiscard]] QString stage2Name() const { return repository_.stage2.name; }
    [[nodiscard]] QString stage3Name() const { return repository_.stage3.name; }
    [[nodiscard]] QString detail() const { return detail_; }
    // Matching recovery sessions with app edits that blocked restoration at
    // this load: session, lastChanged and the display paths it edited.
    [[nodiscard]] QVariantList recoveryChoices() const;
    // The source roles and shortened commit IDs for display.
    [[nodiscard]] QString stage2() const;
    [[nodiscard]] QString stage3() const;
    [[nodiscard]] QString stage2CommitId() const;
    [[nodiscard]] QString stage3CommitId() const;
    [[nodiscard]] QString stage2ShortCommitId() const;
    [[nodiscard]] QString stage3ShortCommitId() const;
    [[nodiscard]] QString workflow() const;
    Q_INVOKABLE QString launchSizeText(const QString &fileId) const;
    [[nodiscard]] const git::RepositoryContext &repository() const { return repository_; }

    // One launched repository session per controller. Paths are launch directories,
    // never inferred from a filename or a Git environment override.
    // preferredFileId selects that file first when the session contains it.
    // recoverySession names the recovery session the user chose to restore when
    // several match; the other matching sessions are set aside.
    bool openRepository(const QString &directory, const QString &preferredFileId = {},
                        const QString &recoverySession = {});
    // Reloads the session with one of recoveryChoices, through the same startup
    // restore as --recovery-session: it is adopted and the other matching
    // sessions are set aside, not deleted. Refused while another window holds
    // one of the choices, or while this window's own session holds app edits,
    // pending input or a running resolution that a reload would leave behind.
    // Those refusals return false. An accepted choice returns true and opens
    // at once, but this window's store is read on the worker first: if it
    // cannot be read, the window returns to its files and notice refuses the
    // choice then.
    Q_INVOKABLE bool chooseRecoverySession(const QString &session);

Q_SIGNALS:
    void launchChanged();
    // Shown as the command message: a refused recovery choice, or empty when
    // an accepted choice clears the previous message.
    void notice(const QString &message);

private:
    // Shown when the repository has no merge or rebase, unlike an operation
    // whose conflicts are all resolved.
    [[nodiscard]] static QString noOperationDetail();
    void setState(StartupState state, QString detail);
    void classified(const git::ConflictClassificationResult &result);
    void loadWorkingFiles(QList<git::Conflict> conflicts);
    // Why this window cannot switch to session now, judged from memory while
    // it shows state, or empty.
    [[nodiscard]] QString choiceRefusal(const QString &session, StartupState state);
    [[nodiscard]] static QString appEditsRefusal();
    // The worker read this window's store at directory for a choice of session.
    void ownStoreRead(const QString &session, const QString &directory, const RecoveryReplay &own);
    // Leaves a choice that will not reload: the window shows its files again.
    void refuseChoice(const QString &message);
    Session session_;
    const git::GitRunners git_;
    std::shared_ptr<const CorrespondenceAlgorithm> correspondence_;
    git::ConflictClassifier classifier_;
    git::RepositoryContext repository_;
    QHash<QString, qint64> launchSizes_;
    StartupState state_ = StartupState::Idle;
    // Whether repository_ comes from a successful classification of the current open.
    bool classified_ = false;
    QString detail_;
    QString recoverySession_;
    std::vector<RecoveryChoice> recoveryChoices_;
    // A choice whose own-store read is running, and what the window showed before it.
    quint64 choiceTask_ = 0;
    QString choiceSession_;
    StartupState choiceState_ = StartupState::Idle;
    QString choiceDetail_;
};
}
