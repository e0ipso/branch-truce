// Background alignment aligns typed edits on a worker and installs the result while its revision is current.
#pragma once

#include "core/RevisionId.h"
#include "core/SessionState.h"
#include <QFuture>
#include <QObject>
#include <QThreadPool>
#include <memory>
#include <set>
#include <string>

namespace branch_truce {
class CorrespondenceMap;
class SessionStore;

// Aligns the provisional passages a typed edit leaves (SessionStore::typeResult)
// on a worker thread and installs the map through the store, which keeps it
// only while the revision it was made for is current: a stale map is dropped.
// One alignment per file runs at a time; when it ends, the file's current
// revision is aligned next if it is still provisional, so typing costs one
// alignment at a time rather than one per key. The worker reads only the
// immutable map it was given. Used on the store's thread.
class BackgroundAlignment final : public QObject {
    Q_OBJECT
public:
    explicit BackgroundAlignment(SessionStore &store, QObject *parent = nullptr);
    // Waits for the alignments still running.
    ~BackgroundAlignment() override = default;
    QFuture<Transition> waitFor(const std::string &id, RevisionId revision);
Q_SIGNALS:
    void finished(const QString &id, quint64 revision, bool succeeded);

private:
    // Starts aligning the file in storage row `row` unless its passages are
    // exact or it is being aligned already.
    void request(int row);
    // A worker's map for `revision` of the file, or null when it failed.
    void aligned(const std::string &id, RevisionId revision, quint64 generation,
                 const std::shared_ptr<const CorrespondenceMap> &map);

    SessionStore &store_;
    // The files being aligned, for the session `generation_` counts.
    std::set<std::string> running_;
    quint64 generation_ = 0;
    // Goes first, finishing its alignments while their results can still be dropped.
    QThreadPool workers_;
};
}
