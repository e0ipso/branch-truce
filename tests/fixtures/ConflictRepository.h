#pragma once

// A disposable repository stopped on conflicts by a real `git merge` or `git rebase --merge`,
// built with fixture Git in a private home (see GitRepository.h). Each path is described by its
// base, left and right bytes; a missing side deletes the path on that branch.
//
//   left   the branch checked out when the operation stops: main for a merge, the rebased
//          state for a rebase.
//   right  the branch being merged, or the commit being replayed: side.

#include "TestHarness.h"
#include "fixtures/Files.h"
#include "fixtures/GitRepository.h"

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QMap>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace test_support {

enum class GitOperation { Merge, Rebase };

// One path of the operation.
struct OperationPath {
    QString path;
    std::optional<QByteArray> base;
    std::optional<QByteArray> left;
    std::optional<QByteArray> right;
    // Written over Git's conflict output after the operation stops, as a user would before launch.
    std::optional<QByteArray> manual;
};

class ConflictRepository {
public:
    ConflictRepository(GitOperation operation, const std::vector<OperationPath> &paths)
    {
        git({"config", "user.name", "Branch Truce Acceptance"});
        git({"config", "user.email", "acceptance@example.invalid"});
        git({"config", "core.autocrlf", "false"});
        // A remote that cannot be reached: opening and resolving must not contact it.
        git({"config", "remote.origin.url", "https://offline.invalid/never-contact"});
        git({"config", "branch.main.remote", "origin"});
        writeFile(path("README"), "acceptance fixture\n");
        QStringList tracked{"README"};
        for (const auto &spec : paths) {
            if (!spec.base) continue;
            CHECK(QDir().mkpath(QFileInfo(path(spec.path)).path()));
            writeFile(path(spec.path), *spec.base);
            tracked << spec.path;
        }
        git(QStringList{"add", "--"} + tracked);
        git({"commit", "--quiet", "-m", "base"});
        rightCommit_ = commitSide("side", paths, false);
        git({"checkout", "--quiet", "main"});
        leftCommit_ = commitSide("main", paths, true);
        if (operation == GitOperation::Rebase) {
            git({"checkout", "--quiet", "side"});
            git({"rebase", "--merge", "main"}, 1);
        } else {
            git({"merge", "--no-edit", "side"}, 1);
        }
        for (const auto &spec : paths)
            if (spec.manual) writeFile(path(spec.path), *spec.manual);
    }
    ConflictRepository(const ConflictRepository &) = delete;
    ConflictRepository &operator=(const ConflictRepository &) = delete;

    QString root() const { return repository_.root(); }
    QString path(const QString &relative) const { return repository_.path(relative); }
    // The environment fixture Git runs in, which also selects the configuration the app's Git reads.
    const QProcessEnvironment &environment() const { return repository_.environment(); }
    // The commits on each side, full object ids.
    const QByteArray &leftCommit() const { return leftCommit_; }
    const QByteArray &rightCommit() const { return rightCommit_; }

    // Runs fixture Git in the worktree; another exit status than `expected` fails the case.
    QByteArray git(const QStringList &arguments, int expected = 0) const
    {
        return repository_.git(arguments, {}, expected);
    }
    QByteArray stageObject(int stage, const QString &relative) const
    {
        return git({"rev-parse", QStringLiteral(":%1:%2").arg(stage).arg(relative)}).trimmed();
    }
    QByteArray stageText(int stage, const QString &relative) const
    {
        return git({"cat-file", "blob", QString::fromLatin1(stageObject(stage, relative))});
    }
    // Unmerged index entries for one path; empty once Git records a resolution.
    QByteArray unmerged(const QString &relative) const { return git({"ls-files", "-u", "--", relative}); }
    // Every file under the worktree, Git's own included: the index, operation metadata, refs,
    // objects and untracked files.
    QMap<QString, QByteArray> snapshot() const
    {
        QMap<QString, QByteArray> result;
        QDirIterator files(root(), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const auto selected = files.next();
            result.insert(QDir(root()).relativeFilePath(selected), readFile(selected));
        }
        return result;
    }

private:
    QByteArray commitSide(const char *branch, const std::vector<OperationPath> &paths, bool left)
    {
        if (!left) git({"checkout", "--quiet", "-b", branch});
        QStringList changed;
        QStringList removed;
        for (const auto &spec : paths) {
            const auto &content = left ? spec.left : spec.right;
            if (content) {
                CHECK(QDir().mkpath(QFileInfo(path(spec.path)).path()));
                writeFile(path(spec.path), *content);
                changed << spec.path;
            } else if (spec.base) {
                removed << spec.path;
            }
        }
        if (!changed.isEmpty()) git(QStringList{"add", "--"} + changed);
        if (!removed.isEmpty()) git(QStringList{"rm", "--quiet", "--"} + removed);
        git({"commit", "--quiet", "-m", branch});
        return git({"rev-parse", "HEAD"}).trimmed();
    }

    GitRepository repository_;
    QByteArray leftCommit_;
    QByteArray rightCommit_;
};

} // namespace test_support
