// Source choice commands apply accept, ignore and combined-side decisions to a result revision.
#pragma once

#include "SessionState.h"

namespace branch_truce {

enum class ChoiceAction { Accept, Ignore, ResolveUsingSide, CombineLeftFirst, CombineRightFirst };
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
    // The exact result scope being replaced, including any protected manual
    // work. Ordered sources are captured explicitly, never inferred from text.
    std::optional<ByteRange> affectedRange;
    bool replacesManual = false;
    std::vector<SourceAnchor> sourceOrder;
    EditingCommand command;
    bool operator==(const ChoicePreview &) const = default;
};

[[nodiscard]] ChoicePreview previewChoice(const FileSession &file, RevisionId expected,
                                         const std::string &target, ChoiceAction action);
Transition applyChoice(FileSession &file, const ChoicePreview &preview, bool confirmReplacement = false);

enum class NonConflictingScope { Left, Right, Both };

struct NonConflictingTarget {
    SourceAnchor earlier;
    SourceAnchor source;
    std::vector<std::string> decisions;
};

// Only exact, non-competing changes relative to an available earlier capture.
// Decision IDs belong to the existing scoped-choice vocabulary.
[[nodiscard]] std::vector<NonConflictingTarget> nonConflictingTargets(
    const FileSnapshots &snapshots, const std::vector<ChoiceTarget> &choices);

struct NonConflictingChange {
    std::vector<SourceAnchor> sources;
    ByteRange result;
    std::string before;
    std::string replacement;
    bool operator==(const NonConflictingChange &) const = default;
};

struct NonConflictingPreview {
    Transition state = Transition::Blocked;
    RevisionId revision = 0;
    NonConflictingScope scope = NonConflictingScope::Both;
    std::string path;
    std::string detail;
    std::size_t skipped = 0;
    std::vector<NonConflictingChange> changes;
    EditingCommand command;
    bool operator==(const NonConflictingPreview &) const = default;
};

[[nodiscard]] NonConflictingPreview previewNonConflicting(const FileSession &file, RevisionId expected,
    NonConflictingScope scope = NonConflictingScope::Both);
Transition applyNonConflicting(FileSession &file, const NonConflictingPreview &preview, bool confirm = false);

struct WholeFileContribution {
    SourceAnchor source;
    Decision before;
    Decision after;
    bool operator==(const WholeFileContribution &) const = default;
};

struct WholeFilePreview {
    Transition state = Transition::Blocked;
    RevisionId revision = 0;
    std::string fileId;
    AnchorOrigin origin;
    SourceSide side = SourceSide::Stage2;
    std::string before;
    std::string replacement;
    std::string detail;
    std::vector<WholeFileContribution> contributions;
    // Source ranges address replacement; result ranges address before. These
    // show all differences, including manual work, without guessing provenance.
    std::vector<Passage> differences;
    std::size_t retainedDecisions = 0;
    EditingCommand command;
    bool operator==(const WholeFilePreview &) const = default;
};

[[nodiscard]] WholeFilePreview previewWholeFile(const FileSession &file, RevisionId expected, SourceSide side);
Transition applyWholeFile(FileSession &file, const WholeFilePreview &preview, bool confirm = false);

}
