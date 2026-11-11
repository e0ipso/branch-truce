#include "TestHarness.h"
#include "fixtures/Files.h"
#include "storage/DestinationWriter.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <map>
#include <mutex>
#include <string_view>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <vector>

using namespace branch_truce::storage;
using test_support::readFile;
using test_support::writeFile;

namespace {
// Fails or observes the destination writer's system calls. Each case gives its snapshot one of
// these, so nothing outside the case sees the faults.
class Interception final : public FileSystem {
public:
    mutable std::atomic<int> writes = 0;
    mutable std::atomic<int> syncs = 0;
    mutable std::atomic<int> exchanges = 0;
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

    ssize_t write(int fd, const void *bytes, size_t size) const override
    {
        if (++writes == 1 && interruptWrite) {
            errno = EINTR;
            return -1;
        }
        if (zeroWrite) return 0;
        if (fullDisk) {
            errno = ENOSPC;
            return -1;
        }
        if (shortWrites) size = std::min(size, size_t{3});
        return FileSystem::write(fd, bytes, size);
    }
    int sync(int fd) const override
    {
        if (++syncs == failSync) {
            errno = EIO;
            return -1;
        }
        return FileSystem::sync(fd);
    }
    int exchange(int source, const char *from, int destination, const char *to) const override
    {
        ++exchanges;
        // Inside the code under test nothing may throw, so failures here are recorded.
        if (beforeExchange) test_support::contain(beforeExchange);
        if (exchangeError && !exchangeBeforeError) {
            errno = exchangeError;
            return -1;
        }
        const auto result = FileSystem::exchange(source, from, destination, to);
        if (result == 0) {
            if (afterExchange) test_support::contain(afterExchange);
            if (exchangeError) {
                errno = exchangeError;
                return -1;
            }
        }
        return result;
    }
    int unlink(int fd, const char *name, int flags) const override
    {
        if (failUnlink) {
            errno = EACCES;
            return -1;
        }
        return FileSystem::unlink(fd, name, flags);
    }
    uid_t effectiveUser() const override { return FileSystem::effectiveUser() + (foreignIdentity ? 1 : 0); }
};

// SELinux labels kept per inode, as a system with SELinux enabled reports them. Other attributes
// come from the real file system.
class Labels final : public FileSystem {
public:
    mutable std::mutex mutex;
    mutable std::map<ino_t, QByteArray> values;
    mutable std::atomic<int> sets = 0;
    bool refuseSet = false;

    static ino_t inode(int fd)
    {
        struct stat identity{};
        return ::fstat(fd, &identity) == 0 ? identity.st_ino : 0;
    }
    ssize_t listAttributes(int fd, char *names, size_t size) const override
    {
        static constexpr char listed[] = "security.selinux";
        {
            const std::lock_guard lock(mutex);
            if (!values.contains(inode(fd))) return FileSystem::listAttributes(fd, names, size);
        }
        if (!names) return sizeof(listed);
        if (size < sizeof(listed)) {
            errno = ERANGE;
            return -1;
        }
        std::memcpy(names, listed, sizeof(listed));
        return sizeof(listed);
    }
    ssize_t attribute(int fd, const char *name, void *value, size_t size) const override
    {
        if (std::string_view(name) != "security.selinux") return FileSystem::attribute(fd, name, value, size);
        const std::lock_guard lock(mutex);
        const auto found = values.find(inode(fd));
        if (found == values.end()) {
            errno = ENODATA;
            return -1;
        }
        const auto length = static_cast<size_t>(found->second.size());
        if (!value) return static_cast<ssize_t>(length);
        if (size < length) {
            errno = ERANGE;
            return -1;
        }
        std::memcpy(value, found->second.constData(), length);
        return static_cast<ssize_t>(length);
    }
    int setAttribute(int fd, const char *name, const void *value, size_t size) const override
    {
        ++sets;
        if (refuseSet || std::string_view(name) != "security.selinux") {
            errno = EPERM;
            return -1;
        }
        const std::lock_guard lock(mutex);
        values[inode(fd)] = QByteArray(static_cast<const char *>(value), static_cast<qsizetype>(size));
        return 0;
    }
};

int openDescriptors()
{
    return static_cast<int>(
        QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::System | QDir::Hidden | QDir::NoDotAndDotDot).size());
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
    DestinationSnapshot capture(std::shared_ptr<const FileSystem> files = systemFileSystem()) const
    {
        const auto observed = captureDestination(root, "result.txt", std::move(files));
        CHECK(observed.destination && observed.failure == DestinationFailure::None);
        if (!observed.destination) throw test_support::Failure("capture has no destination");
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
    CHECK(!result.destination && !result.problem.isEmpty() && result.retainedPath.isEmpty());
    CHECK(readFile(fixture.path()) == fixture.original);
    fixture.clean();
}

void retained(const Fixture &fixture, const PublicationResult &result, const QByteArray &previous)
{
    CHECK(result.state == PublicationState::Uncertain && result.failure != DestinationFailure::None);
    CHECK(!result.destination && !result.problem.isEmpty() && !result.retainedPath.isEmpty());
    CHECK(readFile(QFile::decodeName(result.retainedPath)) == previous);
    CHECK(readFile(fixture.path()) == fixture.edited);
}
} // namespace

