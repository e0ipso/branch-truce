// The mergetool intake keeps a waiting launcher's input copies where its window can read and check them.
#pragma once

#include <QByteArray>
#include <QString>
#include <optional>

namespace branch_truce {

// Where launchers copy their inputs: inside the private socket directory, so
// the copies live no longer than the user's runtime directory.
[[nodiscard]] QString mergetoolIntakeRoot(const QString &socketDirectory);

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

// Copies the sources into <root>/<sessionKey>/<invocation>, creating each
// directory with mode 0700. Returns that directory, or an empty string. It
// names this process, by its PID and start time, as the launcher that owns it;
// that launcher removes it when it exits.
[[nodiscard]] QString writeMergetoolIntake(const QString &root, const QByteArray &sessionKey, const QString &invocation,
                                           const MergetoolSources &sources, QString *error);
// Re-reads that directory and checks it still sits under root and the session key.
[[nodiscard]] std::optional<MergetoolIntake> readMergetoolIntake(const QString &directory, const QString &root,
                                                                 const QByteArray &sessionKey, QString *error);
void removeMergetoolIntake(const QString &directory);
// Removes the session's intakes whose launcher process has ended, as after a
// launcher was killed. A process that reuses the launcher's PID does not keep
// an intake, since its start time differs. Blocking; returns how many it removed.
int pruneMergetoolIntakes(const QString &root, const QByteArray &sessionKey);
}
