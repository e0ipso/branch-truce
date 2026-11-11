#pragma once

// A `git` that holds one subcommand on one path until the test releases it,
// then runs the fixture Git. A test constructs the controller with executable(),
// so it can change the repository at an exact point of a background
// inspection. PATH stays as it is, so child processes do not run it.

#include "TestHarness.h"
#include "fixtures/Files.h"
#include "fixtures/GitRepository.h"

#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QSaveFile>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace mergetool_fixture {

class GitGate {
public:
    // Holds `git ... <subcommand> ... -- <path>` while closed. A held call
    // leaves one held.* file naming its arguments and blocks reading its own
    // release.* FIFO until the test writes to it. The call writes its arguments
    // to an arguments.* file and opens the FIFO first, and links held.* to the
    // arguments only then, so a test never sees a held call it cannot read or
    // release. The arguments.* file keeps the suffix reserved until the call
    // is released. Nothing polls: the call sleeps in read(2).
    explicit GitGate(const QByteArray &path)
    {
        CHECK(directory_.isValid());
        using test_support::shellQuote;
        const auto script = QStringLiteral("#!/bin/sh\n"
                                           "gate=%1\n"
                                           "held=$(cat \"$gate/closed\" 2>/dev/null)\n"
                                           "last=\n"
                                           "found=\n"
                                           "for argument do\n"
                                           "  last=$argument\n"
                                           "  [ -n \"$held\" ] && [ \"$argument\" = \"$held\" ] && found=1\n"
                                           "done\n"
                                           "if [ -n \"$found\" ] && [ \"$last\" = %2 ]; then\n"
                                           "  arguments=$(mktemp \"$gate/arguments.XXXXXX\") || exit 125\n"
                                           "  printf '%s\\n' \"$*\" > \"$arguments\" || exit 125\n"
                                           "  suffix=${arguments##*/arguments.}\n"
                                           "  mkfifo \"$gate/release.$suffix\" || exit 125\n"
                                           "  exec 3<>\"$gate/release.$suffix\" || exit 125\n"
                                           "  ln \"$arguments\" \"$gate/held.$suffix\" || exit 125\n"
                                           "  read -r line <&3\n"
                                           "  exec 3<&-\n"
                                           "  rm -f \"$gate/held.$suffix\" \"$gate/release.$suffix\" \"$arguments\"\n"
                                           "fi\n"
                                           "exec %3 \"$@\"\n")
                                .arg(shellQuote(directory_.path()), shellQuote(QString::fromLatin1(path)),
                                     shellQuote(test_support::gitExecutable()));
        // GitAdapter trusts only executables whose file and directories nobody else can write.
        const auto owner = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
        CHECK(QDir().mkpath(directory_.filePath(QStringLiteral("bin")))
              && QFile::setPermissions(directory_.filePath(QStringLiteral("bin")), owner));
        QFile git(directory_.filePath(QStringLiteral("bin/git")));
        const auto bytes = script.toUtf8();
        CHECK(git.open(QIODevice::WriteOnly | QIODevice::Truncate) && git.write(bytes) == bytes.size());
        git.close();
        CHECK(git.setPermissions(owner));
    }
    GitGate(const GitGate &) = delete;
    GitGate &operator=(const GitGate &) = delete;
    ~GitGate()
    {
        open();
        releaseAll();
    }

    // The wrapper to pass as the controller's Git executable.
    QString executable() const { return directory_.filePath(QStringLiteral("bin/git")); }

    // Replaces the closed file in one rename, so a call starting meanwhile reads either the
    // previous subcommand or this one, never an empty file.
    void close(const QString &subcommand)
    {
        QSaveFile closed(directory_.filePath(QStringLiteral("closed")));
        const auto bytes = subcommand.toUtf8();
        CHECK(closed.open(QIODevice::WriteOnly) && closed.write(bytes) == bytes.size() && closed.commit());
    }
    void open() { QFile::remove(directory_.filePath(QStringLiteral("closed"))); }
    // The calls waiting now, sorted by their random names, not by age.
    QStringList held() const
    {
        return QDir(directory_.path()).entryList({QStringLiteral("held.*")}, QDir::Files, QDir::Name);
    }
    QByteArray arguments(const QString &call) const
    {
        QFile file(directory_.filePath(call));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
    // Lets the held call run Git. Its held.* file is gone when this returns.
    void release(const QString &call)
    {
        CHECK(QFile::remove(directory_.filePath(call)));
        wake(call);
    }
    void releaseAll()
    {
        for (const auto &call : held()) {
            QFile::remove(directory_.filePath(call));
            wake(call);
        }
    }

private:
    // Writes a line to the call's FIFO. The call holds it open for reading, so the open cannot
    // block; a call that has already gone leaves no reader and nothing to wake.
    void wake(const QString &call) const
    {
        const auto fifo = QFile::encodeName(directory_.filePath(QStringLiteral("release.") + call.mid(5)));
        const int fd = ::open(fifo.constData(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) return;
        while (::write(fd, "\n", 1) < 0 && errno == EINTR) {}
        ::close(fd);
    }
    QTemporaryDir directory_;
};

} // namespace mergetool_fixture
