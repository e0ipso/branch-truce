// Revision history stores a file's result revisions and the undo cursor over them.
#include "RevisionHistory.h"
#include "ResourceLimits.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace branch_truce {
RevisionHistory::Stored::Stored(std::shared_ptr<const Revision> value) noexcept : revision(std::move(value))
{
    bytes = sizeof(Stored) + sizeof(Revision) + 8 * sizeof(void *) + revision->draft.text.capacity() + 1;
    for (const auto &[id, decision] : revision->draft.decisions)
        bytes += sizeof(decltype(revision->draft.decisions)::value_type) + 4 * sizeof(void *) + id.capacity() + 1;
    if (revision->correspondence) bytes += revision->correspondence->retainedBytes();
    if (revision->markers)
        bytes += sizeof(MarkerScan) + revision->markers->blocks.capacity() * sizeof(MarkerRange)
            + revision->markers->orphans.capacity() * sizeof(MarkerLine);
}

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
