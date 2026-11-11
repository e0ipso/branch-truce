// Repository discovery verifies merge or rebase sources and their paths before the app presents them.
#pragma once

#include "GitAdapter.h"
#include <QHash>
#include <QMap>

namespace branch_truce::git::detail {
// A bounded, read-only protocol driven by GitAdapter's supervised QProcess.
// No subprocesses, environment overrides or arbitrary commands enter here.
class RepositoryDiscovery {
public:
    RepositoryDiscovery(Result &target, const QString &root);
    [[nodiscard]] QList<QByteArray> arguments() const;
    // false means the structured result is complete, including a rejection.
    bool consume(const QByteArray &output, int code);

private:
    enum class Step {
        GitDirectory,
        Index,
        Head,
        Commit,
        MergeBase,
        ReplayAncestor,
        OntoAncestor,
        Tree,
        SourceTree,
        FinalIndex,
        FinalHead,
        Names,
        Done
    };
    struct Commit {
        QList<QByteArray> parents;
    };
    struct TreeEntry {
        QByteArray mode;
        QByteArray objectId;
    };
    bool captureMetadata(QMap<QString, QByteArray> &snapshot);
    bool readMetadata(const QString &name, bool directory, QMap<QString, QByteArray> &snapshot, qsizetype &total);
    bool identifyOperation();
    bool identifyRebase(bool rebaseMerge);
    bool readId(const QString &name, QByteArray &value, bool required = true);
    bool readCommit(const QByteArray &bytes);
    bool readTree(const QByteArray &bytes);
    void queueSourceTrees();
    bool readDirectory(const QByteArray &output);
    bool readIndex(const QByteArray &output);
    bool readHead(const QByteArray &output);
    bool consumeCommit(const QByteArray &output);
    bool readMergeBase(const QByteArray &output);
    bool readNames(const QByteArray &output);
    void beginTrees();
    void verifyStages();
    bool finish();
    bool fail(Problem reason);
    void unsupported(Problem reason);
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
