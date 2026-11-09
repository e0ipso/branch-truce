#pragma once

// File helpers the Qt test suites share. Each one fails the case when the file system refuses.

#include "TestHarness.h"

#include <QByteArray>
#include <QFile>
#include <QFileDevice>
#include <QString>

#include <fcntl.h>
#include <unistd.h>

namespace test_support {

// The whole file. A missing or unreadable file fails the case.
inline QByteArray readFile(const QString &path)
{
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly));
    const auto bytes = file.readAll();
    CHECK(file.error() == QFileDevice::NoError);
    return bytes;
}

// Replaces the file with `bytes`; `executable` leaves it readable, writable and runnable by its
// owner only, which is what GitAdapter requires of a Git executable.
inline void writeFile(const QString &path, const QByteArray &bytes, bool executable = false)
{
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    CHECK(file.write(bytes) == bytes.size());
    file.close();
    if (executable)
        CHECK(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
}

// Replaces the file named by raw bytes, which need not be UTF-8, so QFile cannot name it.
inline void writeRawFile(const QByteArray &path, const QByteArray &bytes)
{
    const int fd = ::open(path.constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    const auto count = ::write(fd, bytes.constData(), static_cast<std::size_t>(bytes.size()));
    const int closed = ::close(fd);
    CHECK(count == bytes.size() && closed == 0);
}

// `value` as one single-quoted POSIX shell word.
inline QString shellQuote(const QString &value)
{
    return QLatin1Char('\'') + QString(value).replace(QLatin1Char('\''), QStringLiteral("'\\''")) + QLatin1Char('\'');
}

} // namespace test_support
