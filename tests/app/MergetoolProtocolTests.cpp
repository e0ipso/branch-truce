// The mergetool protocol on its own: message encoding and validation, framing, the session's
// listener against misbehaving peers, socket and intake paths, and the launcher's file restore.
// No Git, launcher or window runs here.
#include "TestHarness.h"
#include "fixtures/EventLoop.h"
#include "fixtures/Files.h"
#include "mergetool/MergetoolFiles.h"
#include "mergetool/MergetoolIntake.h"
#include "mergetool/MergetoolMessages.h"
#include "mergetool/MergetoolServer.h"
#include "mergetool/MergetoolSocket.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <csignal>
#include <memory>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace branch_truce;
using test_support::readFile;
using test_support::waitUntil;
using test_support::writeFile;

namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

git::GitEnvironment configuration()
{
    return {QStringLiteral("/home/user"),
            QStringLiteral("/home/user/.config"),
            QStringLiteral("/home/user/global"),
            {},
            QStringLiteral("1"),
            QStringLiteral("'core.autocrlf'='true'"),
            {{QStringLiteral("core.eol"), QStringLiteral("lf")}}};
}

QByteArray frameOf(const MergetoolMessage &message) { return mergetoolFrame(encodeMergetoolMessage(message)); }

QByteArray lengthPrefix(quint32 length)
{
    QByteArray prefix;
    for (const int shift : {24, 16, 8, 0}) prefix.append(static_cast<char>((length >> shift) & 0xffU));
    return prefix;
}

