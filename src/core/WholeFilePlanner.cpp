// Plans whole-file replacement and reports differing provenance and decision changes.
#include "internal/EditingPlans.h"

namespace branch_truce::detail {
WholeFilePlan planWholeFile(const PlanningInput &input, RevisionId expected, SourceSide side)
{
    WholeFilePlan preview;
    preview.revision = expected;
    preview.fileId = input.id;
    preview.side = side;
    if (expected != input.revision)
        return refused(std::move(preview), Transition::StaleRevision, WholeFileNote::StaleRevision);
    if (!input.ready()) return preview;
    if (side != SourceSide::Stage2 && side != SourceSide::Stage3)
        return refused(std::move(preview), Transition::InvalidAnchor, WholeFileNote::InvalidSide);
    const auto &captured = *input.snapshots;
    const auto &source = captured.source(side);
    if (!source) return refused(std::move(preview), Transition::Blocked, WholeFileNote::SourceUnavailable);
    const auto &draft = input.draft;
    const auto &mapping = *input.mapping;
    preview.origin = {captured.provenance.sessionId, input.path, source->objectId};
    preview.before = draft.text;
    preview.replacement = source->worktree.utf8;
    preview.command().expected = expected;
    preview.command().scope = EditingScope::WholeFile;
    preview.command().edits.push_back({{0, preview.before.size()}, preview.replacement});
    for (const auto &target : input.choices.targets()) {
        if (target.scope != ChoiceScope::Section) continue;
        const Decision after{target.source.side == side ? DecisionValue::Accepted : DecisionValue::Ignored, {}};
        for (const auto &id : target.decisions) {
            const auto found = draft.decisions.find(id);
            const auto *anchor = mapping.anchor(id);
            // Earlier recovered drafts keep their exact decision vocabulary.
            if (found == draft.decisions.end() || preview.command().decisions.contains(id) || !anchor) continue;
            preview.contributions.push_back({anchor->source, found->second, after});
            preview.command().decisions.emplace(id, after);
        }
    }
    preview.retainedDecisions = draft.decisions.size() - preview.command().decisions.size();
    if (const auto *passages = mapping.passages(side))
        for (const auto &passage : *passages)
            if (passage.kind != PassageKind::Equal) preview.differences.push_back(passage);
    preview.state = Transition::ConfirmationRequired;
    preview.note = WholeFileNote::Replace;
    return preview;
}

}
