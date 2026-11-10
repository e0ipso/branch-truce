// Background alignment aligns typed edits on a worker and installs the result while its revision is current.
#include "session/BackgroundAlignment.h"
#include "session/SessionStore.h"
#include "storage/OffThread.h"
#include <QPromise>
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

QFuture<Transition> BackgroundAlignment::waitFor(const std::string &id, RevisionId revision)
{
    const auto *file = store_.file(id);
    if (!file || file->current().id != revision) return completedFuture(Transition::StaleRevision);
    if (!file->pendingAlignment()) return completedFuture(Transition::NoChange);
    auto promise = std::make_shared<QPromise<Transition>>();
    promise->start();
    auto future = promise->future();
    auto *waiter = new QObject(this);
    const auto finish = [waiter, promise](Transition result) {
        promise->addResult(result);
        promise->finish();
        delete waiter;
    };
    connect(&store_, &SessionStore::aboutToReplace, waiter, [finish] { finish(Transition::StaleRevision); });
    connect(&store_, &SessionStore::fileChanged, waiter, [this, id, revision, finish](int) {
        const auto *current = store_.file(id);
        if (!current || current->current().id != revision) finish(Transition::StaleRevision);
    });
    connect(this, &BackgroundAlignment::finished, waiter,
            [id, revision, finish](const QString &file, quint64 aligned, bool succeeded) {
                if (file.toStdString() == id && aligned == revision)
                    finish(succeeded ? Transition::Applied : Transition::OutOfMemory);
            });
    request(store_.rowOf(id));
    return future;
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
            } catch (...) {
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
    const auto result = map ? store_.installAlignment(id, revision, map) : Transition::OutOfMemory;
    const bool installed = result == Transition::Applied || result == Transition::NoChange;
    Q_EMIT finished(QString::fromStdString(id), revision, installed);
    // A failed alignment is not repeated for the same revision.
    if (!installed && store_.files()[static_cast<std::size_t>(row)].current().id == revision) return;
    request(row);
}
}
