#pragma once

#include "SessionState.h"

namespace branch_truce {

enum class ChoiceAction { Accept, Ignore, ResolveUsingSide };
enum class ChoiceScope { Line, Section };

struct ChoiceTarget {
    SourceAnchor source;
    ChoiceScope scope;
    std::string section;
    std::vector<std::string> decisions;
    std::vector<std::string> competitors;
    bool operator==(const ChoiceTarget &) const = default;
};

// Stable, immutable source scopes. A section groups the differing passages
// between the two captured sides; its line decisions are shared with line picks.
[[nodiscard]] std::vector<ChoiceTarget> choiceTargets(const FileSnapshots &snapshots);
[[nodiscard]] std::map<std::string, Decision> initialChoiceDecisions(const FileSnapshots &snapshots);

struct ChoicePreview {
    Transition state = Transition::InvalidAnchor;
    RevisionId revision = 0;
    ChoiceAction action = ChoiceAction::Accept;
    std::string target;
    std::string before;
    std::string replacement;
    std::string detail;
    EditingCommand command;
    bool operator==(const ChoicePreview &) const = default;
};

[[nodiscard]] ChoicePreview previewChoice(const FileSession &file, RevisionId expected,
                                         const std::string &target, ChoiceAction action);
Transition applyChoice(FileSession &file, const ChoicePreview &preview, bool confirmReplacement = false);

}
