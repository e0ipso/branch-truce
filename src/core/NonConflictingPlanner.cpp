// Plans safe non-conflicting batches, skipping unavailable or colliding contributions.
#include "internal/EditingPlans.h"
#include <algorithm>
#include <numeric>
#include <tuple>

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
    std::vector<ByteRange> accepted;
    std::vector<std::size_t> maximumEnd;
    bool unmappedAccepted = false;
    BatchLimits &budget;
    bool refused = false;

    BatchPlanner(const PlanningInput &value, BatchLimits &limits) : input(value), budget(limits)
    {
        for (const auto &[id, decision] : input.draft.decisions) {
            if (decision.value != DecisionValue::Accepted) continue;
            if (!take(id.size() + 1, 64)) return;
            if (const auto range = mappedRange(input.mapping->anchor(id)))
                accepted.push_back(*range);
            else
                unmappedAccepted = true;
        }
        std::sort(accepted.begin(), accepted.end(),
                  [](auto a, auto b) { return std::tie(a.begin, a.end) < std::tie(b.begin, b.end); });
        for (const auto range : accepted)
            maximumEnd.push_back(std::max(range.end, maximumEnd.empty() ? 0 : maximumEnd.back()));
    }
    bool take(std::size_t count, std::size_t bytes)
    {
        if (count > budget.work || bytes > budget.bytes) {
            refused = true;
            return false;
        }
        budget.work -= count;
        budget.bytes -= bytes;
        return true;
    }
    // The change a target makes, or nothing when it must be skipped.
    [[nodiscard]] std::optional<Candidate> candidate(const NonConflictingTarget &target)
    {
        if (!take(target.decisions.size() + 1, 0)) return std::nullopt;
        for (const auto &id : target.decisions)
            if (!take(id.size(), 0)) return std::nullopt;
        if (std::any_of(target.decisions.begin(), target.decisions.end(), [&](const auto &id) {
                const auto found = input.draft.decisions.find(id);
                return found == input.draft.decisions.end() || found->second.value != DecisionValue::Undecided;
            }))
            return std::nullopt;
        const auto *anchor = input.mapping->anchor(target.earlier.contributionId);
        if (!mappedRange(anchor)) return std::nullopt;
        const auto source = slice(input.snapshots->source(target.source.side)->worktree.utf8, target.source.range);
        const auto base = slice(input.snapshots->earlier->worktree.utf8, target.earlier.range);
        // Includes repeated comparisons in indexed grouping, candidate copies and command output.
        const auto bytes = source.size() + base.size() + anchor->result->end - anchor->result->begin;
        if (bytes > budget.bytes / 16 || !take(bytes, bytes * 16)) {
            refused = true;
            return std::nullopt;
        }
        const auto replacement = std::string(source);
        anchor = present(target, *anchor, replacement);
        if (!anchor->result) return std::nullopt;
        const auto range = *anchor->result;
        if (range.end - range.begin > budget.bytes / 16
            || !take(range.end - range.begin, (range.end - range.begin) * 16)) {
            refused = true;
            return std::nullopt;
        }
        const auto before = std::string(slice(input.draft.text, range));
        if (before != replacement && (anchor->manualOverlap || before != base)) return std::nullopt;
        if (overlapsAccepted(range)) return std::nullopt;
        std::size_t metadata = sizeof(Candidate) + target.source.contributionId.size()
            + anchor->source.contributionId.size() + anchor->origin.sessionId.size() + anchor->origin.path.size()
            + anchor->origin.sourceObjectId.size();
        for (const auto &id : target.decisions) metadata += id.size() + sizeof(std::string) + 64;
        if (metadata > budget.bytes / 8 || !take(metadata, metadata * 8)) {
            refused = true;
            return std::nullopt;
        }
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
    [[nodiscard]] bool overlapsAccepted(ByteRange range) const
    {
        // A candidate's own decision IDs were all checked Undecided above,
        // so none can be one of these accepted contributions.
        if (unmappedAccepted) return true;
        const auto after = std::lower_bound(accepted.begin(), accepted.end(), range.end,
                                            [](auto at, auto end) { return at.begin < end; });
        const auto count = static_cast<std::size_t>(after - accepted.begin());
        if (count && maximumEnd[count - 1] > range.begin) return true;
        return range.begin == range.end && after != accepted.end() && *after == range;
    }
};

