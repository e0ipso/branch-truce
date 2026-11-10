// Background alignment aligns typed edits on a worker and installs the result while its revision is current.
#include "session/BackgroundAlignment.h"
#include "session/SessionStore.h"
#include "storage/OffThread.h"
#include <exception>

namespace branch_truce {
BackgroundAlignment::BackgroundAlignment(SessionStore &store, QObject *parent) : QObject(parent), store_(store)
{
    connect(&store_, &SessionStore::fileChanged, this, &BackgroundAlignment::request);
    // A replaced session's maps belong to files that are gone.
    connect(&store_, &SessionStore::aboutToReplace, this, [this] {
        running_.clear();
        ++generation_;
    });
}

void BackgroundAlignment::request(int row)
{
    const auto &file = store_.files()[static_cast<std::size_t>(row)];
    if (running_.contains(file.id())) return;
    // Immutable and shared, so the worker reads it while the file goes on changing.
    auto pending = file.pendingAlignment();
    if (!pending) return;
    const auto id = file.id();
    const auto revision = file.current().id;
    const auto generation = generation_;
    running_.insert(id);
    runOffThread(
        workers_, this,
        [pending]() -> std::shared_ptr<const CorrespondenceMap> {
            try {
                return std::make_shared<const CorrespondenceMap>(pending->realigned());
            } catch (const std::exception &) {
                // Out of memory: the passages stay provisional, as the engine allows.
                return nullptr;
            }
        },
        [this, id, revision, generation](const auto &map) { aligned(id, revision, generation, map); });
}

void BackgroundAlignment::aligned(const std::string &id, RevisionId revision, quint64 generation,
                                  const std::shared_ptr<const CorrespondenceMap> &map)
{
    if (generation != generation_) return;
    running_.erase(id);
    const auto row = store_.rowOf(id);
    if (row < 0) return;
    // StaleRevision once typing moved on; the map is dropped and the current revision is aligned next.
    if (map) (void)store_.installAlignment(id, revision, map);
    // A failed alignment is not repeated for the same revision.
    if (!map && store_.files()[static_cast<std::size_t>(row)].current().id == revision) return;
    request(row);
}
}
