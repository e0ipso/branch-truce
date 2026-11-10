// Source choice commands apply accept, ignore and combined-side decisions to a result revision.
#pragma once

#include "SessionState.h"

namespace branch_truce {
namespace detail {
template <typename Preview> struct Planned;
}

// Revision-bound application data. Only the engine can inspect or create it.
class ApplyToken {
public:
    bool operator==(const ApplyToken &) const = default;

private:
    template <typename Preview> friend struct detail::Planned;
    EditingCommand command_;
};

enum class ChoiceAction { Accept, Ignore, ResolveUsingSide, CombineLeftFirst, CombineRightFirst };

// What an applicable choice preview does, or why a preview was refused. The
// app turns it into text. Previews are blocked until alignment is installed.
enum class ChoiceNote {
    // Applicable: the preview's action, as its state says.
    Ignore,
    Accept,
    ResolveUsingSide,
    Combine,
    // Refused.
    StaleRevision,
    FileNotEditable,
    UnknownScope,
    UnmappedScope,
    NoRecordedChoices,
    AlreadyDecided,
    SeveralCompetitors,
    NoCompetitor,
    UnplacedSectionLines,
    ReplacesUnselected,
    UncertainDeletion,
    AcceptedOverlap,
    MissingCompetitor,
    UncertainCompetingLine,
    NoExactCompetingLine,
    CompetingAccepted,
    NoCompetingLine
};

// What an applicable choice does to the conflict-marker block around it.
enum class MarkerEffect {
    None,
    // The choice decides the block's last open contribution, so its marker
    // lines and ignored text go.
    Removed,
    // A combination in a block with other sections: the markers stay until
    // every section is decided, then the block holds this order.
    KeptUntilDecided
};

struct ChoicePreview {
    Transition state = Transition::InvalidAnchor;
    RevisionId revision = 0;
    ChoiceAction action = ChoiceAction::Accept;
    std::string target;
    std::string before;
    std::string replacement;
    ChoiceNote note = ChoiceNote::UnknownScope;
    MarkerEffect markers = MarkerEffect::None;
    // The exact result scope being replaced, including any protected manual
    // work. Ordered sources are captured explicitly, never inferred from text.
    std::optional<ByteRange> affectedRange;
    bool replacesManual = false;
    std::vector<SourceAnchor> sourceOrder;
    ApplyToken apply;
    bool operator==(const ChoicePreview &) const = default;
};

[[nodiscard]] ChoicePreview previewChoice(const FileSession &file, RevisionId expected, const std::string &target,
                                          ChoiceAction action);
Transition applyChoice(FileSession &file, const ChoicePreview &preview, bool confirmReplacement = false);

enum class NonConflictingScope { Left, Right, Both };

// What a non-conflicting batch preview offers, or why it offers nothing.
enum class BatchNote { Batch, StaleRevision, FileNotEditable, NoEarlierVersion };

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
    BatchNote note = BatchNote::FileNotEditable;
    std::size_t skipped = 0;
    std::vector<NonConflictingChange> changes;
    ApplyToken apply;
    bool operator==(const NonConflictingPreview &) const = default;
};

[[nodiscard]] NonConflictingPreview previewNonConflicting(const FileSession &file, RevisionId expected,
                                                          NonConflictingScope scope = NonConflictingScope::Both);
Transition applyNonConflicting(FileSession &file, const NonConflictingPreview &preview, bool confirm = false);

// What a whole-file preview replaces, or why it cannot.
enum class WholeFileNote { Replace, StaleRevision, NoCapturedSource, InvalidSide, SourceUnavailable };

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
    WholeFileNote note = WholeFileNote::NoCapturedSource;
    std::vector<WholeFileContribution> contributions;
    // Source ranges address replacement; result ranges address before. These
    // show all differences, including manual work, without guessing provenance.
    std::vector<Passage> differences;
    std::size_t retainedDecisions = 0;
    ApplyToken apply;
    bool operator==(const WholeFilePreview &) const = default;
};

[[nodiscard]] WholeFilePreview previewWholeFile(const FileSession &file, RevisionId expected, SourceSide side);
Transition applyWholeFile(FileSession &file, const WholeFilePreview &preview, bool confirm = false);

}