// A private directory short enough for a socket path.
class SocketDirectory {
public:
    SocketDirectory()
    {
        CHECK(directory_.isValid());
        CHECK(QFile::setPermissions(directory_.path(),
                                    QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    }
    QString path() const { return directory_.path(); }
    QString socket() const { return directory_.path() + QStringLiteral("/s"); }

private:
    QTemporaryDir directory_{QStringLiteral("/tmp/bt-proto-XXXXXX")};
};

// A launcher stand-in that records what the session sends and when it closes.
class Peer {
public:
    explicit Peer(const QString &name)
    {
        QObject::connect(&socket, &QLocalSocket::disconnected, &socket, [this] { closed = true; });
        QObject::connect(&socket, &QLocalSocket::readyRead, &socket, [this] { received += socket.readAll(); });
        socket.connectToServer(name);
        waitUntil([this] { return closed || socket.state() == QLocalSocket::ConnectedState; }, "peer connect");
    }
    void write(const QByteArray &bytes)
    {
        CHECK(socket.write(bytes) == bytes.size());
        CHECK(socket.flush() || socket.bytesToWrite() == 0);
    }
    // The next complete message the session sent.
    MergetoolMessage next()
    {
        QByteArray payload;
        waitUntil([&] { return takeMergetoolFrame(&received, &payload) == MergetoolFrame::Ready; },
                  "a message from the session");
        const auto decoded = decodeMergetoolMessage(payload);
        CHECK(decoded.message.has_value());
        return *decoded.message;
    }
    void waitClosed()
    {
        waitUntil([this] { return closed; }, "the session closes the connection");
    }

    QLocalSocket socket;
    QByteArray received;
    bool closed = false;
};

struct Attaches {
    explicit Attaches(MergetoolServer &server)
    {
        QObject::connect(&server, &MergetoolServer::attachReceived, &server,
                         [this](quint64 connection, const AttachMessage &message) {
                             connections.append(connection);
                             messages.append(message);
                         });
        QObject::connect(&server, &MergetoolServer::closed, &server,
                         [this](quint64 connection) { closed.append(connection); });
    }
    QList<quint64> connections;
    QList<AttachMessage> messages;
    QList<quint64> closed;
};

// Every file write fails with EFBIG while this is alive; truncation still works.
class NoFileGrowth {
public:
    NoFileGrowth()
    {
        previousHandler = std::signal(SIGXFSZ, SIG_IGN);
        CHECK(::getrlimit(RLIMIT_FSIZE, &previous) == 0);
        auto limited = previous;
        limited.rlim_cur = 0;
        CHECK(::setrlimit(RLIMIT_FSIZE, &limited) == 0);
    }
    ~NoFileGrowth()
    {
        ::setrlimit(RLIMIT_FSIZE, &previous);
        std::signal(SIGXFSZ, previousHandler);
    }
    NoFileGrowth(const NoFileGrowth &) = delete;
    NoFileGrowth &operator=(const NoFileGrowth &) = delete;

private:
    struct rlimit previous{};
    void (*previousHandler)(int) = nullptr;
};

ino_t inodeOf(const QString &path)
{
    struct stat status{};
    CHECK(::stat(QFile::encodeName(path).constData(), &status) == 0);
    return status.st_ino;
}

void everyMessageRoundTrips()
{
    const auto invocation = uuid();
    const QList<std::pair<const char *, MergetoolMessage>> rows{
        {"attach", AttachMessage{invocation, QStringLiteral("/run/user/1/branch-truce/intake/k/i"), configuration()}},
        {"attached", AttachedMessage{invocation}},
        {"rejected", RejectedMessage{invocation, QStringLiteral("Git is already waiting on this path.")}},
        {"rejected without invocation", RejectedMessage{{}, QStringLiteral("unreadable attach")}},
        {"unsupported", UnsupportedMessage{}},
        {"unsupported from another version", UnsupportedMessage{kMergetoolProtocolVersion + 7}},
        {"resolved", ResolvedMessage{invocation, 42, QByteArray(40, 'a')}},
        {"resolved sha256", ResolvedMessage{invocation, 1, QByteArray(64, 'f')}},
        {"leave", LeaveMessage{invocation}}};
    for (const auto &[name, message] : rows) {
        SCOPE(name);
        const auto decoded = decodeMergetoolMessage(encodeMergetoolMessage(message));
        CHECK(decoded.failure == MergetoolDecodeFailure::None);
        CHECK(decoded.message && *decoded.message == message);
    }
}

void invalidMessagesAreRefusedWithTheirReason()
{
    const auto invocation = uuid().toLatin1();
    const auto version = QByteArray::number(kMergetoolProtocolVersion);
    const auto other = QByteArray::number(kMergetoolProtocolVersion + 1);
    const QByteArray configuration =
        R"("configuration":{"home":"/h","configHome":"","globalConfig":"","systemConfig":"","noSystem":""})";
    struct Row {
        const char *name;
        QByteArray payload;
        MergetoolDecodeFailure failure;
    };
    const QList<Row> rows{
        {"not JSON", "attach", MergetoolDecodeFailure::Malformed},
        {"an array", "[1,2]", MergetoolDecodeFailure::Malformed},
        {"no version", R"({"kind":"leave","invocation":")" + invocation + R"("})",
         MergetoolDecodeFailure::OtherVersion},
        {"another version", R"({"v":)" + other + R"(,"kind":"leave","invocation":")" + invocation + R"("})",
         MergetoolDecodeFailure::OtherVersion},
        {"an unknown kind", R"({"v":)" + version + R"(,"kind":"cancel"})", MergetoolDecodeFailure::UnknownKind},
        {"no kind", R"({"v":)" + version + "}", MergetoolDecodeFailure::UnknownKind},
        {"an invocation that is not a UUID", R"({"v":)" + version + R"(,"kind":"leave","invocation":"x"})",
         MergetoolDecodeFailure::Malformed},
        {"a relative intake",
         R"({"v":)" + version + R"(,"kind":"attach","invocation":")" + invocation + R"(","intake":"intake",)"
             + configuration + "}",
         MergetoolDecodeFailure::Malformed},
        {"an attach whose configuration entries are not pairs",
         R"({"v":)" + version + R"(,"kind":"attach","invocation":")" + invocation + R"(","intake":"/i",)"
             + configuration.chopped(1) + R"(,"parameters":"","entries":[["core.eol"]]}})",
         MergetoolDecodeFailure::Malformed},
        {"an attach without its configuration",
         R"({"v":)" + version + R"(,"kind":"attach","invocation":")" + invocation + R"(","intake":"/i"})",
         MergetoolDecodeFailure::Malformed},
        {"a revision of zero",
         R"({"v":)" + version + R"(,"kind":"resolved","invocation":")" + invocation + R"(","revision":"0","object":")"
             + QByteArray(40, 'a') + R"("})",
         MergetoolDecodeFailure::Malformed},
        {"a revision with a sign",
         R"({"v":)" + version + R"(,"kind":"resolved","invocation":")" + invocation + R"(","revision":"+7","object":")"
             + QByteArray(40, 'a') + R"("})",
         MergetoolDecodeFailure::Malformed},
        {"a short object id",
         R"({"v":)" + version + R"(,"kind":"resolved","invocation":")" + invocation
             + R"(","revision":"7","object":"abc"})",
         MergetoolDecodeFailure::Malformed},
        {"a rejection without a reason",
         R"({"v":)" + version + R"(,"kind":"rejected","invocation":")" + invocation + R"("})",
         MergetoolDecodeFailure::Malformed},
        {"an unsupported reply without a version", R"({"kind":"unsupported"})", MergetoolDecodeFailure::Malformed}};
    for (const auto &row : rows) {
        SCOPE(row.name);
        const auto decoded = decodeMergetoolMessage(row.payload);
        CHECK(!decoded.message);
        CHECK(decoded.failure == row.failure);
        CHECK(!decoded.problem.isEmpty());
    }
    const auto unknownField = decodeMergetoolMessage(R"({"v":)" + version + R"(,"kind":"leave","invocation":")"
                                                     + invocation + R"(","later":true})");
    CHECK(unknownField.message && std::holds_alternative<LeaveMessage>(*unknownField.message));
    const auto newer = decodeMergetoolMessage(R"({"v":)" + other + R"(,"kind":"leave"})");
    CHECK_EQ(newer.version, kMergetoolProtocolVersion + 1);
}

