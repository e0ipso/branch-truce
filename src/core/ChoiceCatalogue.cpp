// The choice catalogue lists the source scopes a user can accept or ignore.
#include "ChoiceCatalogue.h"

#include <algorithm>
#include <set>

namespace branch_truce {
namespace {
// Decision and contribution IDs are recorded in recovery journals, so each
// side has a fixed token that does not follow the enumeration's order.
const char *sideToken(SourceSide side) noexcept
{
    switch (side) {
    case SourceSide::Earlier: return "0";
    case SourceSide::Stage2: return "1";
    case SourceSide::Stage3: return "2";
    }
    return "?";
}

std::string rangeId(SourceSide side, ByteRange range)
{
    return std::string(sideToken(side)) + "/" + std::to_string(range.begin) + "/" + std::to_string(range.end);
}

// One line pick per line of `range`, or one empty pick for an empty range.
std::vector<SourceAnchor> lines(SourceSide side, std::string_view text, ByteRange range)
{
    std::vector<SourceAnchor> result;
    auto begin = range.begin;
    do {
        const ByteRange line{begin, std::min(range.end, lineAt(text, begin).end)};
        result.push_back({"pick/" + rangeId(side, line), side, line});
        begin = line.end;
    } while (begin < range.end);
    return result;
}

void addSection(std::vector<ChoiceTarget> &result, const std::string &section, SourceSide side, ByteRange range,
                const std::vector<SourceAnchor> &picks)
{
    std::vector<std::string> ids;
    ids.reserve(picks.size());
    for (const auto &pick : picks) ids.push_back(pick.contributionId);
    result.push_back({{"section/" + rangeId(side, range), side, range}, ChoiceScope::Section, section, ids});
    for (const auto &pick : picks)
        if (pick.range.begin != pick.range.end)
            result.push_back({pick, ChoiceScope::Line, section, {pick.contributionId}});
}

// Insertions at a changed range's boundary can depend on its ordering, so
// they compete with that range too.
bool competes(ByteRange first, ByteRange second)
{
    if (first.begin == first.end) return second.begin <= first.begin && first.begin <= second.end;
    if (second.begin == second.end) return first.begin <= second.begin && second.begin <= first.end;
    return first == second || overlaps(first, second);
}

// The changes of one side against the earlier version that the other side's
// changes leave alone. Identical changes on both sides do not compete.
struct SideChanges {
    SourceSide side;
    const std::vector<Passage> &own;
    const std::vector<Passage> &other;
    std::string_view text;
    std::string_view otherText;

    [[nodiscard]] bool competing(const Passage &passage) const
    {
        const auto replacement = slice(text, passage.result);
        return std::any_of(other.begin(), other.end(), [&](const auto &candidate) {
            if (candidate.kind == PassageKind::Equal || !competes(passage.source, candidate.source)) return false;
            return candidate.kind == PassageKind::Ambiguous || passage.source != candidate.source
                || replacement != slice(otherText, candidate.result);
        });
    }

    void add(std::vector<NonConflictingTarget> &result, const std::set<std::string, std::less<>> &decisions) const
    {
        for (const auto &passage : own) {
            if (passage.kind == PassageKind::Equal || passage.kind == PassageKind::Ambiguous || competing(passage))
                continue;
            const auto id = "non-conflicting/" + rangeId(side, passage.result);
            NonConflictingTarget target{
                {id + "/earlier", SourceSide::Earlier, passage.source}, {id, side, passage.result}, {}};
            for (const auto &line : lines(side, text, passage.result))
                if (decisions.contains(line.contributionId)) target.decisions.push_back(line.contributionId);
            result.push_back(std::move(target));
        }
    }
};
}

std::vector<ChoiceTarget> choiceTargets(const FileSnapshots &snapshots, const CorrespondenceAlgorithm &compare)
{
    if (!snapshots.stage2 || !snapshots.stage3) return {};
    const auto &left = snapshots.stage2->worktree.utf8;
    const auto &right = snapshots.stage3->worktree.utf8;
    std::vector<ChoiceTarget> result;
    for (const auto &passage : compare.compare(left, right)) {
        if (passage.kind == PassageKind::Equal) continue;
        const auto section =
            "region/" + rangeId(SourceSide::Stage2, passage.source) + "/" + rangeId(SourceSide::Stage3, passage.result);
        addSection(result, section, SourceSide::Stage2, passage.source,
                   lines(SourceSide::Stage2, left, passage.source));
        addSection(result, section, SourceSide::Stage3, passage.result,
                   lines(SourceSide::Stage3, right, passage.result));
    }
    return result;
}

std::vector<NonConflictingTarget> nonConflictingTargets(const FileSnapshots &snapshots,
                                                        const std::vector<ChoiceTarget> &choices,
                                                        const CorrespondenceAlgorithm &compare)
{
    if (!snapshots.earlier || !snapshots.stage2 || !snapshots.stage3) return {};
    const auto &earlier = snapshots.earlier->worktree.utf8;
    const auto &left = snapshots.stage2->worktree.utf8;
    const auto &right = snapshots.stage3->worktree.utf8;
    const auto a = compare.compare(earlier, left);
    const auto b = compare.compare(earlier, right);
    std::set<std::string, std::less<>> decisions;
    for (const auto &choice : choices) decisions.insert(choice.decisions.begin(), choice.decisions.end());
    std::vector<NonConflictingTarget> result;
    SideChanges{SourceSide::Stage2, a, b, left, right}.add(result, decisions);
    SideChanges{SourceSide::Stage3, b, a, right, left}.add(result, decisions);
    return result;
}

ChoiceCatalogue::ChoiceCatalogue(std::vector<ChoiceTarget> targets) : targets_(std::move(targets))
{
    for (std::size_t i = 0; i < targets_.size(); ++i) {
        const auto &target = targets_[i];
        byContribution_.emplace(target.source.contributionId, i);
        if (target.scope == ChoiceScope::Section) sections_.emplace(std::pair{target.section, target.source.side}, i);
    }
}

const ChoiceTarget *ChoiceCatalogue::find(const std::string &contributionId) const noexcept
{
    const auto found = byContribution_.find(contributionId);
    return found == byContribution_.end() ? nullptr : &targets_[found->second];
}

const ChoiceTarget *ChoiceCatalogue::section(const std::string &section, SourceSide side) const noexcept
{
    const auto found = sections_.find(std::pair{section, side});
    return found == sections_.end() ? nullptr : &targets_[found->second];
}

const ChoiceTarget *ChoiceCatalogue::opposite(const ChoiceTarget &target) const noexcept
{
    for (const auto side : {SourceSide::Stage2, SourceSide::Stage3})
        if (side != target.source.side)
            if (const auto *found = section(target.section, side)) return found;
    return nullptr;
}

}
