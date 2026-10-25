#pragma once

#include "git/GitAdapter.h"
#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

class QLocalServer;
class QLocalSocket;
class QTimer;

namespace branch_truce {
class ApplicationController;

// Listens for blocking mergetool launchers and keeps their temporary inputs.
// Exit success is sent only after Mark resolved has written the launched path.
class MergetoolSession final : public QObject {
    Q_OBJECT
public:
    // Elsewhere: another live session already owns this operation's socket.
    // Unavailable: no launcher can attach to this session; error says why.
    enum class Listen { Listening, Elsewhere, Unavailable };
    explicit MergetoolSession(ApplicationController *controller);
    Listen listen(const git::RepositoryContext &repository, QString *error);
    [[nodiscard]] bool listening() const { return listening_; }
    [[nodiscard]] bool parentSessionActive() const { return parentSession_; }
    [[nodiscard]] bool blocks(const QByteArray &path) const;
    [[nodiscard]] bool waitingOn(const QByteArray &path) const;
    [[nodiscard]] QByteArray waitingPath() const;
    [[nodiscard]] bool hasWaiter(const QByteArray &path) const;
    [[nodiscard]] bool canEndParentSession() const;
    [[nodiscard]] QStringList intakes() const { return intakes_; }
    bool leave(const QByteArray &path);
    // Launched paths whose launcher has not received a decision yet.
    [[nodiscard]] int unreleasedLaunches() const;
    // Deliberate session leave: every waiting launcher gets an explicit leave,
    // restores its original file and exits unresolved. Returns how many were told.
    int leaveAll();
    bool endParentSession();
    void onPublisherChanged();

Q_SIGNALS:
    void notice(const QString &message);
    void changed();

private:
    struct Waiter {
        QString invocation;
        QByteArray path;
        QString intake;
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

    ApplicationController *controller_ = nullptr;
    QLocalServer *server_ = nullptr;
    QTimer *acknowledgment_ = nullptr;
    git::RepositoryContext repository_;
    QByteArray sessionKey_;
    QList<Waiter> waiters_;
    QHash<QLocalSocket *, QByteArray> buffers_;
    QStringList intakes_;
    QString acknowledgmentFile_;
    QString acknowledgmentRevision_;
    int acknowledgmentAttempts_ = 0;
    bool listening_ = false;
    bool parentSession_ = false;
    bool offerHeld_ = false;
    bool assigning_ = false;
};
}
