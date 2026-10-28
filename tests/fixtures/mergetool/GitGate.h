#pragma once

// A `git` first on this process's PATH that holds one subcommand on one path
// until the test releases it, then runs /usr/bin/git. The app finds Git through
// PATH, so a test can change the repository at an exact point of a background
// inspection. Child processes started with their own environment keep theirs.

#include "TestHarness.h"

#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <optional>

namespace mergetool_fixture {

class GitGate {
public:
    // Holds `git ... <subcommand> ... -- <path>` while closed. A held call
    // leaves one held.* file naming its arguments and waits until it is gone.
    // The call writes its arguments to an arguments.* file first and links
    // held.* to it only when they are complete, so a test never reads a held
    // call before its arguments exist. The arguments.* file keeps the suffix
    // reserved until the call is released.
    explicit GitGate(const QByteArray &path)
    {
        CHECK(directory_.isValid());
        const auto quoted = [](const QString &value) {
            return QLatin1Char('\'') + QString(value).replace(QLatin1Char('\''), QStringLiteral("'\\''")) + QLatin1Char('\'');
        };
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
            "  call=\"$gate/held.${arguments##*/arguments.}\"\n"
            "  ln \"$arguments\" \"$call\" || exit 125\n"
            "  while [ -e \"$call\" ]; do sleep 0.01; done\n"
            "  rm -f \"$arguments\"\n"
            "fi\n"
            "exec /usr/bin/git \"$@\"\n").arg(quoted(directory_.path()), quoted(QString::fromLatin1(path)));
        // GitAdapter trusts only executables whose file and directories nobody else can write.
        const auto owner = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
        CHECK(QDir().mkpath(directory_.filePath(QStringLiteral("bin")))
              && QFile::setPermissions(directory_.filePath(QStringLiteral("bin")), owner));
        QFile git(directory_.filePath(QStringLiteral("bin/git")));
        const auto bytes = script.toUtf8();
        CHECK(git.open(QIODevice::WriteOnly | QIODevice::Truncate) && git.write(bytes) == bytes.size());
        git.close();
        CHECK(git.setPermissions(owner));
        if (qEnvironmentVariableIsSet("PATH")) previousPath_ = qgetenv("PATH");
        qputenv("PATH", QFile::encodeName(directory_.filePath(QStringLiteral("bin"))) + ':'
            + previousPath_.value_or("/usr/bin:/bin"));
    }
    GitGate(const GitGate &) = delete;
    GitGate &operator=(const GitGate &) = delete;
    ~GitGate()
    {
        open();
        releaseAll();
        if (previousPath_) qputenv("PATH", *previousPath_);
        else qunsetenv("PATH");
    }

    void close(const QString &subcommand)
    {
        QFile closed(directory_.filePath(QStringLiteral("closed")));
        const auto bytes = subcommand.toUtf8();
        CHECK(closed.open(QIODevice::WriteOnly | QIODevice::Truncate) && closed.write(bytes) == bytes.size());
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
    void release(const QString &call) { CHECK(QFile::remove(directory_.filePath(call))); }
    void releaseAll()
    {
        for (const auto &call : held()) QFile::remove(directory_.filePath(call));
    }

private:
    QTemporaryDir directory_;
    std::optional<QByteArray> previousPath_;
};

} // namespace mergetool_fixture
