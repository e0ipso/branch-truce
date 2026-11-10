// Git types name a repository's operation and the configuration files its Git reads, without running Git.
#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <utility>

class QProcessEnvironment;

namespace branch_truce::git {

enum class RepositoryOperation { None, Merge, Rebase, Unsupported };
struct SourceIdentity {
    QByteArray commitId;
    QString label;
    QString name = {}; // A unique local branch observed pointing exactly at commitId.
};
struct RepositoryContext {
    QString worktreeRoot;
    QString gitDirectory; // Canonical per-worktree directory, including linked worktrees.
    RepositoryOperation operation = RepositoryOperation::None;
    // Operation identity only. Check unsupportedPaths, file types and conversion
    // policy before editing or publishing. Reinspect before any mutation.
    bool supported = false;
    QString diagnostic;
    // Exact conflict paths whose stages cannot be attributed to the operation.
    // Keep their index entries visible without rejecting other paths.
    QHash<QByteArray, QString> unsupportedPaths;
    SourceIdentity stage2;
    SourceIdentity stage3;
    QByteArray baseCommitId; // Empty for an unavailable common earlier version.
    // Validated rebase context; these names do not label either index side.
    QByteArray headName;
    QByteArray originalHead;
    QByteArray onto;
    QString ontoName = {};
};

// The variables that choose which configuration files Git reads. The Git the
// adapter starts reads the same files as the user's own Git and a `git
// mergetool` parent started from the same environment. Every other variable of
// the Git child is fixed by the adapter.
struct GitEnvironment {
    QString home; // HOME
    QString configHome; // XDG_CONFIG_HOME; empty lets Git use $HOME/.config
    QString globalConfig; // GIT_CONFIG_GLOBAL; empty when unset
    QString systemConfig; // GIT_CONFIG_SYSTEM; empty when unset
    QString noSystem; // GIT_CONFIG_NOSYSTEM as given; empty when unset
    // Configuration given on a Git command line (git -c), which Git passes to
    // the tools it starts. GIT_CONFIG_PARAMETERS as given; empty when unset.
    QString parameters;
    // GIT_CONFIG_KEY_<n> and GIT_CONFIG_VALUE_<n> for each n below
    // GIT_CONFIG_COUNT, in order. Empty when the count is unset or invalid.
    QList<std::pair<QString, QString>> entries;
    bool operator==(const GitEnvironment &) const = default;
    // Takes each path only when it is absolute. HOME falls back to the
    // account's home directory, as before a desktop session sets it.
    [[nodiscard]] static GitEnvironment from(const QProcessEnvironment &environment);
    [[nodiscard]] static GitEnvironment fromProcess();
};

} // namespace branch_truce::git
