#pragma once

#include "GitAdapter.h"
#include <QHash>
#include <QMap>

namespace branch_truce::git::detail {
// A bounded, read-only protocol driven by GitAdapter's supervised QProcess.
// No subprocesses, environment overrides or arbitrary commands enter here.
class RepositoryDiscovery {
public:
    RepositoryDiscovery(Result &result, const QString &root);
    [[nodiscard]] QList<QByteArray> arguments() const;
    // false means the structured result is complete, including a rejection.
    bool consume(const QByteArray &output, int exitCode);
private:
    enum class Step { GitDirectory, Index, Head, Commit, MergeBase, ReplayAncestor,
                      OntoAncestor, Tree, SourceTree, FinalIndex, FinalHead, Names, Done };
    struct Commit { QList<QByteArray> parents; };
    struct TreeEntry { QByteArray mode; QByteArray objectId; };
    bool captureMetadata(QMap<QString, QByteArray> &snapshot);
    bool identifyOperation();
    bool readId(const QString &name, QByteArray &value, bool required = true);
    bool readCommit(const QByteArray &bytes);
    bool readTree(const QByteArray &bytes);
    void beginTrees();
    void verifyStages();
    bool finish();
    bool fail(const QString &reason);
    void unsupported(const QString &reason);
    Result &result;
    Step step = Step::GitDirectory;
    QMap<QString, QByteArray> metadata;
    QByteArray index;
    QByteArray head;
    QByteArray other;
    QByteArray originalHead;
    QByteArray onto;
    QByteArray headName;
    QByteArray base;
    QList<QByteArray> commitQueue;
    QHash<QByteArray, Commit> commits;
    QList<QByteArray> paths;
    QList<QByteArray> treeQueue;
    QHash<QByteArray, QHash<QByteArray, TreeEntry>> trees;
    QHash<QByteArray, QHash<QByteArray, bool>> sourceObjects;
    qsizetype pathOffset = 0;
    static constexpr qsizetype treeBatchSize = 64;
};
} // namespace branch_truce::git::detail