void framesSplitJoinAndRefuseBadLengths()
{
    const auto leave = encodeMergetoolMessage(LeaveMessage{uuid()});
    const auto frame = mergetoolFrame(leave);
    CHECK_EQ(frame.size(), leave.size() + 4);
    CHECK(mergetoolFrame({}).isEmpty());
    CHECK(mergetoolFrame(QByteArray(kMergetoolFrameLimit + 1, 'x')).isEmpty());
    CHECK_EQ(mergetoolFrame(QByteArray(kMergetoolFrameLimit, 'x')).size(), kMergetoolFrameLimit + 4);

    struct Row {
        const char *name;
        QByteArray buffer;
        MergetoolFrame expected;
        QByteArray payload;
        qsizetype left;
    };
    const QList<Row> rows{
        {"empty", {}, MergetoolFrame::NeedMore, {}, 0},
        {"part of the length", frame.left(3), MergetoolFrame::NeedMore, {}, 3},
        {"the length only", frame.left(4), MergetoolFrame::NeedMore, {}, 4},
        {"a split payload", frame.left(frame.size() - 1), MergetoolFrame::NeedMore, {}, frame.size() - 1},
        {"one frame", frame, MergetoolFrame::Ready, leave, 0},
        {"two frames", frame + frame, MergetoolFrame::Ready, leave, frame.size()},
        {"zero length", lengthPrefix(0), MergetoolFrame::Invalid, {}, 4},
        {"an oversized length", lengthPrefix(kMergetoolFrameLimit + 1) + "x", MergetoolFrame::Invalid, {}, 5},
        {"the largest length", lengthPrefix(kMergetoolFrameLimit) + QByteArray(kMergetoolFrameLimit, 'y'),
         MergetoolFrame::Ready, QByteArray(kMergetoolFrameLimit, 'y'), 0}};
    for (const auto &row : rows) {
        SCOPE(row.name);
        auto buffer = row.buffer;
        QByteArray payload;
        CHECK(takeMergetoolFrame(&buffer, &payload) == row.expected);
        CHECK(payload == row.payload);
        CHECK_EQ(buffer.size(), row.left);
    }
    // A frame that arrives one byte at a time is taken once it is complete.
    QByteArray buffer;
    QByteArray payload;
    for (qsizetype i = 0; i < frame.size(); ++i) {
        CHECK(takeMergetoolFrame(&buffer, &payload) == MergetoolFrame::NeedMore);
        buffer.append(frame.at(i));
    }
    CHECK(takeMergetoolFrame(&buffer, &payload) == MergetoolFrame::Ready && payload == leave);
    // A complete frame whose payload is not an object is framed but not a message.
    auto array = mergetoolFrame("[]");
    CHECK(takeMergetoolFrame(&array, &payload) == MergetoolFrame::Ready);
    CHECK(decodeMergetoolMessage(payload).failure == MergetoolDecodeFailure::Malformed);
}

