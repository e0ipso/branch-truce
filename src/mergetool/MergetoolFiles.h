// Mergetool files are the paths Git hands a launcher: read without following links and restored whole.
#pragma once

#include "storage/FileSystem.h"
#include "storage/Problem.h"
#include <QByteArray>
#include <QString>
#include <optional>

namespace branch_truce {

// Repository-relative path Git passed as MERGED. Rejects parent segments.
[[nodiscard]] std::optional<QByteArray> mergetoolRepositoryPath(const QByteArray &merged);

// Reads a regular file of at most the recovery payload limit, following no link.
[[nodiscard]] bool readMergetoolFile(const QByteArray &path, QByteArray *bytes, Problem *error);
// Replaces the file with bytes through a sibling temporary file and a rename,
// keeping its permission bits, so a crash leaves either version whole. Equal
// bytes leave the file alone.
[[nodiscard]] bool restoreMergetoolFile(const QByteArray &path, const QByteArray &bytes, Problem *error,
                                        const storage::FileSystem &files = *storage::systemFileSystem());
}
