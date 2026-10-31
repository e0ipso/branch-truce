// The mergetool session connects waiting Git launchers to the window and tracks their published results.
#pragma once

#include "publication/ResolutionPublisher.h"
#include "session/SessionCoordinator.h"
#include "git/GitAdapter.h"
#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <map>
#include <string>

class QLocalServer;
class QLocalSocket;
class QTimer;

namespace branch_truce {
class SessionModel;

// Listens for blocking mergetool launchers and keeps their temporary inputs.
// Exit success is sent only after Mark resolved has written the launched path.
// It also revalidates restored files that only a waiting launcher can clear;
// see RecoveryRestore.
class MergetoolSession final : public QObject {
    Q_OBJECT
public:
    // Elsewhere: another live session already owns this operation's socket.
    // Unavailable: no launcher can attach to this session; error says why.
    enum class Listen { Listening, Elsewhere, Unavailable };
    // loadedRepository is the loaded operation that copied-launch checks
    // inspect, and gitExecutable the Git they run.
    MergetoolSession(ResolutionPublisher &publisher, SessionModel &files, SessionCoordinator &coordinator,
                     std::function<git::RepositoryContext()> loadedRepository, QString gitExecutable,
                     QObject *parent = nullptr);
    Listen listen(const git::RepositoryContext &repository, QString *error);
    [[nodiscard]] bool listening() const { return listening_; }
    [[nodiscard]] bool parentSessionActive() const { return parentSession_; }
    [[nodiscard]] bool blocks(const QByteArray &path) const;
    [[nodiscard]] bool waitingOn(const QByteArray &path) const;
    [[nodiscard]] QByteArray waitingPath() const;
    [[nodiscard]] bool hasWaiter(const QByteArray &path) const;
    // SHA-256 hex of the MERGED copy the waiting launcher for path kept at
    // launch. Empty when no unreleased launcher waits on path.
    [[nodiscard]] QByteArray waitingMergedSha256(const QByteArray &path) const;
    [[nodiscard]] bool canEndParentSession() const;
    [[nodiscard]] QStringList intakes() const { return intakes_; }
    bool leave(const QByteArray &path);
    // Launched paths whose launcher has not received a decision yet.
    [[nodiscard]] int unreleasedLaunches() const;
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
    struct Waiter {
        QString invocation;
        QByteArray path;
        QString intake;
        QByteArray mergedSha256;
        QLocalSocket *socket = nullptr;
        bool assigned = false;
        bool released = false;
    };
    void acceptConnection();
    void readSocket(QLocalSocket *socket);
    void dropped(QLocalSocket *socket);
    void reject(QLocalSocket *socket, const QString &invocation, const QString &reason);
    bool send(QLocalSocket *socket, const QJsonObject &object);
    void tryAssign();
    void pollAcknowledgment();
    [[nodiscard]] int indexOfSocket(const QLocalSocket *socket) const;
    [[nodiscard]] int indexOfPath(const QByteArray &path, bool unreleasedOnly) const;
    [[nodiscard]] bool launchedWithCapturedBytes(const FileSession &file) const;
    void settleCopiedLaunch(const std::string &fileId, const SnapshotFileIdentity &identity, const QString &failure);

    ResolutionPublisher &publisher_;
    SessionModel &files_;
    SessionCoordinator &coordinator_;
    std::function<git::RepositoryContext()> loadedRepository_;
    const QString gitExecutable_;
    QLocalServer *server_ = nullptr;
    QTimer *acknowledgment_ = nullptr;
    git::RepositoryContext repository_;
    QByteArray sessionKey_;
    QList<Waiter> waiters_;
    QHash<QLocalSocket *, QByteArray> buffers_;
    QStringList intakes_;
    std::string acknowledgmentFile_;
    RevisionId acknowledgmentRevision_ = 0;
    int acknowledgmentAttempts_ = 0;
    bool listening_ = false;
    bool parentSession_ = false;
    bool offerHeld_ = false;
    bool assigning_ = false;
    // Restored files that only a waiting launcher can clear.
    std::map<std::string, SnapshotFileIdentity> copiedLaunches_;
    // The revalidation task in flight for a copiedLaunches_ entry.
    std::map<std::string, TaskId> copiedLaunchChecks_;
};
}
