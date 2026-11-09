#pragma once

// A FIFO a shell script blocks on until the test lets it through, so scripted stand-ins wait in
// read(2) instead of polling for a file.

#include "TestHarness.h"

#include <QByteArray>
#include <QFile>
#include <QString>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace test_support {

// A gate a scripted Git blocks on with `read -r line < <name>` until the case opens it. The
// case holds the FIFO open for writing, so the script never sees end of file early and opening
// it never blocks. open() writes enough lines that every later read passes too.
class ScriptGate {
public:
    explicit ScriptGate(const QString &path)
    {
        const auto native = QFile::encodeName(path);
        CHECK(::mkfifo(native.constData(), 0600) == 0);
        fd_ = ::open(native.constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        CHECK(fd_ >= 0);
    }
    ~ScriptGate()
    {
        if (fd_ >= 0) ::close(fd_);
    }
    ScriptGate(const ScriptGate &) = delete;
    ScriptGate &operator=(const ScriptGate &) = delete;
    // Lets every waiting and later read through.
    void open() { pass(256); }
    // Lets the next `reads` reads through, whether their scripts wait already or arrive later.
    void pass(int reads = 1)
    {
        const QByteArray lines(reads, '\n');
        EXPECT(::write(fd_, lines.constData(), static_cast<std::size_t>(lines.size())) == lines.size());
    }

private:
    int fd_ = -1;
};

} // namespace test_support
