// Background alignment retains finite reservations across session generations until workers terminate.
#include "BackgroundAlignment.h"
#include "SessionStore.h"
#include "core/internal/ComputationInput.h"
#include "storage/OffThread.h"

namespace branch_truce {
namespace {
void finish(const std::shared_ptr<QPromise<Transition>> &promise, Transition result)
{
    promise->addResult(result);
    promise->finish();
}
}
BackgroundAlignment::BackgroundAlignment(SessionStore &store, QObject *parent, detail::WorkAdmission limits)
    : QObject(parent), store_(store), admission_(limits)
{
    workers_.setMaxThreadCount(static_cast<int>(admission_.maxCount));
    connect(&store_, &SessionStore::fileChanged, this, [this](int row) {
        settle();
        request(row);
    });
    connect(&store_, &SessionStore::replaced, this, &BackgroundAlignment::resume);
    connect(&store_, &SessionStore::aboutToReplace, this, [this] {
        ++generation_;
        failed_.clear();
        auto waiters = std::move(waiters_);
        waiters_.clear();
        for (const auto &waiter : waiters) finish(waiter.promise, Transition::StaleRevision);
    });
}
BackgroundAlignment::~BackgroundAlignment()
{
    for (const auto &waiter : waiters_) finish(waiter.promise, Transition::Blocked);
}

void BackgroundAlignment::settle()
{
    for (std::size_t i = 0; i < waiters_.size();) {
        const auto &waiter = waiters_[i];
        const auto *file = store_.file(waiter.id);
        const auto failed = failed_.find(waiter.id);
        const auto result = !file || file->current().id != waiter.revision       ? Transition::StaleRevision
            : !file->pendingAlignment()                                          ? Transition::Applied
            : failed != failed_.end() && failed->second.first == waiter.revision ? failed->second.second
                                                                                 : Transition::ConfirmationRequired;
        if (result == Transition::ConfirmationRequired) {
            ++i;
            continue;
        }
        const auto promise = waiter.promise;
        waiters_.erase(waiters_.begin() + static_cast<std::ptrdiff_t>(i));
        finish(promise, result);
    }
}

QFuture<Transition> BackgroundAlignment::waitFor(const std::string &id, RevisionId revision)
{
    const auto *file = store_.file(id);
    if (!file || file->current().id != revision) return completedFuture(Transition::StaleRevision);
    if (!file->pendingAlignment()) return completedFuture(Transition::NoChange);
    if (const auto failed = failed_.find(id); failed != failed_.end() && failed->second.first == revision)
        return completedFuture(failed->second.second);
    if (waiters_.size() >= 128) return completedFuture(Transition::ResourceLimit);
    auto promise = std::make_shared<QPromise<Transition>>();
    promise->start();
    waiters_.push_back({id, revision, promise});
    request(store_.rowOf(id));
    return promise->future();
}

void BackgroundAlignment::request(int row)
{
    if (row < 0 || static_cast<std::size_t>(row) >= store_.size()) return;
    const auto &file = store_.files()[static_cast<std::size_t>(row)];
    const auto key = std::pair{generation_, file.id()};
    const auto revision = file.current().id;
    if (running_.contains(key)) return;
    if (const auto failed = failed_.find(file.id()); failed != failed_.end() && failed->second.first == revision)
        return;
    const auto *map = file.correspondence();
    if (!map || !file.pendingAlignment()) return;
    const auto bytes = 2 * map->retainedBytes() + 4 * kMaxMetadataBytes
        + (file.snapshots() ? detail::captureBytes(*file.snapshots()) : 0);
    if (bytes > admission_.maxBytes) {
        failed_[file.id()] = {revision, Transition::ResourceLimit};
        settle();
        Q_EMIT finished(QString::fromStdString(file.id()), revision, Transition::ResourceLimit);
        return;
    }
    const auto id = file.id();
    const auto generation = generation_;
    const auto done = [this, id, revision, generation, bytes](const Outcome &result) {
        aligned(id, revision, generation, bytes, result);
    };
    if (!admission_.take(bytes)) return;
    try {
        running_.emplace(key, revision);
        runOffThread(
            workers_, this,
            [pending = file.pendingAlignment()]() -> Outcome {
                return {Transition::Applied, std::make_shared<const CorrespondenceMap>(pending->realigned())};
            },
            done, [](const std::exception_ptr &error) noexcept -> Outcome { return {computationFailure(error)}; });
    } catch (...) {
        const Outcome result{computationFailure(std::current_exception())};
        QMetaObject::invokeMethod(this, [done, result] { done(result); }, Qt::QueuedConnection);
    }
}

void BackgroundAlignment::resume()
{
    // The current files are the coalesced queue. No queued job retains a snapshot.
    for (std::size_t row = 0; row < store_.size(); ++row) request(static_cast<int>(row));
}

void BackgroundAlignment::aligned(const std::string &id, RevisionId revision, quint64 generation, std::size_t bytes,
                                  const Outcome &outcome)
{
    auto result = Transition::StaleRevision;
    if (generation == generation_) {
        const auto *file = store_.file(id);
        if (file && file->current().id == revision) {
            result = outcome.map ? store_.installAlignment(id, revision, outcome.map) : outcome.state;
            if (result != Transition::Applied && result != Transition::NoChange) failed_[id] = {revision, result};
        }
    }
    running_.erase({generation, id});
    admission_.release(bytes);
    settle();
    Q_EMIT finished(QString::fromStdString(id), revision, result);
    resume();
}
}
