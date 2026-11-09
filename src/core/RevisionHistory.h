// Revision history stores a file's result revisions and the undo cursor over them.
#pragma once

#include "ConflictMarkers.h"
#include "Correspondence.h"
#include "Draft.h"
#include "RevisionId.h"
#include <map>
#include <memory>
#include <vector>

namespace branch_truce {

struct Revision {
    RevisionId id;
    // The revisions whose result text and whose decisions this one repeats.
    RevisionId result;
    RevisionId decisions;
    Draft draft;
    std::shared_ptr<const CorrespondenceMap> correspondence = {};
    // The marker scan of draft.text, computed once when the revision is made.
    std::shared_ptr<const MarkerScan> markers = {};
};

// Undo history over immutable, shared revisions. Undo and redo do not move
// back to an entry: they publish a fresh copy of it, so every revision a
// caller has seen keeps its identity. A copy of the history shares every
// revision with the original.
class RevisionHistory {
public:
    RevisionHistory() = default;
    explicit RevisionHistory(Revision launch);

    [[nodiscard]] const Revision &current() const noexcept { return *current_; }
    [[nodiscard]] RevisionId launchId() const noexcept { return launch_; }
    [[nodiscard]] RevisionId nextId() const noexcept { return next_; }
    // The stored revision with this id, or nullptr once it is unreachable.
    [[nodiscard]] const Revision *find(RevisionId id) const noexcept;
    [[nodiscard]] std::size_t depth() const noexcept { return entries_.size(); }
    [[nodiscard]] bool canUndo() const noexcept { return cursor_ > 0; }
    [[nodiscard]] bool canRedo() const noexcept { return cursor_ + 1 < entries_.size(); }
    // The entries undo and redo restore copies of. Only valid when allowed.
    [[nodiscard]] const Revision &previous() const { return *revisions_.at(entries_.at(cursor_ - 1)); }
    [[nodiscard]] const Revision &following() const { return *revisions_.at(entries_.at(cursor_ + 1)); }

    // How a published revision moves the undo cursor: an edit becomes the
    // newest entry and replaces every entry after the cursor, while undo and
    // redo move to the entry their revision copies.
    enum class Step { Edit, Undo, Redo };
    // Makes `revision`, whose id must be nextId(), current.
    void publish(Revision revision, Step step);
    // Gives the current revision a new value under the same id.
    void replaceCurrent(Revision revision);
    // Makes an earlier revision current again as a new entry, storing it
    // again if it was dropped.
    void reopen(const Revision &revision);

private:
    void append(RevisionId id);
    void prune();
    std::map<RevisionId, std::shared_ptr<const Revision>> revisions_;
    std::shared_ptr<const Revision> current_;
    std::vector<RevisionId> entries_;
    std::size_t cursor_ = 0;
    RevisionId launch_ = 0;
    RevisionId next_ = 1;
};

}