void anAttachIsReceivedAndAnsweredWithoutBlocking()
{
    SocketDirectory directory;
    MergetoolServer server;
    QString error;
    CHECK(server.listen(directory.socket(), &error) == MergetoolServer::Listen::Listening);
    Attaches attaches(server);
    Peer peer(directory.socket());
    const AttachMessage attach{uuid(), QStringLiteral("/tmp/intake"), configuration()};
    // The frame arrives in two writes.
    const auto frame = frameOf(attach);
    peer.write(frame.left(5));
    waitUntil([&] { return server.connections() == 1; }, "the session accepts the connection");
    CHECK(attaches.messages.isEmpty());
    peer.write(frame.mid(5));
    waitUntil([&] { return !attaches.messages.isEmpty(); }, "the attach");
    CHECK(attaches.messages.front() == attach);
    CHECK(server.send(attaches.connections.front(), AttachedMessage{attach.invocation}));
    CHECK(std::get<AttachedMessage>(peer.next()).invocation == attach.invocation);
    // An attached launcher only waits; it stays connected without a deadline.
    CHECK(server.send(attaches.connections.front(), LeaveMessage{attach.invocation}));
    server.close(attaches.connections.front());
    CHECK(std::holds_alternative<LeaveMessage>(peer.next()));
    peer.waitClosed();
    waitUntil([&] { return attaches.closed == attaches.connections; }, "closed() for the attach");
    CHECK_EQ(server.connections(), 0);
    CHECK(!server.send(attaches.connections.front(), LeaveMessage{attach.invocation}));
}

void misbehavingPeersAreClosed()
{
    struct Row {
        const char *name;
        QByteArray bytes;
        // The reply the session sends before it closes, if any.
        std::optional<MergetoolMessage> reply;
    };
    const auto invocation = uuid();
    const AttachMessage attach{invocation, QStringLiteral("/tmp/intake"), configuration()};
    const QList<Row> rows{
        {"a stalled peer that sends nothing", {}, std::nullopt},
        {"a stalled peer with half a length", QByteArray(2, '\0'), std::nullopt},
        {"a stalled peer with half a frame", frameOf(attach).left(10), std::nullopt},
        {"a zero length", lengthPrefix(0), std::nullopt},
        {"an oversized length", lengthPrefix(kMergetoolFrameLimit + 1) + QByteArray(64, 'x'), std::nullopt},
        {"a payload that is not an object", mergetoolFrame("[1]"),
         RejectedMessage{{}, QStringLiteral("The mergetool attach message is invalid.")}},
        {"a frame that is not an attach", frameOf(LeaveMessage{invocation}),
         RejectedMessage{{}, QStringLiteral("The mergetool attach message is invalid.")}},
        {"another protocol version", mergetoolFrame(R"({"v":99,"kind":"attach"})"), UnsupportedMessage{}},
        {"an unknown kind",
         mergetoolFrame(R"({"v":)" + QByteArray::number(kMergetoolProtocolVersion) + R"(,"kind":"x"})"),
         UnsupportedMessage{}}};
    for (const auto &row : rows) {
        SCOPE(row.name);
        SocketDirectory directory;
        MergetoolServer server(MergetoolServer::Limits{200, 4}, nullptr);
        QString error;
        CHECK(server.listen(directory.socket(), &error) == MergetoolServer::Listen::Listening);
        Attaches attaches(server);
        Peer peer(directory.socket());
        if (!row.bytes.isEmpty()) peer.write(row.bytes);
        if (row.reply) CHECK(peer.next() == *row.reply);
        peer.waitClosed();
        waitUntil([&] { return server.connections() == 0; }, "the session drops the connection");
        CHECK(attaches.messages.isEmpty());
        CHECK(peer.received.isEmpty());
    }
}

void aByteAfterTheAttachClosesTheConnection()
{
    SocketDirectory directory;
    MergetoolServer server;
    QString error;
    CHECK(server.listen(directory.socket(), &error) == MergetoolServer::Listen::Listening);
    Attaches attaches(server);
    Peer peer(directory.socket());
    peer.write(frameOf(AttachMessage{uuid(), QStringLiteral("/tmp/intake"), configuration()}));
    waitUntil([&] { return !attaches.messages.isEmpty(); }, "the attach");
    peer.write("x");
    peer.waitClosed();
    waitUntil([&] { return attaches.closed == attaches.connections; }, "closed() for the attach");
    CHECK_EQ(server.connections(), 0);
}

