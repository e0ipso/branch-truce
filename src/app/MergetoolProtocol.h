#pragma once

#include "git/GitAdapter.h"
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <optional>

class QIODevice;

namespace branch_truce {

// Local socket for one worktree operation. Empty when the operation is not a
// supported merge or rebase with both stage commits.
[[nodiscard]] QString mergetoolServerName(const git::RepositoryContext &repository);
[[nodiscard]] QByteArray mergetoolSessionKey(const git::RepositoryContext &repository);
[[nodiscard]] QString mergetoolIntakeRoot();

struct MergetoolIntake {
    QString invocation;
    QByteArray path;
    QByteArray mergedSha256;
    QByteArray baseSha256;
    QByteArray localSha256;
    QByteArray remoteSha256;
    bool basePresent = false;
    bool localPresent = false;
    bool remotePresent = false;
};

struct MergetoolSources {
    QByteArray path;
    QByteArray merged;
    QByteArray base;
    QByteArray local;
    QByteArray remote;
    bool basePresent = false;
    bool localPresent = false;
    bool remotePresent = false;
};

// Copies regular files into an app-owned directory. Does not delete them.
[[nodiscard]] bool writeMergetoolIntake(const QString &directory, const QString &invocation,
                                        const MergetoolSources &sources, QString *error);
// Re-reads that directory and checks it still sits under the session key.
[[nodiscard]] std::optional<MergetoolIntake> readMergetoolIntake(const QString &directory,
                                                                  const QByteArray &sessionKey, QString *error);

enum class MergetoolFrame { NeedMore, Ready, Invalid };
[[nodiscard]] MergetoolFrame takeMergetoolFrame(QByteArray *buffer, QJsonObject *object, QString *error);
[[nodiscard]] bool writeMergetoolFrame(QIODevice *device, const QJsonObject &object, QString *error);

// Repository-relative path Git passed as MERGED. Rejects parent segments.
[[nodiscard]] std::optional<QByteArray> mergetoolRepositoryPath(const QByteArray &merged);

[[nodiscard]] bool readMergetoolFile(const QByteArray &path, QByteArray *bytes, QString *error);
[[nodiscard]] bool restoreMergetoolFile(const QByteArray &path, const QByteArray &bytes, QString *error);
}
