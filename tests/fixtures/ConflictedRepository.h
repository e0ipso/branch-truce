#pragma once

// A disposable repository for the application suites: fixture Git in the shared sandbox, the
// commits a conflict needs, and the environment a window opened on it runs with.
//
//   RuntimeDirectory      a private directory short enough for a session socket path.
//   ConflictedRepository  a GitRepository on branch main whose `environment` also points the XDG
//                         data, cache and runtime directories at private ones, with helpers to
//                         commit files and stop a merge, rebase or cherry-pick on conflicts.

#include "TestHarness.h"
#include "fixtures/Files.h"
#include "fixtures/GitRepository.h"

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDevice>
#include <QMap>
#include <QProcessEnvironment>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <utility>

namespace test_support {

// An owner-only directory under /tmp. A QTemporaryDir under a long TMPDIR would push a session
// socket path past its length limit, and the session would fall back to the shared
// /tmp/branch-truce-<uid> directory instead of this private one.
class RuntimeDirectory {
public:
    RuntimeDirectory()
    {
        CHECK(directory_.isValid());
        CHECK(QFile::setPermissions(directory_.path(),
                                    QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    }
    QString path() const { return directory_.path(); }

private:
    QTemporaryDir directory_{QStringLiteral("/tmp/bt-run-XXXXXX")};
};

class ConflictedRepository {
public:
    // How Git stops on the conflicts between main and side.
    enum class Stop { Merge, RebaseMerge, RebaseApply, CherryPick };

    ConflictedRepository() : root(repository_.root()), environment(repository_.environment())
    {
        // GitRepository's sandbox sets PATH, LC_ALL, HOME, XDG_CONFIG_HOME and the Git configuration.
        environment.insert(QStringLiteral("XDG_CACHE_HOME"), repository_.home().path() + QStringLiteral("/cache"));
        environment.insert(QStringLiteral("XDG_DATA_HOME"), repository_.home().path() + QStringLiteral("/data"));
        // The window's mergetool socket directory is created under here.
        environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime.path());
        git({QStringLiteral("config"), QStringLiteral("core.autocrlf"), QStringLiteral("false")});
    }
    ConflictedRepository(const ConflictedRepository &) = delete;
    ConflictedRepository &operator=(const ConflictedRepository &) = delete;

    QString path(const QString &relative) const { return root + QLatin1Char('/') + relative; }
    // Runs fixture Git in the worktree with `environment`. Another exit status fails the case.
    QByteArray git(const QStringList &arguments, int expectedExit = 0, const QByteArray &input = {}) const
    {
        return runGit(environment, root, arguments, input, expectedExit);
    }
    // Writes each file and commits them all with `message`.
    void commit(const QMap<QString, QByteArray> &files, const QString &message) const
    {
        for (auto file = files.cbegin(); file != files.cend(); ++file) writeFile(path(file.key()), file.value());
        git(QStringList{QStringLiteral("add"), QStringLiteral("--")} + files.keys());
        git({QStringLiteral("commit"), QStringLiteral("--quiet"), QStringLiteral("-m"), message});
    }
    // Commits `side` on a new branch side and `main` on main, from the current commit, then stops
    // `stop` on them. Returns the side and main commit IDs.
    std::pair<QByteArray, QByteArray> conflict(const QMap<QString, QByteArray> &main,
                                               const QMap<QString, QByteArray> &side, Stop stop = Stop::Merge) const
    {
        git({QStringLiteral("checkout"), QStringLiteral("--quiet"), QStringLiteral("-b"), QStringLiteral("side")});
        commit(side, QStringLiteral("side"));
        const auto sideCommit = git({QStringLiteral("rev-parse"), QStringLiteral("HEAD")}).trimmed();
        git({QStringLiteral("checkout"), QStringLiteral("--quiet"), QStringLiteral("main")});
        commit(main, QStringLiteral("main"));
        const auto mainCommit = git({QStringLiteral("rev-parse"), QStringLiteral("HEAD")}).trimmed();
        switch (stop) {
        case Stop::Merge: git({QStringLiteral("merge"), QStringLiteral("--no-edit"), QStringLiteral("side")}, 1); break;
        case Stop::RebaseMerge:
        case Stop::RebaseApply:
            git({QStringLiteral("checkout"), QStringLiteral("--quiet"), QStringLiteral("side")});
            git({QStringLiteral("rebase"),
                 stop == Stop::RebaseApply ? QStringLiteral("--apply") : QStringLiteral("--merge"),
                 QStringLiteral("main")},
                1);
            break;
        case Stop::CherryPick: git({QStringLiteral("cherry-pick"), QStringLiteral("side")}, 1); break;
        }
        return {sideCommit, mainCommit};
    }
    // Every file under the worktree, .git included, by relative path: the index, operation
    // metadata, refs, objects and untracked files.
    QMap<QString, QByteArray> snapshot() const
    {
        QMap<QString, QByteArray> result;
        QDirIterator files(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const auto selected = files.next();
            result.insert(QDir(root).relativeFilePath(selected), readFile(selected));
        }
        return result;
    }
    // The paths that differ between two snapshots, a missing one prefixed with "missing ".
    static QStringList changed(const QMap<QString, QByteArray> &before, const QMap<QString, QByteArray> &after)
    {
        QStringList paths;
        for (auto it = after.cbegin(); it != after.cend(); ++it)
            if (!before.contains(it.key()) || before.value(it.key()) != it.value()) paths << it.key();
        for (auto it = before.cbegin(); it != before.cend(); ++it)
            if (!after.contains(it.key())) paths << QStringLiteral("missing ") + it.key();
        return paths;
    }
    // The paths Git lists as unmerged.
    QSet<QByteArray> unmerged() const
    {
        QSet<QByteArray> result;
        for (const auto &path : git({QStringLiteral("diff"), QStringLiteral("--name-only"),
                                     QStringLiteral("--diff-filter=U"), QStringLiteral("-z")})
                                    .split('\0'))
            if (!path.isEmpty()) result.insert(path);
        return result;
    }
    const GitHome &home() const { return repository_.home(); }

private:
    GitRepository repository_;

public:
    RuntimeDirectory runtime;
    // The worktree, with symbolic links resolved.
    const QString root;
    // The environment for fixture Git and for a window opened on this repository. Add to it for a
    // case that needs more.
    QProcessEnvironment environment;
};

} // namespace test_support
