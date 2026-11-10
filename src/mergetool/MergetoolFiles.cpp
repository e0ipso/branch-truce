// Mergetool files are the paths Git hands a launcher: read without following links and restored whole.
#include "mergetool/MergetoolFiles.h"
#include "core/ResourceLimits.h"
#include "storage/FileDescriptors.h"

#include <QFile>

#include <fcntl.h>
#include <unistd.h>

namespace branch_truce {
namespace {
bool failed(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

QString openFailure(storage::OpenFailure failure)
{
    switch (failure) {
    case storage::OpenFailure::EmptyPath: return QStringLiteral("The mergetool path is empty.");
    case storage::OpenFailure::ParentSegment: return QStringLiteral("The mergetool path leaves its directory.");
    case storage::OpenFailure::WrongType: return QStringLiteral("The mergetool path is not a regular file.");
    case storage::OpenFailure::Unopenable:
    case storage::OpenFailure::None: break;
    }
    return QStringLiteral("The mergetool path could not be opened.");
}
}

std::optional<QByteArray> mergetoolRepositoryPath(const QByteArray &merged)
{
    auto path = merged;
    if (path.startsWith("./")) path = path.mid(2);
    if (path.isEmpty() || path.startsWith('/') || path.contains('\0') || path.contains('\\')) return std::nullopt;
    for (const auto &part : path.split('/')) {
        if (part.isEmpty() || part == "." || part == "..") return std::nullopt;
    }
    return path;
}

bool readMergetoolFile(const QByteArray &path, QByteArray *bytes, QString *error)
{
    if (!bytes) return failed(error, QStringLiteral("The mergetool file could not be read."));
    auto opened = storage::openWithoutLinks(path, storage::OpenTarget::RegularFile);
    if (!opened.descriptor.valid()) return failed(error, openFailure(opened.failure));
    QFile file;
    if (!file.open(opened.descriptor.get(), QIODevice::ReadOnly, QFileDevice::DontCloseHandle))
        return failed(error, QStringLiteral("The mergetool file could not be read."));
    constexpr qsizetype limit = static_cast<qsizetype>(kMaxRecoveryPayloadBytes);
    *bytes = file.read(limit + 1);
    if (file.error() != QFileDevice::NoError || bytes->size() > limit) {
        bytes->clear();
        return failed(error,
                      QStringLiteral("The mergetool file exceeds the %1 byte recovery payload limit.")
                          .arg(static_cast<qulonglong>(limit)));
    }
    return true;
}

bool restoreMergetoolFile(const QByteArray &path, const QByteArray &bytes, QString *error,
                          const storage::FileSystem &files)
{
    QByteArray current;
    if (!readMergetoolFile(path, &current, error)) return false;
    if (current == bytes) return true;
    const auto slash = path.lastIndexOf('/');
    const auto name = path.mid(slash + 1);
    const auto directory = slash < 0
        ? storage::OpenedPath{storage::Descriptor(::open(".", O_RDONLY | O_CLOEXEC | O_DIRECTORY)),
                              storage::OpenFailure::None}
        : storage::openWithoutLinks(slash == 0 ? QByteArray("/") : path.first(slash), storage::OpenTarget::Directory);
    if (!directory.descriptor.valid() || !storage::replaceFile(files, directory.descriptor.get(), name, bytes))
        return failed(error, QStringLiteral("The original file could not be restored."));
    return true;
}
}
