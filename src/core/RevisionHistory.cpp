// Revision history stores a file's result revisions and the undo cursor over them.
#include "RevisionHistory.h"

#include <algorithm>
#include <stdexcept>

namespace branch_truce {
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
    return found == revisions_.end() ? nullptr : found->second.get();
}

void RevisionHistory::publish(Revision revision, Step step)
{
    if (revision.id != next_) throw std::logic_error("A published revision must take the next revision id");
    if ((step == Step::Undo && !canUndo()) || (step == Step::Redo && !canRedo()))
        throw std::logic_error("No history entry to move to");
    auto stored = std::make_shared<const Revision>(std::move(revision));
    revisions_.emplace(stored->id, stored);
    current_ = std::move(stored);
    ++next_;
    switch (step) {
    case Step::Edit: append(current_->id); break;
    case Step::Undo: --cursor_; break;
    case Step::Redo: ++cursor_; break;
    }
    prune();
}

void RevisionHistory::replaceCurrent(Revision revision)
{
    if (revision.id != current_->id) throw std::logic_error("A replaced revision must keep its id");
    auto stored = std::make_shared<const Revision>(std::move(revision));
    revisions_[stored->id] = stored;
    current_ = std::move(stored);
}

void RevisionHistory::reopen(const Revision &revision)
{
    auto found = revisions_.find(revision.id);
    if (found == revisions_.end())
        found = revisions_.emplace(revision.id, std::make_shared<const Revision>(revision)).first;
    current_ = found->second;
    append(revision.id);
    prune();
}

void RevisionHistory::append(RevisionId id)
{
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(cursor_ + 1), entries_.end());
    entries_.push_back(id);
    cursor_ = entries_.size() - 1;
}

void RevisionHistory::prune()
{
    // Every revision stays stored, as before the history was split out.
}

}
