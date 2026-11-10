// Atomic editing command values, independent of session ownership.
#pragma once

#include "Correspondence.h"
#include "Draft.h"

namespace branch_truce {

// One atomic history entry. Future composition and bulk planners can submit
// mapped edits and explicit ordered decisions through the same boundary.
enum class EditingScope { Mapped, WholeFile };
struct EditingCommand {
    RevisionId expected = 0;
    std::vector<ResultAnchor> targets;
    std::vector<ResultEdit> edits;
    std::map<std::string, Decision> decisions;
    EditingScope scope = EditingScope::Mapped;
    bool operator==(const EditingCommand &) const = default;
};

}
