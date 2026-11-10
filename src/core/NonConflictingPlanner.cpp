// Plans safe non-conflicting batches, skipping unavailable or colliding contributions.
#include "internal/EditingPlans.h"
#include <algorithm>

namespace branch_truce::detail {
namespace {
// A batch change, with what applying it touches.
struct Candidate {
    NonConflictingChange change;
    ResultAnchor anchor;
    ByteRange earlier;
    std::vector<std::string> decisions;
    bool unavailable = false;
};

struct BatchPlanner {
    const PlanningInput &input;
    // The change a target makes, or nothing when it must be skipped.
    [[nodiscard]] std::optional<Candidate> candidate(const NonConflictingTarget &target) const
    {
        if (std::any_of(target.decisions.begin(), target.decisions.end(), [&](const auto &id) {
                const auto found = input.draft.decisions.find(id);
                return found == input.draft.decisions.end() || found->second.value != DecisionValue::Undecided;
            }))
            return std::nullopt;
        const auto *anchor = input.mapping->anchor(target.earlier.contributionId);
        if (!mappedRange(anchor)) return std::nullopt;
        const auto replacement = sourceText(*input.snapshots, target.source);
        anchor = present(target, *anchor, replacement);
        if (!anchor->result) return std::nullopt;
        const auto range = *anchor->result;
        const auto before = std::string(slice(input.draft.text, range));
        const auto base = sourceText(*input.snapshots, target.earlier);
        if (before != replacement && (anchor->manualOverlap || before != base)) return std::nullopt;
        if (overlapsAccepted(target, range)) return std::nullopt;
        return Candidate{
            {{target.source}, range, before, replacement}, *anchor, target.earlier.range, target.decisions};
    }

    // An insertion already present in the result lies beside its empty
    // earlier anchor. Its own registered source range identifies it exactly.
    [[nodiscard]] const ResultAnchor *present(const NonConflictingTarget &target, const ResultAnchor &earlier,
                                              const std::string &replacement) const
    {
        if (target.earlier.range.begin != target.earlier.range.end) return &earlier;
        const auto *inserted = input.mapping->anchor(target.source.contributionId);
        const auto range = mappedRange(inserted);
        if (!earlier.result || !range) return &earlier;
        const auto at = earlier.result->begin;
        if ((at == range->begin || at == range->end) && slice(input.draft.text, *range) == replacement) return inserted;
        return &earlier;
    }

    // Never overwrite another accepted contribution, even if it happens to
    // contain the earlier bytes. Other decisions are preserved verbatim.
    [[nodiscard]] bool overlapsAccepted(const NonConflictingTarget &target, ByteRange range) const
    {
        return std::any_of(input.draft.decisions.begin(), input.draft.decisions.end(), [&](const auto &entry) {
            if (entry.second.value != DecisionValue::Accepted
                || std::find(target.decisions.begin(), target.decisions.end(), entry.first) != target.decisions.end())
                return false;
            const auto *accepted = input.mapping->anchor(entry.first);
            const auto place = mappedRange(accepted);
            return !place || sharesPlace(range, *place);
        });
    }
};

// Independent source ranges can collapse onto the same result passage.
// Reject both unless they describe exactly the same contribution.
void markCollisions(std::vector<Candidate> &candidates)
{
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        for (std::size_t j = i + 1; j < candidates.size(); ++j) {
            const auto &a = candidates[i].change;
            const auto &b = candidates[j].change;
            const bool collides = sharesPlace(a.result, b.result)
                || (a.before != a.replacement && b.before != b.replacement && a.result.begin == b.result.begin);
            if (collides
                && (a.result != b.result || a.replacement != b.replacement
                    || candidates[i].earlier != candidates[j].earlier)) {
                candidates[i].unavailable = true;
                candidates[j].unavailable = true;
            }
        }
    }
}

// Identical changes from both sides appear once in a combined batch.
void addCandidate(BatchPlan &preview, Candidate candidate)
{
    const auto duplicate = std::find_if(preview.changes.begin(), preview.changes.end(), [&](const auto &change) {
        return change.result == candidate.change.result && change.replacement == candidate.change.replacement;
    });
    if (duplicate != preview.changes.end()) {
        duplicate->sources.push_back(candidate.change.sources.front());
    } else {
        if (candidate.change.before != candidate.change.replacement)
            preview.command().edits.push_back({candidate.change.result, candidate.change.replacement});
        preview.changes.push_back(std::move(candidate.change));
        preview.command().targets.push_back(std::move(candidate.anchor));
    }
    for (const auto &id : candidate.decisions) preview.command().decisions[id] = {DecisionValue::Accepted, {}};
}
}

BatchPlan planNonConflicting(const PlanningInput &input, const std::vector<NonConflictingTarget> &targets,
                             RevisionId expected, NonConflictingScope scope)
{
    BatchPlan preview;
    preview.revision = expected;
    preview.scope = scope;
    preview.path = input.path;
    preview.command().expected = expected;
    if (expected != input.revision)
        return refused(std::move(preview), Transition::StaleRevision, BatchNote::StaleRevision);
    if (!input.ready()) return preview;
    if (!input.snapshots->earlier) return refused(std::move(preview), Transition::Blocked, BatchNote::NoEarlierVersion);
    const BatchPlanner planner{input};
    std::vector<Candidate> candidates;
    for (const auto &target : targets) {
        if (scope != NonConflictingScope::Both
            && target.source.side != (scope == NonConflictingScope::Left ? SourceSide::Stage2 : SourceSide::Stage3))
            continue;
        auto candidate = planner.candidate(target);
        if (!candidate) {
            ++preview.skipped;
            continue;
        }
        if (candidate->change.before == candidate->change.replacement && target.decisions.empty()) continue;
        candidates.push_back(std::move(*candidate));
    }
    markCollisions(candidates);
    for (auto &candidate : candidates) {
        if (candidate.unavailable)
            ++preview.skipped;
        else
            addCandidate(preview, std::move(candidate));
    }
    preview.state = preview.changes.empty() ? Transition::NoChange : Transition::ConfirmationRequired;
    preview.note = BatchNote::Batch;
    return preview;
}

}
