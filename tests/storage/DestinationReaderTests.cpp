// Destination reads admit expected metadata and finite original bytes before allocation.
#include "TestHarness.h"
#include "fixtures/Files.h"
#include "storage/DestinationWriter.h"
#include "storage/TextFormat.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

using namespace branch_truce;
using namespace branch_truce::storage;
namespace {
class Reads final : public FileSystem {
public:
    enum class Fault { None, Open, Entry, Status, Read, Finish, Grow, Metadata, Interrupt };
    mutable Fault fault = Fault::None;
    mutable int reads = 0;
    QByteArray path;
    int openAt(int directory, const char *name, int flags, mode_t mode) const override
    {
        if (fault == Fault::Open && std::string_view(name) == "result") return fail();
        return FileSystem::openAt(directory, name, flags, mode);
    }
    int statusAt(int directory, const char *name, struct stat *value, int flags) const override
    {
        if (fault == Fault::Entry && std::string_view(name) == "result") return fail();
        return FileSystem::statusAt(directory, name, value, flags);
    }
    int status(int fd, struct stat *value) const override
    {
        const auto result = FileSystem::status(fd, value);
        if (result || !S_ISREG(value->st_mode)) return result;
        if (fault == Fault::Status) return fail();
        if (fault == Fault::Metadata) value->st_mode ^= S_IXUSR;
        if (fault == Fault::Grow) {
            fault = Fault::None;
            if (::truncate(path.constData(), static_cast<off_t>(kMaxDestinationBytes) + 1) != 0) return -1;
        }
        return result;
    }
    ssize_t readAt(int fd, void *bytes, std::size_t size, off_t offset) const override
    {
        ++reads;
        if (fault == Fault::Read || (fault == Fault::Finish && offset == 4)) return fail();
        if (fault == Fault::Interrupt) {
            fault = Fault::None;
            errno = EINTR;
            return -1;
        }
        return FileSystem::readAt(fd, bytes, std::min(size, std::size_t{2}), offset);
    }

private:
    static int fail()
    {
        errno = EIO;
        return -1;
    }
};
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    test_support::configure(argc, argv);
    test_support::Suite suite("DestinationReaderTests");
    suite.run("sparse growth is refused before capture or publication reads and mutation admission", [] {
        QTemporaryDir directory;
        const auto root = QFileInfo(directory.path()).canonicalFilePath();
        const auto path = QFile::encodeName(root + "/result");
        test_support::writeFile(root + "/result", "old\n");
        const auto files = std::make_shared<Reads>();
        const auto captured = captureDestination(root, "result", files);
        CHECK(captured.destination);
        const auto identity = captured.destination->identity();
        CHECK(::truncate(path.constData(), off_t{1} << 40) == 0);
        files->reads = 0;
        CHECK(captureDestination(root, "result", files).failure == DestinationFailure::ReadLimit);
        CHECK(captureDestination(root, "result", files, &identity).failure == DestinationFailure::ContentChanged);
        int admissions = 0;
        const auto published = publishDestination(*captured.destination, "new\n", "100644", [&] {
            ++admissions;
            return true;
        });
        CHECK(published.failure == DestinationFailure::ContentChanged && admissions == 0 && files->reads == 0);
    });
    for (const auto fault :
         {Reads::Fault::Open, Reads::Fault::Entry, Reads::Fault::Status, Reads::Fault::Read, Reads::Fault::Finish,
          Reads::Fault::Grow, Reads::Fault::Metadata, Reads::Fault::Interrupt}) {
        suite.run("syscall errors and growth retain stable read ordering", [fault] {
            QTemporaryDir directory;
            const auto root = QFileInfo(directory.path()).canonicalFilePath();
            test_support::writeFile(root + "/result", "old\n");
            const auto files = std::make_shared<Reads>();
            files->path = QFile::encodeName(root + "/result");
            files->fault = fault;
            const auto captured = captureDestination(root, "result", files);
            CHECK(captured.destination.has_value() == (fault == Reads::Fault::Interrupt));
            if (fault == Reads::Fault::Metadata || fault == Reads::Fault::Status || fault == Reads::Fault::Open
                || fault == Reads::Fault::Entry)
                CHECK(files->reads == 0);
            if (fault == Reads::Fault::Grow) CHECK(captured.failure == DestinationFailure::ContentChanged);
            if (fault == Reads::Fault::Interrupt) CHECK(captured.destination->bytes() == "old\n");
        });
    }
    suite.run("the raw ceiling preserves expanded UTF16 CRLF original bytes", [] {
        QTemporaryDir directory;
        const auto root = QFileInfo(directory.path()).canonicalFilePath();
        const auto utf8 = (QByteArray(131071, 'x') + '\n').repeated(33);
        const auto encoded = serializeText(utf8, {TextEncoding::Utf16LittleEndian, true, Newline::CrLf, true});
        CHECK(encoded.document && encoded.document->originalBytes.size() > qsizetype(kMaxRecoveryPayloadBytes));
        test_support::writeFile(root + "/result", encoded.document->originalBytes);
        const auto captured = captureDestination(root, "result");
        CHECK(captured.destination && captured.destination->bytes() == encoded.document->originalBytes);
        const auto published =
            publishDestination(*captured.destination, encoded.document->originalBytes, "100644", [] { return true; });
        CHECK(published.destination && published.destination->bytes() == encoded.document->originalBytes);
    });
    return suite.report();
}
