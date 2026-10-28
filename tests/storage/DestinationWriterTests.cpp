#include "storage/DestinationWriter.h"
#include "TestHarness.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <functional>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

using namespace branch_truce::storage;

namespace {
struct Interception;
Interception *active = nullptr;

struct Interception {
    Interception() { CHECK(!active); active = this; }
    ~Interception() { active = nullptr; }
    Interception(const Interception &) = delete;
    Interception &operator=(const Interception &) = delete;
    int writes = 0;
    int syncs = 0;
    int exchanges = 0;
    int failSync = 0;
    int exchangeError = 0;
    bool exchangeBeforeError = false;
    bool interruptWrite = false;
    bool shortWrites = false;
    bool zeroWrite = false;
    bool fullDisk = false;
    bool failUnlink = false;
    bool foreignIdentity = false;
    std::function<void()> beforeExchange;
    std::function<void()> afterExchange;
};

void writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly));
    CHECK(file.write(bytes) == bytes.size());
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly));
    const auto bytes = file.readAll();
    CHECK(file.error() == QFileDevice::NoError);
    return bytes;
}

class Fixture {
public:
    Fixture()
    {
        CHECK(directory.isValid());
        root = QFileInfo(directory.path()).canonicalFilePath();
        writeFile(path(), original);
        CHECK(::chmod(native().constData(), 0644) == 0);
    }
    QString path() const { return root + "/result.txt"; }
    QByteArray native() const { return QFile::encodeName(path()); }
    DestinationSnapshot capture() const
    {
        const auto observed = captureDestination(root, "result.txt");
        CHECK(observed.destination && observed.failure == DestinationFailure::None);
        CHECK(observed.destination->bytes() == original);
        return *observed.destination;
    }
    void clean() const
    {
        CHECK(QDir(root).entryList({".branch-truce-publish-*"}, QDir::Dirs | QDir::Hidden).isEmpty());
    }
    QTemporaryDir directory;
    QString root;
    const QByteArray original = "original manual result\n";
    const QByteArray edited = "reviewed UTF-8 \xc3\xa9\n";
};

void unchanged(const Fixture &fixture, const PublicationResult &result, DestinationFailure failure)
{
    CHECK(result.state == PublicationState::Unchanged && result.failure == failure);
    CHECK(!result.destination && !result.diagnostic.isEmpty() && result.retainedPath.isEmpty());
    CHECK(readFile(fixture.path()) == fixture.original);
    fixture.clean();
}

void retained(const Fixture &fixture, const PublicationResult &result, const QByteArray &previous)
{
    CHECK(result.state == PublicationState::Uncertain && result.failure != DestinationFailure::None);
    CHECK(!result.destination && !result.diagnostic.isEmpty() && !result.retainedPath.isEmpty());
    CHECK(readFile(QFile::decodeName(result.retainedPath)) == previous);
    CHECK(readFile(fixture.path()) == fixture.edited);
}
} // namespace

