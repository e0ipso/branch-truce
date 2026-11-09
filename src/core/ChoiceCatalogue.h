// The choice catalogue lists the source scopes a user can accept or ignore.
#pragma once

#include "Correspondence.h"
#include "FileSnapshot.h"
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace branch_truce {

enum class ChoiceScope { Line, Section };

struct ChoiceTarget {
    SourceAnchor source;
    ChoiceScope scope;
    std::string section;
    std::vector<std::string> decisions;
    bool operator==(const ChoiceTarget &) const = default;
};

// A change one side made against the earlier version that no change of the
// other side competes with.
struct NonConflictingTarget {
    SourceAnchor earlier;
    SourceAnchor source;
    std::vector<std::string> decisions;
};

// Stable, immutable source scopes. A section groups the differing passages
// between the two captured sides; its line decisions are shared with line
// picks. `compare` aligns the left side with the right side.
[[nodiscard]] std::vector<ChoiceTarget> choiceTargets(const FileSnapshots &snapshots,
                                                      const CorrespondenceAlgorithm &compare);

// Only exact, non-competing changes relative to an available earlier capture.
// Decision IDs belong to the existing scoped-choice vocabulary.
[[nodiscard]] std::vector<NonConflictingTarget> nonConflictingTargets(const FileSnapshots &snapshots,
                                                                      const std::vector<ChoiceTarget> &choices,
                                                                      const CorrespondenceAlgorithm &compare);

// The targets of one file, indexed once so lookups by contribution or by
// section do not scan the whole list.
class ChoiceCatalogue {
public:
    ChoiceCatalogue() = default;
    explicit ChoiceCatalogue(std::vector<ChoiceTarget> targets);

    [[nodiscard]] const std::vector<ChoiceTarget> &targets() const noexcept { return targets_; }
    [[nodiscard]] const ChoiceTarget *find(const std::string &contributionId) const noexcept;
    // The section scope of `side` in `section`, or nullptr.
    [[nodiscard]] const ChoiceTarget *section(const std::string &section, SourceSide side) const noexcept;
    // The section scope across from `target`: the other side's scope of its section.
    [[nodiscard]] const ChoiceTarget *opposite(const ChoiceTarget &target) const noexcept;

private:
    std::vector<ChoiceTarget> targets_;
    std::map<std::string, std::size_t, std::less<>> byContribution_;
    std::map<std::pair<std::string, SourceSide>, std::size_t> sections_;
};

}
