// A session store with the list model and handoff the application builds over it, for tests.
#pragma once

#include "app/AppPaths.h"
#include "app/SessionHandoff.h"
#include "app/SessionModel.h"
#include "fixtures/Await.h"
#include "session/SessionStore.h"
#include <QVariantList>
#include <QVariantMap>
#include <utility>

namespace test_support {
// Commands run on the store this derives from. Queries that name rows read
// presentation rows through `rows`, the SessionModel the window lists.
class TestSession : public branch_truce::SessionStore {
public:
    explicit TestSession(
        QString directory = branch_truce::newRecoveryDirectory(),
        std::shared_ptr<const branch_truce::storage::FileSystem> files = branch_truce::storage::systemFileSystem())
        : SessionStore(std::move(directory), std::move(files)), rows(*this)
    {}
    bool appendFile(branch_truce::FileSession file)
    {
        const bool accepted = SessionStore::appendFile(std::move(file));
        settle();
        return accepted;
    }
    void replaceFiles(std::vector<branch_truce::FileSession> files, branch_truce::RecoveryRestore *restore = nullptr)
    {
        settle();
        SessionStore::replaceFiles(std::move(files), restore);
        settle();
    }
    bool recordTyping()
    {
        SessionStore::recordTyping();
        settle();
        return !hasPendingRecovery();
    }
    void settle()
    {
        test_support::waitUntil([this] { return !recording(); }, "recovery did not settle");
    }
    branch_truce::SessionModel rows;

    [[nodiscard]] int totalFiles() const { return rows.totalFiles(); }
    [[nodiscard]] int resolvedFiles() const { return rows.resolvedFiles(); }
    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const { return rows.rowCount(parent); }
    [[nodiscard]] QModelIndex index(int row) const { return rows.index(row); }
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const { return rows.data(index, role); }
    [[nodiscard]] QHash<int, QByteArray> roleNames() const { return rows.roleNames(); }
    // The presentation row, as the window lists it.
    [[nodiscard]] int rowOf(const std::string &id) const { return rows.rowOf(id); }
    [[nodiscard]] const branch_truce::FileSession *fileAt(int row) const { return rows.fileAt(row); }
    [[nodiscard]] branch_truce::FilePresentation::Progress progress(int row) const { return rows.progress(row); }
    [[nodiscard]] QString displayPath(const std::string &id) const { return rows.displayPath(id); }
    [[nodiscard]] branch_truce::Handoff handoffFacts() const { return branch_truce::SessionHandoff::build(*this, {}); }
    [[nodiscard]] QVariantMap handoff() const { return handoffFacts().toVariant(); }
    // Whether the files alone would let the session finish.
    [[nodiscard]] bool canFinish() const { return handoffFacts().clean; }
    // {fileId, path, blockers} for each file with a blocker.
    [[nodiscard]] QVariantList finishBlockers() const
    {
        QVariantList result;
        for (const auto &group : handoffFacts().groups)
            for (const auto &entry : group.entries)
                if (!entry.blockers.isEmpty())
                    result.push_back(QVariantMap{{"fileId", QString::fromStdString(entry.fileId)},
                                                 {"path", entry.path},
                                                 {"blockers", entry.blockers}});
        return result;
    }
    bool retryPendingRecovery(const QString &id)
    {
        return await(retryRecovery(id.toStdString())) == branch_truce::Transition::Applied;
    }
};
}