void connectionsBeyondTheCapAreClosedAtOnce()
{
    SocketDirectory directory;
    MergetoolServer server(MergetoolServer::Limits{60000, 2}, nullptr);
    QString error;
    CHECK(server.listen(directory.socket(), &error) == MergetoolServer::Listen::Listening);
    Peer first(directory.socket());
    Peer second(directory.socket());
    waitUntil([&] { return server.connections() == 2; }, "two accepted connections");
    Peer third(directory.socket());
    third.waitClosed();
    CHECK_EQ(server.connections(), 2);
    CHECK(!first.closed && !second.closed);
    // A closed slot takes the next connection again.
    first.socket.disconnectFromServer();
    waitUntil([&] { return server.connections() == 1; }, "the first connection's slot is free");
    Peer fourth(directory.socket());
    waitUntil([&] { return server.connections() == 2; }, "the fourth connection is accepted");
    CHECK(!fourth.closed);
}

void aPeerOfAnotherUserIsClosedAtOnce()
{
    SocketDirectory directory;
    MergetoolServer::Limits limits;
    qintptr checked = -1;
    limits.peerIsCurrentUser = [&checked](qintptr descriptor) {
        checked = descriptor;
        return false;
    };
    MergetoolServer server(limits, nullptr);
    QString error;
    CHECK(server.listen(directory.socket(), &error) == MergetoolServer::Listen::Listening);
    Attaches attaches(server);
    Peer peer(directory.socket());
    peer.waitClosed();
    CHECK(checked >= 0);
    CHECK_EQ(server.connections(), 0);
    CHECK(attaches.messages.isEmpty() && attaches.connections.isEmpty());
    CHECK(peer.received.isEmpty());
}