// Independent source ranges can collapse onto the same result passage.
// Reject both unless they describe exactly the same contribution.
void markCollisions(std::vector<Candidate> &candidates)
{
    std::vector<std::size_t> order(candidates.size());
    std::iota(order.begin(), order.end(), 0);
    const auto key = [&](std::size_t i) {
        const auto &c = candidates[i];
        return std::tie(c.change.result.begin, c.change.result.end, c.change.replacement, c.earlier.begin,
                        c.earlier.end);
    };
    std::sort(order.begin(), order.end(), [&](auto a, auto b) { return key(a) < key(b); });
    struct Group {
        std::size_t first, end;
        bool collision = false;
    };
    std::vector<Group> groups;
    for (std::size_t i = 0; i < order.size();) {
        const auto first = i++;
        while (i < order.size() && key(order[first]) == key(order[i])) ++i;
        groups.push_back({first, i});
    }
    const auto change = [&](std::size_t i) -> const NonConflictingChange & {
        return candidates[order[groups[i].first]].change;
    };
    std::size_t end = 0;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const auto range = change(i).result;
        groups[i].collision = end > range.begin || (i && sharesPlace(change(i - 1).result, range))
            || (i + 1 < groups.size() && sharesPlace(range, change(i + 1).result));
        end = std::max(end, range.end);
    }
    // Changing insertions at a nonempty range's starting edge collide too.
    for (std::size_t i = 0; i < groups.size();) {
        const auto first = i;
        std::size_t changing = 0;
        do {
            changing += change(i).before != change(i).replacement;
            ++i;
        } while (i < groups.size() && change(i).result.begin == change(first).result.begin);
        if (changing > 1)
            for (auto j = first; j < i; ++j)
                if (change(j).before != change(j).replacement) groups[j].collision = true;
    }
    for (const auto &group : groups)
        if (group.collision)
            for (auto i = group.first; i < group.end; ++i) candidates[order[i]].unavailable = true;
}

// Identical changes from both sides appear once in a combined batch.
void addCandidate(BatchPlan &preview, Candidate candidate,
                  std::map<std::pair<std::size_t, std::size_t>, std::size_t> &index)
{
    const auto range = candidate.change.result;
    const auto [duplicate, inserted] = index.emplace(std::pair{range.begin, range.end}, preview.changes.size());
    if (!inserted) {
        preview.changes[duplicate->second].sources.push_back(candidate.change.sources.front());
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
                             RevisionId expected, NonConflictingScope scope, BatchLimits budget)
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
    if (targets.size() > budget.targets || input.draft.decisions.size() > budget.work)
        return refused(std::move(preview), Transition::ResourceLimit, BatchNote::FileNotEditable);
    budget.work -= input.draft.decisions.size();
    BatchPlanner planner{input, budget};
    if (planner.refused) return refused(std::move(preview), Transition::ResourceLimit, BatchNote::FileNotEditable);
    std::vector<Candidate> candidates;
    for (const auto &target : targets) {
        if (scope != NonConflictingScope::Both
            && target.source.side != (scope == NonConflictingScope::Left ? SourceSide::Stage2 : SourceSide::Stage3))
            continue;
        auto candidate = planner.candidate(target);
        if (planner.refused) return refused(std::move(preview), Transition::ResourceLimit, BatchNote::FileNotEditable);
        if (!candidate) {
            ++preview.skipped;
            continue;
        }
        if (candidate->change.before == candidate->change.replacement && target.decisions.empty()) continue;
        candidates.push_back(std::move(*candidate));
    }
    markCollisions(candidates);
    std::map<std::pair<std::size_t, std::size_t>, std::size_t> index;
    for (auto &candidate : candidates) {
        if (candidate.unavailable)
            ++preview.skipped;
        else
            addCandidate(preview, std::move(candidate), index);
    }
    preview.state = preview.changes.empty() ? Transition::NoChange : Transition::ConfirmationRequired;
    preview.note = BatchNote::Batch;
    return preview;
}

}
