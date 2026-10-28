#pragma once
#include "MergetoolSession.h"
#include "RecoveryRestore.h"
#include "git/ConflictClassifier.h"
#include <QObject>
#include <QString>
#include <QVariantList>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace branch_truce {
class SessionCoordinator;
class SessionModel;

// Opens the launched repository: classifies its conflicts, loads the working
// files and their recovery records off the GUI thread, and reloads with a
// recovery session the user chooses. It owns the launch state the window shows
// until the session is ready.
class SessionLoader final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString startupState READ startupState NOTIFY launchChanged)
    Q_PROPERTY(bool opening READ opening NOTIFY launchChanged)
    Q_PROPERTY(QString repositoryRoot READ repositoryRoot NOTIFY launchChanged)
    Q_PROPERTY(QString operation READ operation NOTIFY launchChanged)
    Q_PROPERTY(QString detail READ detail NOTIFY launchChanged)
    Q_PROPERTY(QVariantList recoveryChoices READ recoveryChoices NOTIFY launchChanged)
    Q_PROPERTY(QString stage2 READ stage2 NOTIFY launchChanged)
    Q_PROPERTY(QString stage3 READ stage3 NOTIFY launchChanged)
    Q_PROPERTY(QString stage2CommitId READ stage2CommitId NOTIFY launchChanged)
    Q_PROPERTY(QString stage3CommitId READ stage3CommitId NOTIFY launchChanged)
    Q_PROPERTY(QString workflow READ workflow CONSTANT)
public:
    // What the loader needs from the window's other owners. A finished load
    // calls clearCopiedLaunches, replaces the session, calls reassignMergetool
    // and then, for a supported operation with files, startSession, in that order.
    struct Owners {
        // Escaped display text for exact path bytes.
        std::function<QString(const std::string &path)> displayPath;
        std::function<QString()> selectedFileId;
        // The session ended, or a resolution is running.
        std::function<bool()> sessionBusy;
        // The previous load's copied launches and their revalidation tasks end.
        std::function<void()> clearCopiedLaunches;
        // A recovery choice reloads while Git may still wait on a launched path.
        std::function<void()> reassignMergetool;
        // Captures the publication baselines, adopts this load's copied
        // launches, then starts the mergetool listener.
        std::function<MergetoolSession::Listen(const git::RepositoryContext &repository,
            std::map<std::string, SnapshotFileIdentity> copiedLaunches, QString *error)> startSession;
    };
    SessionLoader(SessionModel &files, SessionCoordinator &coordinator, Owners owners, QObject *parent = nullptr);

    [[nodiscard]] QString startupState() const { return state_; }
    [[nodiscard]] bool opening() const { return state_ == QLatin1String("opening"); }
    [[nodiscard]] QString repositoryRoot() const { return repository_.worktreeRoot; }
    [[nodiscard]] QString operation() const { return operation_; }
    [[nodiscard]] QString detail() const { return detail_; }
    // Matching recovery sessions with app edits that blocked restoration at
    // this load: session, lastChanged and the display paths it edited.
    [[nodiscard]] QVariantList recoveryChoices() const;
    // Each source's label, which already names its commit.
    [[nodiscard]] QString stage2() const;
    [[nodiscard]] QString stage3() const;
    [[nodiscard]] QString stage2CommitId() const;
    [[nodiscard]] QString stage3CommitId() const;
    [[nodiscard]] QString workflow() const;
    [[nodiscard]] const git::RepositoryContext &repository() const { return repository_; }
    // The file to select first once the load lists it; see openRepository.
    [[nodiscard]] QString preferredFileId() const { return preferredFileId_; }
    void clearPreferredFileId() { preferredFileId_.clear(); }

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
    Q_INVOKABLE bool chooseRecoverySession(const QString &session);

Q_SIGNALS:
    void launchChanged();
    // Shown as the command message: a refused recovery choice, or empty when
    // an accepted choice clears the previous message.
    void notice(const QString &message);

private:
    void setState(QString state, QString detail);
    void classified(const git::ConflictClassificationResult &result);
    void loadWorkingFiles(QList<git::Conflict> conflicts);
    SessionModel &files_;
    SessionCoordinator &coordinator_;
    Owners owners_;
    git::ConflictClassifier classifier_;
    git::RepositoryContext repository_;
    QString state_ = QStringLiteral("idle");
    QString operation_;
    QString detail_;
    QString preferredFileId_;
    QString recoverySession_;
    std::vector<RecoveryChoice> recoveryChoices_;
};
}