namespace {
void addDestinationCases(test_support::Suite &suite)
{
    suite.run("publication preserves bytes and permissions and supplies the next baseline", [] {
        for (const bool executable : {false, true}) {
            Fixture fixture;
            CHECK(::chmod(fixture.native().constData(), executable ? 0750 : 0640) == 0);
            const auto baseline = fixture.capture();
            const auto mode = executable ? "100755" : "100644";
            int admissions = 0;
            const auto result = publishDestination(baseline, fixture.edited, mode, [&] {
                ++admissions;
                return true;
            });
            CHECK(admissions == 1 && result.state == PublicationState::Published);
            CHECK(result.failure == DestinationFailure::None && result.destination && result.retainedPath.isEmpty());
            CHECK(result.destination->bytes() == fixture.edited && baseline.bytes() == fixture.original);
            CHECK(readFile(fixture.path()) == fixture.edited);
            struct stat metadata{};
            CHECK(::stat(fixture.native().constData(), &metadata) == 0);
            CHECK((metadata.st_mode & 0777U) == (executable ? 0750U : 0640U));
            const auto next = publishDestination(*result.destination, {}, mode, [] { return true; });
            CHECK(next.state == PublicationState::Published && next.destination && next.destination->bytes().isEmpty());
            CHECK(readFile(fixture.path()).isEmpty());
            fixture.clean();
        }
    });
    suite.run("kept snapshots hold no open descriptors and still publish", [] {
        Fixture fixture;
        const auto before = openDescriptors();
        std::vector<DestinationSnapshot> kept;
        kept.reserve(400);
        // More than a 1024-descriptor limit would allow if each snapshot pinned its path.
        for (int index = 0; index < 400; ++index) kept.push_back(fixture.capture());
        CHECK_EQ(openDescriptors(), before);
        const auto result = publishDestination(kept.back(), fixture.edited, "100644", [] { return true; });
        CHECK(result.state == PublicationState::Published && result.destination);
        CHECK_EQ(openDescriptors(), before);
        CHECK(readFile(fixture.path()) == fixture.edited);
        fixture.clean();
    });
    suite.run("unsafe paths and symlinks are rejected before capture", [] {
        Fixture fixture;
        for (const auto &path : QList<QByteArray>{"", "/result.txt", "../result.txt", "./result.txt", "a//b",
                                                  ".git/config", QByteArray("a\0b", 3)}) {
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
        unchanged(fixture, publishDestination(baseline, fixture.edited, "100644", {}),
                  DestinationFailure::InvalidRequest);
        unchanged(fixture, publishDestination(baseline, fixture.edited, "100755", [] { return true; }),
                  DestinationFailure::InvalidRequest);
        unchanged(fixture, publishDestination(baseline, fixture.edited, "120000", [] { return true; }),
                  DestinationFailure::InvalidRequest);
        unchanged(fixture, publishDestination(baseline, fixture.edited, "100644", [] { return false; }),
                  DestinationFailure::Cancelled);
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
            const auto result = publishDestination(baseline, fixture.edited, "100644", [&] {
                ++admissions;
                return true;
            });
            CHECK(result.state == PublicationState::Unchanged && admissions == 0);
            CHECK(result.failure
                  == (replacement ? DestinationFailure::PathChanged : DestinationFailure::ContentChanged));
            CHECK(!result.destination && readFile(fixture.path()) == "external\n");
            CHECK(baseline.bytes() == fixture.original);
            fixture.clean();
        }
    });
    suite.run("ancestor replacement and post-admission edits are caught before exchange", [] {
        for (const bool ancestor : {false, true}) {
            Fixture fixture;
            const auto interception = std::make_shared<Interception>();
            const auto baseline = fixture.capture(interception);
            const auto result = publishDestination(baseline, fixture.edited, "100644", [&] {
                test_support::contain([&] {
                    if (ancestor) {
                        CHECK(QDir().rename(fixture.root, fixture.root + "-moved"));
                        CHECK(QDir().mkdir(fixture.root));
                    }
                    writeFile(fixture.path(), "external\n");
                });
                return true;
            });
            CHECK(result.state == PublicationState::Unchanged && !result.destination);
            CHECK(interception->exchanges == 0 && readFile(fixture.path()) == "external\n");
            CHECK(result.failure == (ancestor ? DestinationFailure::PathChanged : DestinationFailure::ContentChanged));
            if (ancestor) {
                CHECK(readFile(fixture.root + "-moved/result.txt") == fixture.original);
                CHECK(QDir(fixture.root + "-moved").removeRecursively());
            }
            fixture.clean();
        }
    });
}
void addMetadataAndWriteCases(test_support::Suite &suite)
{
    suite.run("hardlinks special bits foreign ownership xattrs and ACLs fail closed", [] {
        for (const auto scenario : {"hardlink", "special-bits", "foreign-owner", "xattr", "acl"}) {
            SCOPE(scenario);
            Fixture fixture;
            if (QByteArray(scenario) == "hardlink")
                CHECK(::link(fixture.native().constData(), QFile::encodeName(fixture.root + "/alias").constData())
                      == 0);
            if (QByteArray(scenario) == "special-bits") CHECK(::chmod(fixture.native().constData(), 04644) == 0);
            if (QByteArray(scenario) == "xattr")
                CHECK(::setxattr(fixture.native().constData(), "user.truce-test", "value", 5, 0) == 0);
            if (QByteArray(scenario) == "acl") {
                // A real named-user POSIX ACL, independent of the setfacl executable.
                auto acl = QByteArray::fromHex("0200000001000600ffffffff02000400");
                const auto otherUser = qToLittleEndian(static_cast<quint32>(::geteuid() + 1));
                acl.append(reinterpret_cast<const char *>(&otherUser), sizeof(otherUser));
                acl += QByteArray::fromHex("04000400ffffffff10000400ffffffff20000400ffffffff");
                CHECK(::setxattr(fixture.native().constData(), "system.posix_acl_access", acl.constData(),
                                 static_cast<size_t>(acl.size()), 0)
                      == 0);
            }
            const auto interception = std::make_shared<Interception>();
            const auto baseline = fixture.capture(interception);
            // Inject the effective identity mismatch; no chown privilege is required.
            interception->foreignIdentity = QByteArray(scenario) == "foreign-owner";
            unchanged(fixture, publishDestination(baseline, fixture.edited, "100644", [] { return true; }),
                      DestinationFailure::UnsupportedMetadata);
            CHECK(interception->writes == 0 && interception->exchanges == 0);
        }
    });
    suite.run("an SELinux label is allowed and the replacement keeps it", [] {
        Fixture fixture;
        const auto labels = std::make_shared<Labels>();
        const QByteArray label = "unconfined_u:object_r:user_home_t:s0";
        struct stat before{};
        CHECK(::stat(fixture.native().constData(), &before) == 0);
        labels->values.emplace(before.st_ino, label);
        const auto baseline = fixture.capture(labels);
        const auto result = publishDestination(baseline, fixture.edited, "100644", [] { return true; });
        CHECK_EQ(result.failure, DestinationFailure::None);
        CHECK(result.state == PublicationState::Published && result.destination);
        struct stat after{};
        CHECK(::stat(fixture.native().constData(), &after) == 0 && after.st_ino != before.st_ino);
        CHECK_EQ(labels->sets.load(), 1);
        CHECK(labels->values.at(after.st_ino) == label);
        CHECK(readFile(fixture.path()) == fixture.edited);
        fixture.clean();
    });
    suite.run("a label the replacement cannot take leaves the destination unchanged", [] {
        Fixture fixture;
        const auto labels = std::make_shared<Labels>();
        labels->refuseSet = true;
        struct stat before{};
        CHECK(::stat(fixture.native().constData(), &before) == 0);
        labels->values.emplace(before.st_ino, "system_u:object_r:etc_t:s0");
        const auto baseline = fixture.capture(labels);
        unchanged(fixture, publishDestination(baseline, fixture.edited, "100644", [] { return true; }),
                  DestinationFailure::UnsupportedMetadata);
        CHECK_EQ(labels->sets.load(), 1);
    });
    suite.run("read-only destinations and directories reject replacement", [] {
        if (::geteuid() == 0) test_support::skip("root writes to read-only files and directories anyway");
        for (const bool directory : {false, true}) {
            SCOPE(directory ? "read-only directory" : "read-only file");
            Fixture fixture;
            const auto target = directory ? QFile::encodeName(fixture.root) : fixture.native();
            // The baseline already holds the read-only mode, so only the write access check
            // (the file) or creating the private publication directory (the directory) refuses.
            CHECK(::chmod(target.constData(), directory ? 0500 : 0444) == 0);
            const auto baseline = fixture.capture();
            const auto result = publishDestination(baseline, fixture.edited, "100644", [] { return true; });
            CHECK(::chmod(target.constData(), directory ? 0700 : 0644) == 0);
            CHECK(result.state == PublicationState::Unchanged && !result.destination && !result.problem.isEmpty());
            CHECK_EQ(result.failure, DestinationFailure::PermissionDenied);
            CHECK(readFile(fixture.path()) == fixture.original);
            fixture.clean();
        }
    });
    suite.run("short writes and EINTR preserve the complete reviewed result", [] {
        Fixture fixture;
        const auto interception = std::make_shared<Interception>();
        const auto baseline = fixture.capture(interception);
        interception->interruptWrite = true;
        interception->shortWrites = true;
        const auto result = publishDestination(baseline, fixture.edited, "100644", [] { return true; });
        CHECK(result.state == PublicationState::Published && result.destination && interception->writes > 2);
        CHECK(readFile(fixture.path()) == fixture.edited && result.destination->bytes() == fixture.edited);
        fixture.clean();
    });
    suite.run("zero writes and ENOSPC leave the original destination intact", [] {
        for (const bool full : {false, true}) {
            Fixture fixture;
            const auto interception = std::make_shared<Interception>();
            const auto baseline = fixture.capture(interception);
            interception->fullDisk = full;
            interception->zeroWrite = !full;
            unchanged(fixture, publishDestination(baseline, fixture.edited, "100644", [] { return true; }),
                      DestinationFailure::IoError);
            CHECK(interception->exchanges == 0);
        }
    });
}
void addExchangeAndCleanupCases(test_support::Suite &suite)
{
    suite.run("each sync failure preserves the observed side of exchange", [] {
        for (int failSync = 1; failSync <= 4; ++failSync) {
            Fixture fixture;
            const auto interception = std::make_shared<Interception>();
            const auto baseline = fixture.capture(interception);
            interception->failSync = failSync;
            const auto result = publishDestination(baseline, fixture.edited, "100644", [] { return true; });
            CHECK(interception->syncs == failSync && result.failure == DestinationFailure::IoError);
            if (failSync <= 2)
                unchanged(fixture, result, DestinationFailure::IoError);
            else
                retained(fixture, result, fixture.original);
            CHECK(interception->exchanges == (failSync <= 2 ? 0 : 1));
        }
    });
    suite.run("unsupported exchange has no overwriting rename fallback", [] {
        for (const int error : {ENOSYS, EINVAL, EOPNOTSUPP, EXDEV, EACCES}) {
            Fixture fixture;
            const auto interception = std::make_shared<Interception>();
            const auto baseline = fixture.capture(interception);
            interception->exchangeError = error;
            unchanged(fixture, publishDestination(baseline, fixture.edited, "100644", [] { return true; }),
                      error == EACCES ? DestinationFailure::PermissionDenied
                                      : DestinationFailure::UnsupportedFilesystem);
            CHECK(interception->exchanges == 1);
        }
    });
    suite.run("lost exchange replies retain both possible outcomes without rollback", [] {
        for (const int error : {EIO, ESTALE, ETIMEDOUT}) {
            for (const bool committed : {false, true}) {
                Fixture fixture;
                const auto interception = std::make_shared<Interception>();
                const auto baseline = fixture.capture(interception);
                interception->exchangeError = error;
                interception->exchangeBeforeError = committed;
                const auto result = publishDestination(baseline, fixture.edited, "100644", [] { return true; });
                CHECK(result.state == PublicationState::Uncertain && result.failure == DestinationFailure::IoError);
                CHECK(!result.destination && !result.retainedPath.isEmpty() && interception->exchanges == 1);
                CHECK(readFile(fixture.path()) == (committed ? fixture.edited : fixture.original));
                CHECK(readFile(QFile::decodeName(result.retainedPath))
                      == (committed ? fixture.original : fixture.edited));
            }
        }
    });
    suite.run("late displaced edits and replacements require reconciliation without rollback", [] {
        for (const bool replacement : {false, true}) {
            Fixture fixture;
            const auto interception = std::make_shared<Interception>();
            const auto baseline = fixture.capture(interception);
            interception->beforeExchange = [&] {
                if (replacement) CHECK(QFile::rename(fixture.path(), fixture.root + "/saved-original"));
                writeFile(fixture.path(), "external late edit\n");
            };
            const auto result = publishDestination(baseline, fixture.edited, "100644", [] { return true; });
            retained(fixture, result, "external late edit\n");
            CHECK(interception->exchanges == 1);
            CHECK(result.failure
                  == (replacement ? DestinationFailure::PathChanged : DestinationFailure::ContentChanged));
            CHECK(baseline.bytes() == fixture.original);
            if (replacement) CHECK(readFile(fixture.root + "/saved-original") == fixture.original);
        }
    });
    suite.run("external edits after exchange survive without restoring the old result", [] {
        Fixture fixture;
        const auto interception = std::make_shared<Interception>();
        const auto baseline = fixture.capture(interception);
        interception->afterExchange = [&] { writeFile(fixture.path(), "newer external edit\n"); };
        const auto result = publishDestination(baseline, fixture.edited, "100644", [] { return true; });
        CHECK(result.state == PublicationState::Uncertain && result.failure == DestinationFailure::ContentChanged);
        CHECK(!result.destination && interception->exchanges == 1);
        CHECK(readFile(fixture.path()) == "newer external edit\n");
        CHECK(readFile(QFile::decodeName(result.retainedPath)) == fixture.original);
    });
    suite.run("a late symlink is retained without following its target or rolling back", [] {
        Fixture fixture;
        writeFile(fixture.root + "/outside.txt", "external target\n");
        const auto interception = std::make_shared<Interception>();
        const auto baseline = fixture.capture(interception);
        interception->beforeExchange = [&] {
            CHECK(QFile::remove(fixture.path()));
            CHECK(::symlink("outside.txt", fixture.native().constData()) == 0);
        };
        const auto result = publishDestination(baseline, fixture.edited, "100644", [] { return true; });
        CHECK(result.state == PublicationState::Uncertain && result.failure == DestinationFailure::Symlink);
        CHECK(!result.destination && interception->exchanges == 1 && !result.retainedPath.isEmpty());
        struct stat metadata{};
        CHECK(::lstat(result.retainedPath.constData(), &metadata) == 0 && S_ISLNK(metadata.st_mode));
        CHECK(readFile(fixture.path()) == fixture.edited);
        CHECK(readFile(fixture.root + "/outside.txt") == "external target\n");
    });
    suite.run("a late ancestor replacement retains the exchange in its pinned directory", [] {
        Fixture fixture;
        const auto interception = std::make_shared<Interception>();
        const auto baseline = fixture.capture(interception);
        const auto moved = fixture.root + "-relocated";
        interception->beforeExchange = [&] {
            CHECK(QDir().rename(fixture.root, moved));
            CHECK(QDir().mkdir(fixture.root));
            writeFile(fixture.path(), "replacement directory's external file\n");
        };
        const auto result = publishDestination(baseline, fixture.edited, "100644", [] { return true; });
        CHECK(result.state == PublicationState::Uncertain && result.failure == DestinationFailure::PathChanged);
        CHECK(!result.destination && interception->exchanges == 1);
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
            const auto interception = std::make_shared<Interception>();
            const auto baseline = fixture.capture(interception);
            interception->failUnlink = true;
            if (!exchanged) interception->exchangeError = EOPNOTSUPP;
            const auto result = publishDestination(baseline, fixture.edited, "100644", [] { return true; });
            CHECK(result.state == (exchanged ? PublicationState::Published : PublicationState::Unchanged));
            CHECK(result.failure == DestinationFailure::CleanupFailure && !result.destination
                  && !result.retainedPath.isEmpty());
            CHECK(readFile(fixture.path()) == (exchanged ? fixture.edited : fixture.original));
            CHECK(readFile(QFile::decodeName(result.retainedPath)) == (exchanged ? fixture.original : fixture.edited));
            CHECK(interception->exchanges == 1);
        }
    });
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    test_support::configure(argc, argv);
    test_support::Suite suite("DestinationWriterTests");
    addDestinationCases(suite);
    addMetadataAndWriteCases(suite);
    addExchangeAndCleanupCases(suite);
    return suite.report();
}
