// Immutable inputs and command-bearing results shared by synchronous editing planners.
#pragma once

#include "../EditingCommands.h"
#include "../MarkerResolution.h"

namespace branch_truce::detail {
// Borrowed only for the planning call. No session, history or publication access.
struct PlanningInput {
    const std::string &id;
    const std::string &path;
    RevisionId revision;
    const Draft &draft;
    const FileSnapshots *snapshots;
    const CorrespondenceMap *mapping;
    const ChoiceCatalogue &choices;
    bool editable;
    bool ready() const { return editable && snapshots && mapping; }
};

// Applying regenerates the plan and checks every displayed value and its token.
template <typename Preview> struct Planned : Preview {
    EditingCommand &command() { return this->apply.command_; }
    const EditingCommand &command() const { return this->apply.command_; }
};
using ChoicePlan = Planned<ChoicePreview>;
using BatchPlan = Planned<NonConflictingPreview>;
using WholeFilePlan = Planned<WholeFilePreview>;

template <typename Plan, typename Note> Plan refused(Plan preview, Transition state, Note note)
{
    preview.state = state;
    preview.note = note;
    return preview;
}

// Equal insertion sites claim the same place even though they do not overlap.
inline bool sharesPlace(ByteRange a, ByteRange b) { return a == b || overlaps(a, b); }
inline std::optional<ByteRange> mappedRange(const ResultAnchor *anchor) noexcept
{
    return anchor && anchor->state == AnchorState::Mapped ? anchor->result : std::nullopt;
}
inline std::string sourceText(const FileSnapshots &snapshots, const SourceAnchor &source)
{
    return std::string(slice(snapshots.source(source.side)->worktree.utf8, source.range));
}

ChoicePlan planChoice(const PlanningInput &, MarkerPlanner, const MarkerScan &, RevisionId, const std::string &,
                      ChoiceAction);
BatchPlan planNonConflicting(const PlanningInput &, const std::vector<NonConflictingTarget> &, RevisionId,
                             NonConflictingScope);
WholeFilePlan planWholeFile(const PlanningInput &, RevisionId, SourceSide);
}