extern "C" {
ssize_t __real_write(int, const void *, size_t);
int __real_fsync(int);
int __real_renameat2(int, const char *, int, const char *, unsigned int);
int __real_unlinkat(int, const char *, int);
uid_t __real_geteuid();

ssize_t __wrap_write(int fd, const void *bytes, size_t size)
{
    if (active) {
        ++active->writes;
        if (active->interruptWrite && active->writes == 1) { errno = EINTR; return -1; }
        if (active->zeroWrite) return 0;
        if (active->fullDisk) { errno = ENOSPC; return -1; }
        if (active->shortWrites) size = std::min(size, size_t{3});
    }
    return __real_write(fd, bytes, size);
}
int __wrap_fsync(int fd)
{
    if (active && ++active->syncs == active->failSync) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
int __wrap_renameat2(int source, const char *from, int destination, const char *to, unsigned int flags)
{
    if (active) {
        ++active->exchanges;
        CHECK(flags == RENAME_EXCHANGE);
        if (active->beforeExchange) active->beforeExchange();
        if (active->exchangeError && !active->exchangeBeforeError) { errno = active->exchangeError; return -1; }
    }
    const auto result = __real_renameat2(source, from, destination, to, flags);
    if (active && result == 0) {
        if (active->afterExchange) active->afterExchange();
        if (active->exchangeError) { errno = active->exchangeError; return -1; }
    }
    return result;
}
int __wrap_unlinkat(int fd, const char *name, int flags)
{
    if (active && active->failUnlink) { errno = EACCES; return -1; }
    return __real_unlinkat(fd, name, flags);
}
uid_t __wrap_geteuid()
{
    const auto real = __real_geteuid();
    return active && active->foreignIdentity ? real + 1 : real;
}
}

class DestinationWriterTests {
public:
    int run()
    {
        test_support::Suite suite("DestinationWriterTests");
        suite.run("publication preserves bytes and permissions and supplies the next baseline", [] {
            for (const bool executable : {false, true}) {
                Fixture fixture;
                CHECK(::chmod(fixture.native().constData(), executable ? 0750 : 0640) == 0);
                const auto baseline = fixture.capture();
                const auto mode = executable ? "100755" : "100644";
                int admissions = 0;
                const auto result = publishDestination(baseline, fixture.edited, mode, [&] { ++admissions; return true; });
                CHECK(admissions == 1 && result.state == PublicationState::Published);
                CHECK(result.failure == DestinationFailure::None && result.destination && result.retainedPath.isEmpty());
                CHECK(result.destination->bytes() == fixture.edited && baseline.bytes() == fixture.original);
                CHECK(readFile(fixture.path()) == fixture.edited);
                struct stat metadata {};
                CHECK(::stat(fixture.native().constData(), &metadata) == 0);
                CHECK((metadata.st_mode & 0777) == (executable ? 0750 : 0640));
                const auto next = publishDestination(*result.destination, {}, mode, [] { return true; });
                CHECK(next.state == PublicationState::Published && next.destination && next.destination->bytes().isEmpty());
                CHECK(readFile(fixture.path()).isEmpty());
                fixture.clean();
            }
        });
        suite.run("unsafe paths and symlinks are rejected before capture", [] {
            Fixture fixture;
            for (const auto &path : QList<QByteArray>{"", "/result.txt", "../result.txt", "./result.txt", "a//b", ".git/config", QByteArray("a\0b", 3)}) {
                const auto result = captureDestination(fixture.root, path);
                CHECK(!result.destination && result.failure == DestinationFailure::InvalidPath);
            }
            const auto link = QFile::encodeName(fixture.root + "/link");
            CHECK(::symlink(fixture.native().constData(), link.constData()) == 0);
            CHECK(captureDestination(fixture.root, "link").failure == DestinationFailure::Symlink);
            CHECK(QDir(fixture.root).mkdir("folder"));
            CHECK(::symlink("folder", QFile::encodeName(fixture.root + "/alias").constData()) == 0);
            CHECK(captureDestination(fixture.root, "alias/file").failure == DestinationFailure::Symlink);
            CHECK(captureDestination(fixture.root + "/alias", "file").failure == DestinationFailure::Symlink);
            CHECK(captureDestination(fixture.root, "folder").failure == DestinationFailure::NotRegularFile);
            CHECK(::mkfifo(QFile::encodeName(fixture.root + "/pipe").constData(), 0600) == 0);
            CHECK(captureDestination(fixture.root, "pipe").failure == DestinationFailure::NotRegularFile);
            CHECK(readFile(fixture.path()) == fixture.original);
        });
        suite.run("admission cancellation and mode disagreement create no publication files", [] {
            Fixture fixture;
            const auto baseline = fixture.capture();
            unchanged(fixture, publishDestination(baseline, fixture.edited, "100644", {}), DestinationFailure::InvalidPath);
            unchanged(fixture, publishDestination(baseline, fixture.edited, "100755", [] { return true; }), DestinationFailure::InvalidPath);
            unchanged(fixture, publishDestination(baseline, fixture.edited, "120000", [] { return true; }), DestinationFailure::InvalidPath);
            unchanged(fixture, publishDestination(baseline, fixture.edited, "100644", [] { return false; }), DestinationFailure::Cancelled);
        });
        suite.run("publication keeps literal non-UTF-8 filename bytes", [] {
            Fixture fixture;
            const auto name = QByteArray(":literal-\n") + char(0xff);
            const auto native = QFile::encodeName(fixture.root) + '/' + name;
            const int fd = ::open(native.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            CHECK(fd >= 0);
            const auto count = ::write(fd, fixture.original.constData(), static_cast<size_t>(fixture.original.size()));
            const auto closed = ::close(fd);
            CHECK(count == fixture.original.size() && closed == 0);
            const auto capture = captureDestination(fixture.root, name);
            CHECK(capture.destination && capture.destination->path() == name);
            const auto result = publishDestination(*capture.destination, fixture.edited, "100644", [] { return true; });
            CHECK(result.state == PublicationState::Published && result.destination && result.destination->path() == name);
            const int read = ::open(native.constData(), O_RDONLY | O_CLOEXEC);
            CHECK(read >= 0);
            QFile file;
            CHECK(file.open(read, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle));
            CHECK(file.readAll() == fixture.edited);
            CHECK(::unlink(native.constData()) == 0); // QTemporaryDir cannot traverse this byte name.
            fixture.clean();
        });
        suite.run("external content and inode replacements never become fresh baselines", [] {
            for (const bool replacement : {false, true}) {
                Fixture fixture;
                const auto baseline = fixture.capture();
                if (replacement) CHECK(QFile::rename(fixture.path(), fixture.root + "/previous.txt"));
                writeFile(fixture.path(), "external\n");
                int admissions = 0;
                const auto result = publishDestination(baseline, fixture.edited, "100644", [&] { ++admissions; return true; });
                CHECK(result.state == PublicationState::Unchanged && admissions == 0);
                CHECK(result.failure == (replacement ? DestinationFailure::PathChanged : DestinationFailure::ContentChanged));
                CHECK(!result.destination && readFile(fixture.path()) == "external\n");
                CHECK(baseline.bytes() == fixture.original);
                fixture.clean();
            }
        });
        suite.run("ancestor replacement and post-admission edits are caught before exchange", [] {
            for (const bool ancestor : {false, true}) {
                Fixture fixture;
                const auto baseline = fixture.capture();
                Interception interception;
                const auto result = publishDestination(baseline, fixture.edited, "100644", [&] {
                    if (ancestor) {
                        CHECK(QDir().rename(fixture.root, fixture.root + "-moved"));
                        CHECK(QDir().mkdir(fixture.root));
                    }
                    writeFile(fixture.path(), "external\n");
                    return true;
                });
                CHECK(result.state == PublicationState::Unchanged && !result.destination);
                CHECK(interception.exchanges == 0 && readFile(fixture.path()) == "external\n");
                CHECK(result.failure == (ancestor ? DestinationFailure::PathChanged : DestinationFailure::ContentChanged));
                if (ancestor) {
                    CHECK(readFile(fixture.root + "-moved/result.txt") == fixture.original);
                    CHECK(QDir(fixture.root + "-moved").removeRecursively());
                }
                fixture.clean();
            }
        });
        suite.run("hardlinks special bits foreign ownership xattrs and ACLs fail closed", [] {
            for (const auto scenario : {"hardlink", "special-bits", "foreign-owner", "xattr", "acl"}) {
                std::cout << "METADATA " << scenario << '\n';
                Fixture fixture;
                if (QByteArray(scenario) == "hardlink")
                    CHECK(::link(fixture.native().constData(), QFile::encodeName(fixture.root + "/alias").constData()) == 0);
                if (QByteArray(scenario) == "special-bits") CHECK(::chmod(fixture.native().constData(), 04644) == 0);
                if (QByteArray(scenario) == "xattr") CHECK(::setxattr(fixture.native().constData(), "user.truce-test", "value", 5, 0) == 0);
                if (QByteArray(scenario) == "acl") {
                    // A real named-user POSIX ACL, independent of the setfacl executable.
                    auto acl = QByteArray::fromHex("0200000001000600ffffffff02000400");
                    const auto otherUser = qToLittleEndian(static_cast<quint32>(::geteuid() + 1));
                    acl.append(reinterpret_cast<const char *>(&otherUser), sizeof(otherUser));
                    acl += QByteArray::fromHex("04000400ffffffff10000400ffffffff20000400ffffffff");
                    CHECK(::setxattr(fixture.native().constData(), "system.posix_acl_access", acl.constData(), static_cast<size_t>(acl.size()), 0) == 0);
                }
                const auto baseline = fixture.capture();
                Interception interception;
                // Inject the effective identity mismatch; no chown privilege is required.
                interception.foreignIdentity = QByteArray(scenario) == "foreign-owner";
                unchanged(fixture, publishDestination(baseline, fixture.edited, "100644", [] { return true; }), DestinationFailure::UnsupportedMetadata);
                CHECK(interception.writes == 0 && interception.exchanges == 0);
            }
        });
        suite.run("read-only destinations and directories reject replacement", [] {
            CHECK(::geteuid() != 0); // This permission check requires an unprivileged test user.
            for (const bool directory : {false, true}) {
                Fixture fixture;
                const auto baseline = fixture.capture();
                const auto target = directory ? QFile::encodeName(fixture.root) : fixture.native();
                CHECK(::chmod(target.constData(), directory ? 0500 : 0444) == 0);
                const auto result = publishDestination(baseline, fixture.edited, "100644", [] { return true; });
                CHECK(::chmod(target.constData(), directory ? 0700 : 0644) == 0);
                CHECK(result.state == PublicationState::Unchanged && !result.destination && !result.diagnostic.isEmpty());
                // A changed permission baseline may be rejected before the write access probe.
                CHECK(result.failure == DestinationFailure::PathChanged || result.failure == DestinationFailure::ContentChanged
                      || result.failure == DestinationFailure::PermissionDenied);
                CHECK(readFile(fixture.path()) == fixture.original);
                fixture.clean();
            }
        });
        suite.run("short writes and EINTR preserve the complete reviewed result", [] {
            Fixture fixture;
            const auto baseline = fixture.capture();
            Interception interception;
            interception.interruptWrite = true;
            interception.shortWrites = true;
            const auto result = publishDestination(baseline, fixture.edited, "100644", [] { return true; });
            CHECK(result.state == PublicationState::Published && result.destination && interception.writes > 2);
            CHECK(readFile(fixture.path()) == fixture.edited && result.destination->bytes() == fixture.edited);
            fixture.clean();
        });
        suite.run("zero writes and ENOSPC leave the original destination intact", [] {
            for (const bool full : {false, true}) {
                Fixture fixture;
                const auto baseline = fixture.capture();
                Interception interception;
                interception.fullDisk = full;
                interception.zeroWrite = !full;
                unchanged(fixture, publishDestination(baseline, fixture.edited, "100644", [] { return true; }), DestinationFailure::IoError);
                CHECK(interception.exchanges == 0);
            }
        });
        suite.run("each sync failure preserves the observed side of exchange", [] {
            for (int failSync = 1; failSync <= 4; ++failSync) {
                Fixture fixture;
                const auto baseline = fixture.capture();
                Interception interception;
                interception.failSync = failSync;
                const auto result = publishDestination(baseline, fixture.edited, "100644", [] { return true; });
                CHECK(interception.syncs == failSync && result.failure == DestinationFailure::IoError);
                if (failSync <= 2) unchanged(fixture, result, DestinationFailure::IoError);
                else retained(fixture, result, fixture.original);
                CHECK(interception.exchanges == (failSync <= 2 ? 0 : 1));
            }
        });
        suite.run("unsupported exchange has no overwriting rename fallback", [] {
            for (const int error : {ENOSYS, EINVAL, EOPNOTSUPP, EXDEV, EACCES}) {
                Fixture fixture;
                const auto baseline = fixture.capture();
                Interception interception;
                interception.exchangeError = error;
                unchanged(fixture, publishDestination(baseline, fixture.edited, "100644", [] { return true; }),
                          error == EACCES ? DestinationFailure::PermissionDenied : DestinationFailure::UnsupportedFilesystem);
                CHECK(interception.exchanges == 1);
            }
        });
        suite.run("lost exchange replies retain both possible outcomes without rollback", [] {
            for (const int error : {EIO, ESTALE, ETIMEDOUT}) {
                for (const bool committed : {false, true}) {
                    Fixture fixture;
                    const auto baseline = fixture.capture();
                    Interception interception;
                    interception.exchangeError = error;
                    interception.exchangeBeforeError = committed;
                    const auto result = publishDestination(baseline, fixture.edited, "100644", [] { return true; });
                    CHECK(result.state == PublicationState::Uncertain && result.failure == DestinationFailure::IoError);
                    CHECK(!result.destination && !result.retainedPath.isEmpty() && interception.exchanges == 1);
                    CHECK(readFile(fixture.path()) == (committed ? fixture.edited : fixture.original));
                    CHECK(readFile(QFile::decodeName(result.retainedPath)) == (committed ? fixture.original : fixture.edited));
                }
            }
        });
        suite.run("late displaced edits and replacements require reconciliation without rollback", [] {
            for (const bool replacement : {false, true}) {
                Fixture fixture;
                const auto baseline = fixture.capture();
                Interception interception;
                interception.beforeExchange = [&] {
                    if (replacement) CHECK(QFile::rename(fixture.path(), fixture.root + "/saved-original"));
                    writeFile(fixture.path(), "external late edit\n");
                };
                const auto result = publishDestination(baseline, fixture.edited, "100644", [] { return true; });
                retained(fixture, result, "external late edit\n");
                CHECK(interception.exchanges == 1);
                CHECK(result.failure == (replacement ? DestinationFailure::PathChanged : DestinationFailure::ContentChanged));
                CHECK(baseline.bytes() == fixture.original);
                if (replacement) CHECK(readFile(fixture.root + "/saved-original") == fixture.original);
            }
        });
        suite.run("external edits after exchange survive without restoring the old result", [] {
            Fixture fixture;
            const auto baseline = fixture.capture();
            Interception interception;
            interception.afterExchange = [&] { writeFile(fixture.path(), "newer external edit\n"); };
            const auto result = publishDestination(baseline, fixture.edited, "100644", [] { return true; });
            CHECK(result.state == PublicationState::Uncertain && result.failure == DestinationFailure::ContentChanged);
            CHECK(!result.destination && interception.exchanges == 1);
            CHECK(readFile(fixture.path()) == "newer external edit\n");
            CHECK(readFile(QFile::decodeName(result.retainedPath)) == fixture.original);
        });
        suite.run("a late symlink is retained without following its target or rolling back", [] {
            Fixture fixture;
            writeFile(fixture.root + "/outside.txt", "external target\n");
            const auto baseline = fixture.capture();
            Interception interception;
            interception.beforeExchange = [&] {
                CHECK(QFile::remove(fixture.path()));
                CHECK(::symlink("outside.txt", fixture.native().constData()) == 0);
            };
            const auto result = publishDestination(baseline, fixture.edited, "100644", [] { return true; });
            CHECK(result.state == PublicationState::Uncertain && result.failure == DestinationFailure::Symlink);
            CHECK(!result.destination && interception.exchanges == 1 && !result.retainedPath.isEmpty());
            struct stat metadata {};
            CHECK(::lstat(result.retainedPath.constData(), &metadata) == 0 && S_ISLNK(metadata.st_mode));
            CHECK(readFile(fixture.path()) == fixture.edited);
            CHECK(readFile(fixture.root + "/outside.txt") == "external target\n");
        });
        suite.run("a late ancestor replacement retains the exchange in its pinned directory", [] {
            Fixture fixture;
            const auto baseline = fixture.capture();
            const auto moved = fixture.root + "-relocated";
            Interception interception;
            interception.beforeExchange = [&] {
                CHECK(QDir().rename(fixture.root, moved));
                CHECK(QDir().mkdir(fixture.root));
                writeFile(fixture.path(), "replacement directory's external file\n");
            };
            const auto result = publishDestination(baseline, fixture.edited, "100644", [] { return true; });
            CHECK(result.state == PublicationState::Uncertain && result.failure == DestinationFailure::PathChanged);
            CHECK(!result.destination && interception.exchanges == 1);
            const auto originalRoot = QFile::encodeName(fixture.root);
            CHECK(result.retainedPath.startsWith(originalRoot + '/'));
            const auto actualRetained = QFile::encodeName(moved) + result.retainedPath.mid(originalRoot.size());
            CHECK(readFile(QFile::decodeName(actualRetained)) == fixture.original);
            CHECK(readFile(moved + "/result.txt") == fixture.edited);
            CHECK(readFile(fixture.path()) == "replacement directory's external file\n");
            CHECK(QDir(moved).removeRecursively());
            fixture.clean();
        });
        suite.run("cleanup failures retain private entries and never authorize another write", [] {
            for (const bool exchanged : {false, true}) {
                Fixture fixture;
                const auto baseline = fixture.capture();
                Interception interception;
                interception.failUnlink = true;
                if (!exchanged) interception.exchangeError = EOPNOTSUPP;
                const auto result = publishDestination(baseline, fixture.edited, "100644", [] { return true; });
                CHECK(result.state == (exchanged ? PublicationState::Published : PublicationState::Unchanged));
                CHECK(result.failure == DestinationFailure::CleanupFailure && !result.destination && !result.retainedPath.isEmpty());
                CHECK(readFile(fixture.path()) == (exchanged ? fixture.edited : fixture.original));
                CHECK(readFile(QFile::decodeName(result.retainedPath)) == (exchanged ? fixture.original : fixture.edited));
                CHECK(interception.exchanges == 1);
            }
        });
        return suite.report();
    }
};

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    return DestinationWriterTests{}.run();
}
