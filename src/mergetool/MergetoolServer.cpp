// The mergetool server is the session's end of the launcher socket, with a guarded listener and checked peers.
#include "mergetool/MergetoolServer.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QTimer>

namespace branch_truce {
MergetoolServer::MergetoolServer(QObject *parent) : MergetoolServer(Limits{}, parent) {}

MergetoolServer::MergetoolServer(Limits limits, QObject *parent) : QObject(parent), limits_(limits) {}

MergetoolServer::~MergetoolServer()
{
    // Sockets and timers are children and go after this body. Their last
    // signals must not reach a server that is being destroyed.
    for (const auto &connection : std::as_const(connections_)) {
        connection.socket->disconnect(this);
        connection.deadline->disconnect(this);
        // Deleting a socket discards what it has not written, such as the leave frames of a
        // session that ends. Hand them to the kernel first.
        connection.socket->flush();
    }
    // Remove the socket file before the lock that guards it is released.
    if (server_) server_->close();
}

MergetoolServer::Listen MergetoolServer::listen(const QString &name, QString *error)
{
    if (server_) return Listen::Listening;
    const auto unavailable = [error](const QString &reason) {
        if (error) *error = reason;
        return Listen::Unavailable;
    };
    // Whoever holds the lock owns the socket. A lock left by a process that
    // has ended is taken over, and so is the socket file it left.
    auto lock = std::make_unique<QLockFile>(name + QStringLiteral(".lock"));
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0)) {
        if (lock->error() == QLockFile::LockFailedError) return Listen::Elsewhere;
        return unavailable(tr("The session lock %1.lock could not be taken.").arg(name));
    }
    QLocalServer::removeServer(name);
    auto *server = new QLocalServer(this);
    server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server->listen(name)) {
        const auto reason = tr("The session socket %1 could not listen: %2").arg(name, server->errorString());
        delete server;
        return unavailable(reason);
    }
    server_ = server;
    lock_ = std::move(lock);
    connect(server_, &QLocalServer::newConnection, this, &MergetoolServer::accept);
    return Listen::Listening;
}

bool MergetoolServer::send(quint64 connection, const MergetoolMessage &message)
{
    const auto found = connections_.find(connection);
    if (found == connections_.end() || found->socket->state() != QLocalSocket::ConnectedState) return false;
    const auto *attached = std::get_if<AttachedMessage>(&message);
    if (attached && (found->invocation != attached->invocation || found->expires.hasExpired())) return false;
    const auto frame = mergetoolFrame(encodeMergetoolMessage(message));
    if (frame.isEmpty() || found->socket->write(frame) != frame.size()) return false;
    if (attached) found->deadline->stop();
    // Hands the frame to the kernel now when its buffer has room; never waits.
    found->socket->flush();
    return true;
}

void MergetoolServer::close(quint64 connection)
{
    const auto found = connections_.find(connection);
    if (found == connections_.end()) return;
    auto *socket = found->socket;
    // Writes what is queued first. With nothing queued, disconnected() and
    // drop() can run inside this call.
    socket->disconnectFromServer();
    if (socket->state() == QLocalSocket::UnconnectedState) drop(connection);
}

void MergetoolServer::accept()
{
    while (auto *socket = server_->nextPendingConnection()) {
        // The directory is private, but its /tmp fallback is shared: check
        // who connected before reading anything.
        if (connections_.size() >= limits_.maxConnections || !limits_.peerIsCurrentUser
            || !limits_.peerIsCurrentUser(socket->socketDescriptor())) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        const auto id = nextConnection_++;
        socket->setParent(this);
        auto *deadline = new QTimer(this);
        deadline->setSingleShot(true);
        connections_.insert(id, {socket, deadline, {}, {}, QDeadlineTimer(limits_.attachDeadlineMs)});
        connect(socket, &QLocalSocket::readyRead, this, [this, id] { read(id); });
        connect(socket, &QLocalSocket::disconnected, this, [this, id] { drop(id); });
        connect(deadline, &QTimer::timeout, this, [this, id] {
            const auto found = connections_.find(id);
            if (found == connections_.end()) return;
            const auto peer = *found;
            if (!peer.invocation.isEmpty())
                send(id,
                     RejectedMessage{peer.invocation,
                                     tr("The mergetool inputs could not be checked in time. Try again.")});
            // Flush above is best effort. Expiry must also drop a peer that stopped reading.
            peer.socket->abort();
            drop(id);
        });
        deadline->start(limits_.attachDeadlineMs);
        if (socket->bytesAvailable() > 0) read(id);
    }
}

void MergetoolServer::read(quint64 connection)
{
    const auto found = connections_.find(connection);
    if (found == connections_.end()) return;
    auto &peer = *found;
    // A launcher sends one attach frame and then only waits.
    if (!peer.invocation.isEmpty()) {
        peer.socket->abort();
        drop(connection);
        return;
    }
    // Never buffer more than one frame of the largest size.
    peer.buffer += peer.socket->read(4 + kMergetoolFrameLimit - peer.buffer.size());
    QByteArray payload;
    const auto frame = takeMergetoolFrame(&peer.buffer, &payload);
    if (frame == MergetoolFrame::NeedMore) return;
    if (frame == MergetoolFrame::Invalid || !peer.buffer.isEmpty() || peer.socket->bytesAvailable() > 0) {
        peer.socket->abort();
        drop(connection);
        return;
    }
    const auto decoded = decodeMergetoolMessage(payload);
    if (decoded.failure == MergetoolDecodeFailure::OtherVersion
        || decoded.failure == MergetoolDecodeFailure::UnknownKind) {
        refuse(connection, UnsupportedMessage{});
        return;
    }
    if (!decoded.message || !std::holds_alternative<AttachMessage>(*decoded.message)) {
        refuse(connection, RejectedMessage{{}, tr("The mergetool attach message is invalid.")});
        return;
    }
    peer.invocation = std::get<AttachMessage>(*decoded.message).invocation;
    Q_EMIT attachReceived(connection, std::get<AttachMessage>(*decoded.message));
}

void MergetoolServer::drop(quint64 connection)
{
    const auto found = connections_.find(connection);
    if (found == connections_.end()) return;
    const auto peer = *found;
    connections_.erase(found);
    peer.socket->disconnect(this);
    peer.deadline->disconnect(this);
    peer.socket->deleteLater();
    peer.deadline->deleteLater();
    Q_EMIT closed(connection);
}

void MergetoolServer::refuse(quint64 connection, const MergetoolMessage &reply)
{
    send(connection, reply);
    close(connection);
}
}
