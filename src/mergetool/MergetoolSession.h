// The mergetool session connects waiting Git launchers to the window and tracks their published results.
#pragma once

#include "git/GitAdapter.h"
#include "mergetool/MergetoolProtocol.h"
#include "mergetool/MergetoolServer.h"
#include "publication/ResolutionPublisher.h"
#include "session/SessionCoordinator.h"
#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <functional>
#include <map>
#include <optional>
#include <string>

class QTimer;

namespace branch_truce {

// Queues the launchers Git started for this operation and hands each one its
// decision. Exit success is sent only after Mark resolved has written the
// launched path. It also revalidates restored files that only a waiting
// launcher can clear; see RecoveryRestore.
class MergetoolSession final : public QObject {
    Q_OBJECT
public:
    using Listen = MergetoolServer::Listen;
    struct Config {
        // The private directory of the session sockets and launcher intakes;
        // see mergetoolSocketDirectory().
        QString socketDirectory;
        // The configuration files this window's Git reads. A launcher whose
        // Git reads others is refused, since the parent stages with them.
        git::GitEnvironment gitEnvironment;
        // The Git that copied-launch checks run.
        QString gitExecutable;
    };
    // loadedRepository is the loaded operation that copied-launch checks inspect.
    MergetoolSession(ResolutionPublisher &publisher, SessionStore &files, SessionCoordinator &coordinator,
                     std::function<git::RepositoryContext()> loadedRepository, Config config,
                     QObject *parent = nullptr);
    Listen listen(const git::RepositoryContext &repository, QString *error);
    [[nodiscard]] bool listening() const { return server_.listening(); }
    [[nodiscard]] bool parentSessionActive() const { return parentSession_; }
    [[nodiscard]] bool blocks(const QByteArray &path) const;
    [[nodiscard]] bool waitingOn(const QByteArray &path) const;
    [[nodiscard]] QByteArray waitingPath() const;
    [[nodiscard]] bool hasWaiter(const QByteArray &path) const;
    // SHA-256 hex of the MERGED copy the waiting launcher for path kept at
    // launch. Empty when no launcher waits on path.
    [[nodiscard]] QByteArray waitingMergedSha256(const QByteArray &path) const;
    // The intake of the launcher waiting on path. Its launcher removes it on exit.
    [[nodiscard]] QString intake(const QByteArray &path) const;
    [[nodiscard]] bool canEndParentSession() const;
    bool leave(const QByteArray &path);
    // Launched paths whose launcher has not received a decision yet.
    [[nodiscard]] int unreleasedLaunches() const { return static_cast<int>(waiters_.size()); }
    // Deliberate session leave: every waiting launcher gets an explicit leave,
    // restores its original file and exits unresolved. Returns how many were told.
    int leaveAll();
    bool endParentSession();
    // Replacing the session's files drops the publisher's parent staging.
    // Assigns the waiting launcher's path to the reloaded files again.
    void reassign();
    void onPublisherChanged();
    // A load's restored files that git mergetool replaced by a same-byte
    // copy. They replace the previous load's entries.
    void adoptCopiedLaunches(std::map<std::string, SnapshotFileIdentity> copiedLaunches);
    // Ends the previous load's copied launches and their revalidation tasks.
    void clearCopiedLaunches();
    // Clears the recovery error of a restored file that git mergetool replaced
    // by a same-byte copy, once a launcher waits on its path with a MERGED copy
    // of the launch bytes, the file still has the identity recovery saw, and a
    // background check finds its index entries and conversion policy unchanged.
    void acceptCopiedLaunches();

Q_SIGNALS:
    void notice(const QString &message);
    void changed();

private:
    // A launcher that is attached and has no decision yet. It leaves the
    // list when it gets one or its connection closes.
    struct Waiter {
        QString invocation;
        QByteArray path;
        QString intake;
        QByteArray mergedSha256;
        quint64 connection = 0;
        bool assigned = false;
    };
    struct IntakeCheck {
        std::optional<MergetoolIntake> intake;
        QString error;
    };
    void attachReceived(quint64 connection, const AttachMessage &message);
    void intakeChecked(quint64 connection, const AttachMessage &message, const IntakeCheck &check);
    void closed(quint64 connection);
    void reject(quint64 connection, const QString &invocation, const QString &reason);
    // Removes the waiter and gives its staging assignment to the next one.
    void release(qsizetype index);
    void tryAssign();
    void clearAssignment();
    void pollAcknowledgment();
    [[nodiscard]] qsizetype indexOfConnection(quint64 connection) const;
    [[nodiscard]] qsizetype indexOfPath(const QByteArray &path) const;
    [[nodiscard]] bool launchedWithCapturedBytes(const FileSession &file) const;
    void settleCopiedLaunch(const std::string &fileId, const SnapshotFileIdentity &identity, const QString &failure);

    ResolutionPublisher &publisher_;
    SessionStore &files_;
    SessionCoordinator &coordinator_;
    std::function<git::RepositoryContext()> loadedRepository_;
    const Config config_;
    MergetoolServer server_;
    QTimer *acknowledgment_ = nullptr;
    git::RepositoryContext repository_;
    QByteArray sessionKey_;
    QList<Waiter> waiters_;
    std::string acknowledgmentFile_;
    RevisionId acknowledgmentRevision_ = 0;
    int acknowledgmentInterval_ = 0;
    bool parentSession_ = false;
    bool offerHeld_ = false;
    bool assigning_ = false;
    // Restored files that only a waiting launcher can clear.
    std::map<std::string, SnapshotFileIdentity> copiedLaunches_;
    // The revalidation task in flight for a copiedLaunches_ entry.
    std::map<std::string, TaskId> copiedLaunchChecks_;
};
}
