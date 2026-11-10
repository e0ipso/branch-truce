// Mergetool files are the paths Git hands a launcher: read without following links and restored whole.
#include "mergetool/MergetoolFiles.h"
#include "core/ResourceLimits.h"
#include "storage/FileDescriptors.h"

#include <QFile>

#include <fcntl.h>
#include <unistd.h>

namespace branch_truce {
namespace {
bool failed(Problem *error, ProblemCode problem)
{
    if (error) *error = problem;
    return false;
}

ProblemCode openFailure(storage::OpenFailure failure)
{
    switch (failure) {
    case storage::OpenFailure::EmptyPath: return ProblemCode::MergetoolPathEmpty;
    case storage::OpenFailure::ParentSegment: return ProblemCode::MergetoolPathLeaves;
    case storage::OpenFailure::WrongType: return ProblemCode::MergetoolPathNotRegular;
    case storage::OpenFailure::Unopenable:
    case storage::OpenFailure::None: break;
    }
    return ProblemCode::MergetoolPathUnopenable;
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

bool readMergetoolFile(const QByteArray &path, QByteArray *bytes, Problem *error)
{
    if (!bytes) return failed(error, ProblemCode::MergetoolFileUnreadable);
    auto opened = storage::openWithoutLinks(path, storage::OpenTarget::RegularFile);
    if (!opened.descriptor.valid()) return failed(error, openFailure(opened.failure));
    QFile file;
    if (!file.open(opened.descriptor.get(), QIODevice::ReadOnly, QFileDevice::DontCloseHandle))
        return failed(error, ProblemCode::MergetoolFileUnreadable);
    constexpr qsizetype limit = static_cast<qsizetype>(kMaxRecoveryPayloadBytes);
    *bytes = file.read(limit + 1);
    if (file.error() != QFileDevice::NoError || bytes->size() > limit) {
        bytes->clear();
        if (error) *error = {ProblemCode::MergetoolFileTooLarge, {QString::number(static_cast<qulonglong>(limit))}};
        return false;
    }
    return true;
}

bool restoreMergetoolFile(const QByteArray &path, const QByteArray &bytes, Problem *error,
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
        return failed(error, ProblemCode::MergetoolRestoreFailed);
    return true;
}
}