void oneListenerOwnsTheSocketAndAStaleOneIsReplaced()
{
    SocketDirectory directory;
    {
        MergetoolServer first;
        MergetoolServer second;
        QString error;
        CHECK(first.listen(directory.socket(), &error) == MergetoolServer::Listen::Listening);
        CHECK(second.listen(directory.socket(), &error) == MergetoolServer::Listen::Elsewhere);
        CHECK(!second.listening());
        // The second window did not unlink the first one's socket.
        Peer peer(directory.socket());
        waitUntil([&] { return first.connections() == 1; }, "the first listener still accepts");
    }
    CHECK(!QFileInfo::exists(directory.socket()));
    // A window that crashed leaves its socket file and no live lock owner.
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto native = QFile::encodeName(directory.socket());
    std::copy(native.cbegin(), native.cend(), address.sun_path);
    CHECK(::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    ::close(fd);
    CHECK(QFileInfo::exists(directory.socket()));
    MergetoolServer replacement;
    QString error;
    CHECK(replacement.listen(directory.socket(), &error) == MergetoolServer::Listen::Listening);
    Peer peer(directory.socket());
    waitUntil([&] { return replacement.connections() == 1; }, "the replacement accepts");
}

void theSocketDirectoryIgnoresTemporaryDirectories()
{
    const auto fallback = mergetoolSocketDirectory({});
    // The fallback directory and its link go when the case ends; the private cache holds the link.
    const auto removeFallback = qScopeGuard([&fallback] { ::rmdir(QFile::encodeName(fallback).constData()); });
    git::RepositoryContext repository;
    repository.operation = git::RepositoryOperation::Merge;
    repository.supported = true;
    repository.worktreeRoot = QStringLiteral("/srv/worktree");
    repository.stage2.commitId = QByteArray(40, 'a');
    repository.stage3.commitId = QByteArray(40, 'b');
    const auto shortDirectory = mergetoolSocketDirectory("/run/user/7/");
    CHECK_EQ(shortDirectory, QStringLiteral("/run/user/7/branch-truce"));
    CHECK_EQ(mergetoolSocketDirectory("/run/user/7"), shortDirectory);
    const auto shortSocket = mergetoolServerName(shortDirectory, repository);
    CHECK(shortSocket.startsWith(shortDirectory + QStringLiteral("/bt-mt-")));
    CHECK_EQ(shortSocket.size(), shortDirectory.size() + 1 + 6 + 32);
    // The longest runtime directory whose socket path stays within Qt's 106 bytes.
    const auto boundary = QByteArray("/r") + QByteArray(52, 'x');
    const auto fitDirectory = mergetoolSocketDirectory(boundary);
    CHECK_EQ(fitDirectory, QString::fromLatin1(boundary) + QStringLiteral("/branch-truce"));
    CHECK_EQ(QFile::encodeName(mergetoolServerName(fitDirectory, repository)).size(), 106);
    CHECK_EQ(mergetoolSocketDirectory(boundary + 'x'), fallback);
    CHECK_EQ(mergetoolSocketDirectory("run/user/7"), fallback);
    CHECK(QFile::encodeName(mergetoolServerName(fallback, repository)).size() <= 106);
    CHECK_EQ(mergetoolIntakeRoot(shortDirectory), shortDirectory + QStringLiteral("/intake"));
    repository.supported = false;
    CHECK(mergetoolServerName(shortDirectory, repository).isEmpty());
}

// Without a usable runtime directory, every process of this user meets in one /tmp directory that
// another user cannot name in advance, and a missing one is made again.
void theFallbackSocketDirectoryIsPrivateAndUnpredictable()
{
    const auto prefix = QStringLiteral("/tmp/branch-truce-%1-").arg(::geteuid());
    const auto first = mergetoolSocketDirectory({});
    auto removeFirst = qScopeGuard([&first] { ::rmdir(QFile::encodeName(first).constData()); });
    CHECK(first.startsWith(prefix) && first.size() == prefix.size() + 6);
    CHECK_EQ(mergetoolSocketDirectory({}), first);
    struct stat info{};
    CHECK(::lstat(QFile::encodeName(first).constData(), &info) == 0 && S_ISDIR(info.st_mode));
    CHECK(info.st_uid == ::geteuid() && (info.st_mode & 077) == 0);
    Problem error;
    CHECK(prepareMergetoolSocketDirectory(first, &error));
    // A directory that went away, as /tmp does at boot, is replaced by a new one.
    CHECK(::rmdir(QFile::encodeName(first).constData()) == 0);
    removeFirst.dismiss();
    const auto second = mergetoolSocketDirectory({});
    const auto removeSecond = qScopeGuard([&second] { ::rmdir(QFile::encodeName(second).constData()); });
    CHECK(second.startsWith(prefix) && second != first);
    // A directory that is no longer private is not used either.
    CHECK(::chmod(QFile::encodeName(second).constData(), 0755) == 0);
    const auto third = mergetoolSocketDirectory({});
    const auto removeThird = qScopeGuard([&third] { ::rmdir(QFile::encodeName(third).constData()); });
    CHECK(third.startsWith(prefix) && third != second);
    CHECK(!prepareMergetoolSocketDirectory({}, &error));
    CHECK(error.code == ProblemCode::MergetoolNoSocketDirectory);
}

void intakesArePrivateCheckedAndRemoved()
{
    SocketDirectory directory;
    const auto root = mergetoolIntakeRoot(directory.path());
    const auto key = QByteArray(64, 'c');
    MergetoolSources sources;
    sources.path = "nested/tracked.txt";
    sources.merged = "merged\n";
    sources.local = "local\n";
    sources.localPresent = true;
    const auto invocation = uuid();
    Problem error;
    const auto intake = writeMergetoolIntake(root, key, invocation, sources, &error);
    CHECK(!intake.isEmpty() && error.isEmpty());
    for (const auto &path : {root, QFileInfo(intake).absolutePath(), intake}) {
        SCOPE(path.toStdString());
        const auto others = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
            | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
        CHECK(!(QFileInfo(path).permissions() & others));
    }
    const auto read = readMergetoolIntake(intake, root, key, &error);
    CHECK(read && read->invocation == invocation && read->path == sources.path);
    CHECK(read->localPresent && !read->basePresent && !read->remotePresent);
    CHECK(!readMergetoolIntake(intake, root, QByteArray(64, 'd'), &error));
    CHECK(!readMergetoolIntake(intake, directory.path(), key, &error));
    writeFile(intake + QStringLiteral("/merged"), "changed\n");
    CHECK(!readMergetoolIntake(intake, root, key, &error));
    CHECK(error.code == ProblemCode::MergetoolIntakeBytes);
    // This process wrote it and is alive, so a prune keeps it.
    CHECK_EQ(pruneMergetoolIntakes(root, key), 0);
    removeMergetoolIntake(intake);
    CHECK(!QFileInfo::exists(intake) && !QFileInfo::exists(QFileInfo(intake).absolutePath()));

    // An intake whose launcher has ended goes at the next listen.
    const auto orphan = writeMergetoolIntake(root, key, uuid(), sources, &error);
    CHECK(!orphan.isEmpty());
    QProcess ended;
    ended.start(QStringLiteral("/bin/true"));
    CHECK(ended.waitForStarted(5000));
    const auto endedPid = ended.processId();
    CHECK(ended.waitForFinished(5000));
    writeFile(orphan + QStringLiteral("/launcher"), QByteArray::number(endedPid) + " 1\n");
    const auto live = writeMergetoolIntake(root, key, uuid(), sources, &error);
    CHECK(!live.isEmpty());
    // A later process with the launcher's PID does not keep its intake: this process's PID with
    // another start time stands for one.
    const auto reused = writeMergetoolIntake(root, key, uuid(), sources, &error);
    CHECK(!reused.isEmpty());
    const auto owner = readFile(reused + QStringLiteral("/launcher")).trimmed().split(' ');
    CHECK(owner.size() == 2 && owner[0] == QByteArray::number(QCoreApplication::applicationPid()));
    writeFile(reused + QStringLiteral("/launcher"),
              owner[0] + ' ' + QByteArray::number(owner[1].toLongLong() + 1) + '\n');
    CHECK_EQ(pruneMergetoolIntakes(root, key), 2);
    CHECK(!QFileInfo::exists(orphan) && !QFileInfo::exists(reused) && QFileInfo::exists(live));
}

void aRestoreReplacesTheFileWholeAndKeepsItsMode()
{
    QTemporaryDir directory;
    CHECK(directory.isValid());
    const auto path = QFileInfo(directory.path()).canonicalFilePath() + QStringLiteral("/merged.txt");
    const auto native = QFile::encodeName(path);
    const QByteArray launch("<<<<<<< ours\nlaunch\n=======\nother\n>>>>>>> theirs\n");
    const auto longer = launch + QByteArray("a longer GUI write\n").repeated(64);
    Problem error;
    writeFile(path, longer);
    CHECK(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    const auto before = inodeOf(path);
    CHECK(restoreMergetoolFile(native, launch, &error) && error.isEmpty());
    CHECK(readFile(path) == launch);
    // A rename put a whole new file in place, with the old permission bits.
    CHECK(inodeOf(path) != before);
    CHECK(QFileInfo(path).permissions().testFlag(QFileDevice::ExeOwner));
    writeFile(path, QByteArray(launch.size(), 'e'));
    CHECK(restoreMergetoolFile(native, launch, &error) && readFile(path) == launch);
    writeFile(path, "short\n");
    CHECK(restoreMergetoolFile(native, longer, &error) && readFile(path) == longer);
    // Equal bytes leave the file alone.
    const auto unchanged = inodeOf(path);
    CHECK(restoreMergetoolFile(native, longer, &error) && inodeOf(path) == unchanged);
    writeFile(path, longer);
    bool restored = true;
    {
        NoFileGrowth limit;
        restored = restoreMergetoolFile(native, launch, &error);
    }
    CHECK(!restored);
    CHECK(error.code == ProblemCode::MergetoolRestoreFailed);
    CHECK(readFile(path) == longer);
    CHECK_EQ(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden), QStringList{QStringLiteral("merged.txt")});
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    test_support::configure(argc, argv);
    test_support::Suite suite("MergetoolProtocolTests");
    suite.run("every message kind round-trips through its encoding", everyMessageRoundTrips);
    suite.run("invalid messages are refused with their reason", invalidMessagesAreRefusedWithTheirReason);
    suite.run("frames split, join and refuse bad lengths", framesSplitJoinAndRefuseBadLengths);
    suite.run("an attach is received and answered without blocking", anAttachIsReceivedAndAnsweredWithoutBlocking);
    suite.run("misbehaving and stalled peers are closed", misbehavingPeersAreClosed);
    suite.run("a byte after the attach closes the connection", aByteAfterTheAttachClosesTheConnection);
    suite.run("connections beyond the cap are closed at once", connectionsBeyondTheCapAreClosedAtOnce);
    suite.run("a peer of another user is closed at once", aPeerOfAnotherUserIsClosedAtOnce);
    suite.run("one listener owns the socket and a stale one is replaced",
              oneListenerOwnsTheSocketAndAStaleOneIsReplaced);
    suite.run("the socket directory ignores temporary directories", theSocketDirectoryIgnoresTemporaryDirectories);
    suite.run("the fallback socket directory is private and unpredictable",
              theFallbackSocketDirectoryIsPrivateAndUnpredictable);
    suite.run("intakes are private, checked and removed", intakesArePrivateCheckedAndRemoved);
    suite.run("a restore replaces the file whole and keeps its mode", aRestoreReplacesTheFileWholeAndKeepsItsMode);
    return suite.report();
}
