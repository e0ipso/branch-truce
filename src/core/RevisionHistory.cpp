// Revision history stores a file's result revisions and the undo cursor over them.
#include "RevisionHistory.h"
#include "ResourceLimits.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace branch_truce {
std::size_t RevisionHistory::retainedBytes(const Revision &revision) noexcept
{
    auto bytes = sizeof(Stored) + sizeof(Revision) + 8 * sizeof(void *) + revision.draft.text.capacity() + 1;
    for (const auto &[id, decision] : revision.draft.decisions)
        bytes += sizeof(decltype(revision.draft.decisions)::value_type) + 4 * sizeof(void *) + id.capacity() + 1;
    if (revision.correspondence) bytes += revision.correspondence->retainedBytes();
    if (revision.markers)
        bytes += sizeof(MarkerScan) + revision.markers->blocks.capacity() * sizeof(MarkerRange)
            + revision.markers->orphans.capacity() * sizeof(MarkerLine);
    return bytes;
}

RevisionHistory::Stored::Stored(std::shared_ptr<const Revision> value) noexcept
    : revision(std::move(value)), bytes(RevisionHistory::retainedBytes(*revision))
{}

RevisionHistory::RevisionHistory(Revision launch)
    : current_(std::make_shared<const Revision>(std::move(launch))),
      entries_{current_->id},
      launch_(current_->id),
      next_(current_->id + 1)
{
    revisions_.emplace(launch_, current_);
}

const Revision *RevisionHistory::find(RevisionId id) const noexcept
{
    const auto found = revisions_.find(id);
    return found == revisions_.end() ? nullptr : found->second.revision.get();
}

RevisionHistory::Checkpoint RevisionHistory::checkpoint() const
{
    Checkpoint result{{}, entries_, cursor_, current_->id, launch_, next_};
    for (const auto &[id, stored] : revisions_) result.revisions.push_back(stored.revision);
    return result;
}

RevisionHistory RevisionHistory::restore(Checkpoint checkpoint, std::vector<RevisionId> held)
{
    RevisionHistory result;
    if (checkpoint.entries.empty() || checkpoint.entries.size() > kMaxHistoryEntries
        || checkpoint.cursor >= checkpoint.entries.size() || !checkpoint.next)
        throw std::invalid_argument("Invalid history checkpoint");
    for (const auto &revision : checkpoint.revisions) {
        if (!revision || !revision->id || revision->id >= checkpoint.next || !revision->result
            || revision->result > revision->id || !revision->decisions || revision->decisions > revision->id
            || !result.revisions_.emplace(revision->id, revision).second)
            throw std::invalid_argument("Invalid checkpoint revision");
    }
    for (auto id : checkpoint.entries)
        if (!result.find(id)) throw std::invalid_argument("Missing checkpoint history entry");
    for (auto id : held)
        if (!result.find(id)) throw std::invalid_argument("Missing protected checkpoint revision");
    if (!result.find(checkpoint.current) || !result.find(checkpoint.launch))
        throw std::invalid_argument("Missing checkpoint current or launch revision");
    const auto &current = *result.find(checkpoint.current);
    const auto &entry = *result.find(checkpoint.entries[checkpoint.cursor]);
    if (current.draft.text != entry.draft.text || current.draft.decisions != entry.draft.decisions
        || current.result != entry.result || current.decisions != entry.decisions)
        throw std::invalid_argument("Checkpoint cursor disagrees with current revision");
    result.entries_ = std::move(checkpoint.entries);
    result.cursor_ = checkpoint.cursor;
    result.current_ = result.revisions_.at(checkpoint.current).revision;
    result.launch_ = checkpoint.launch;
    result.next_ = checkpoint.next;
    result.held_ = std::move(held);
    const auto count = result.revisions_.size(), depth = result.entries_.size();
    result.prune();
    if (result.revisions_.size() != count || result.entries_.size() != depth)
        throw std::invalid_argument("Unreachable or oversized checkpoint history");
    return result;
}

void RevisionHistory::publish(Revision revision, Step step)
{
    if (revision.id < next_ || revision.id == std::numeric_limits<RevisionId>::max())
        throw std::logic_error("A published revision must take a new revision id");
    if ((step == Step::Undo && !canUndo()) || (step == Step::Redo && !canRedo()))
        throw std::logic_error("No history entry to move to");
    // Everything that allocates comes first, so a failure changes nothing.
    auto stored = std::make_shared<const Revision>(std::move(revision));
    if (step == Step::Edit || step == Step::Join) entries_.reserve(cursor_ + 2);
    revisions_.emplace(stored->id, stored);
    current_ = std::move(stored);
    next_ = current_->id + 1;
    switch (step) {
    case Step::Edit: append(current_->id); break;
    case Step::Join:
        if (cursor_ > 0) --cursor_;
        append(current_->id);
        break;
    case Step::Undo: --cursor_; break;
    case Step::Redo: ++cursor_; break;
    }
    prune();
}

void RevisionHistory::reopen(const Revision &revision)
{
    entries_.reserve(cursor_ + 2);
    auto found = revisions_.find(revision.id);
    if (found == revisions_.end())
        found = revisions_.emplace(revision.id, std::make_shared<const Revision>(revision)).first;
    current_ = found->second.revision;
    append(revision.id);
    prune();
}

void RevisionHistory::append(RevisionId id) noexcept
{
    // The caller reserved room for the new entry.
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(cursor_ + 1), entries_.end());
    entries_.push_back(id);
    cursor_ = entries_.size() - 1;
}

void RevisionHistory::replaceCurrent(Revision revision)
{
    if (revision.id != current_->id) throw std::logic_error("A replacement must keep the current revision id");
    auto stored = std::make_shared<const Revision>(std::move(revision));
    revisions_.at(stored->id) = Stored(stored);
    current_ = std::move(stored);
    prune();
}

void RevisionHistory::hold(std::vector<RevisionId> ids) noexcept
{
    held_ = std::move(ids);
    prune();
}

void RevisionHistory::prune() noexcept
{
    for (auto &[id, stored] : revisions_) stored.references = 0;
    const auto retain = [&](RevisionId id) {
        if (auto found = revisions_.find(id); found != revisions_.end()) ++found->second.references;
    };
    retain(launch_);
    retain(current_->id);
    for (const auto id : held_) retain(id);
    for (const auto id : entries_) retain(id);
    std::size_t bytes = 0;
    for (const auto &[id, stored] : revisions_)
        if (stored.references) bytes += stored.bytes;
    // Remove a prefix once. Counts handle reopened entries that repeat an id.
    std::size_t dropped = 0;
    while (dropped < cursor_ && (bytes > kMaxHistoryBytes || entries_.size() - dropped > kMaxHistoryEntries)) {
        auto &stored = revisions_.find(entries_[dropped++])->second;
        if (--stored.references == 0) bytes -= stored.bytes;
    }
    cursor_ -= dropped;
    entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(dropped));
    std::erase_if(revisions_, [](const auto &entry) { return entry.second.references == 0; });
}

}
