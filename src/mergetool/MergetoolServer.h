// The mergetool server is the session's end of the launcher socket, with a guarded listener and checked peers.
#pragma once

#include "mergetool/MergetoolMessages.h"
#include "mergetool/MergetoolSocket.h"
#include <QByteArray>
#include <QDeadlineTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>

class QLocalServer;
class QLocalSocket;
class QLockFile;
class QTimer;

namespace branch_truce {

// Listens on one private socket per operation. A lock file next to the socket
// keeps a second window from replacing a live listener. Peers must run as
// this user, send their attach within a deadline and stay within a connection
// cap. Writes are queued and never wait on the GUI thread. Use on the owning
// thread only.
class MergetoolServer final : public QObject {
    Q_OBJECT
public:
    // Elsewhere: another live window already listens for this operation.
    // Unavailable: no launcher can attach here; error says why.
    enum class Listen { Listening, Elsewhere, Unavailable };
    struct Limits {
        // Total time from connection through intake validation; expiry closes the peer.
        int attachDeadlineMs = 5000;
        // Connections open at once, attached launchers included. Others are closed at once.
        int maxConnections = 32;
        // Whether the process at the other end of a connection runs as this user. Other
        // peers are closed before anything is read from them.
        std::function<bool(qintptr descriptor)> peerIsCurrentUser = mergetoolPeerIsCurrentUser;
    };
    explicit MergetoolServer(QObject *parent = nullptr);
    MergetoolServer(Limits limits, QObject *parent);
    ~MergetoolServer() override;

    // name is the socket path; its directory must already be private.
    Listen listen(const QString &name, QString *error);
    [[nodiscard]] bool listening() const { return server_ != nullptr; }
    // Queues one frame. False when the connection is already gone. True does
    // not mean the peer read it: the caller treats delivery as unknown.
    bool send(quint64 connection, const MergetoolMessage &message);
    // Closes once the queued frames are written. closed() follows.
    void close(quint64 connection);
    [[nodiscard]] int connections() const { return static_cast<int>(connections_.size()); }

Q_SIGNALS:
    // A peer sent a valid attach. Its deadline runs until Attached is sent; any
    // further byte closes it. A late Attached is refused even before timer delivery.
    void attachReceived(quint64 connection, const branch_truce::AttachMessage &message);
    // The connection is gone, whichever side closed it.
    void closed(quint64 connection);

private:
    struct Connection {
        QLocalSocket *socket = nullptr;
        QTimer *deadline = nullptr;
        QByteArray buffer;
        QString invocation;
        QDeadlineTimer expires;
    };
    void accept();
    void read(quint64 connection);
    void drop(quint64 connection);
    void refuse(quint64 connection, const MergetoolMessage &reply);

    Limits limits_;
    QLocalServer *server_ = nullptr;
    std::unique_ptr<QLockFile> lock_;
    QHash<quint64, Connection> connections_;
    quint64 nextConnection_ = 1;
};
}
