// Marker resolution ties conflict-marker blocks to choice sections and plans the edits that remove them.
#include "MarkerResolution.h"

#include <array>
#include <cstdint>
#include <utility>

namespace branch_truce {
namespace {
// Index of a side region: 0 for the left side, 1 for the right side.
std::size_t sideIndex(SourceSide side) noexcept { return side == SourceSide::Stage3 ? 1 : 0; }

ByteRange sideRegion(const MarkerRange &block, SourceSide side)
{
    if (side == SourceSide::Stage3) return {block.separator.bytes.end, block.end.bytes.begin};
    return {block.begin.bytes.end, block.base ? block.base->bytes.begin : block.separator.bytes.begin};
}

std::array<ByteRange, 2> sideRegions(const MarkerRange &block)
{
    return {sideRegion(block, SourceSide::Stage2), sideRegion(block, SourceSide::Stage3)};
}

BlockFrame frame(const MarkerRange &block, std::string_view text)
{
    const auto base = block.base ? block.base->bytes.begin : block.separator.bytes.begin;
    return {std::string(slice(text, block.begin.bytes)), std::string(slice(text, {base, block.separator.bytes.begin})),
            std::string(slice(text, block.separator.bytes)), std::string(slice(text, block.end.bytes))};
}

void addPassages(MarkerContext &context, SourceSide side, const std::vector<Passage> &passages)
{
    for (const auto &passage : passages) {
        if (side == SourceSide::Earlier && passage.kind == PassageKind::Ambiguous)
            context.uncertain.push_back(passage.result);
        if (passage.kind != PassageKind::Equal) continue;
        (side == SourceSide::Earlier ? context.earlier : context.known).push_back(passage.result);
        if (side == SourceSide::Stage3) context.right.push_back(passage.result);
    }
}

// A deletion scope has no text to place, so it fills only an empty region.
bool fills(const CorrespondenceMap &mapping, const ChoiceTarget &target, ByteRange region)
{
    if (target.source.range.begin == target.source.range.end) return region.begin == region.end;
    const auto *anchor = mapping.anchor(target.source.contributionId);
    return anchor && anchor->state == AnchorState::Mapped && anchor->result == region;
}

// Ignored text is removed only where it is still exactly what the source
// put there: the range to remove, or nothing.
std::optional<ByteRange> removable(const ResultAnchor *anchor, ByteRange region)
{
    if (!anchor || anchor->state != AnchorState::Mapped || !anchor->result || anchor->handEdited
        || anchor->manualOverlap || !contains(region, *anchor->result))
        return std::nullopt;
    return anchor->result;
}

// The section whose two source scopes fill the block's side regions.
std::optional<std::string> filledSection(const ChoiceCatalogue &choices, const CorrespondenceMap &mapping,
                                         const MarkerRange &block)
{
    const auto regions = sideRegions(block);
    for (const auto &left : choices.targets()) {
        if (left.scope != ChoiceScope::Section || left.source.side != SourceSide::Stage2
            || !fills(mapping, left, regions[0]))
            continue;
        const auto *right = choices.section(left.section, SourceSide::Stage3);
        if (right && fills(mapping, *right, regions[1])) return left.section;
    }
    return std::nullopt;
}

// The text before, between and after a side region's section scopes in text
// order: n + 1 ranges for n scopes. Git copies it into both side regions.
std::vector<ByteRange> sharedText(ByteRange region, const std::vector<ByteRange> &scopes)
{
    std::vector<ByteRange> result;
    result.reserve(scopes.size() + 1);
    auto begin = region.begin;
    for (const auto &scope : scopes) {
        result.push_back({begin, scope.begin});
        begin = scope.end;
    }
    result.push_back({begin, region.end});
    return result;
}

using PlacedScopes = std::array<std::vector<std::pair<ByteRange, std::string>>, 2>;

// Each side's mapped section scopes inside its region, in text order. A
// deletion's insertion point counts only strictly inside its region, where it
// lies between shared lines; at an edge it could belong outside the block.
PlacedScopes placedScopes(const ChoiceCatalogue &choices, const CorrespondenceMap &mapping, const MarkerRange &block)
{
    PlacedScopes placed;
    const auto regions = sideRegions(block);
    for (const auto &target : choices.targets()) {
        if (target.scope != ChoiceScope::Section || target.source.side == SourceSide::Earlier) continue;
        const auto *anchor = mapping.anchor(target.source.contributionId);
        if (!anchor || anchor->state != AnchorState::Mapped || !anchor->result) continue;
        const auto index = sideIndex(target.source.side);
        const auto region = regions[index];
        const auto range = *anchor->result;
        if (range.begin == range.end ? region.begin < range.begin && range.end < region.end : contains(region, range))
            placed[index].emplace_back(range, target.section);
    }
    for (auto &side : placed)
        // A deletion goes before a scope that starts where it is.
        std::sort(side.begin(), side.end(), [](const auto &a, const auto &b) {
            return a.first.begin != b.first.begin ? a.first.begin < b.first.begin : a.first.end < b.first.end;
        });
    return placed;
}

// The sections of a block Git wrote around lines both sides share: each side
// region holds one mapped scope of every section, in the same order on both
// sides, and the same text around them. A section with a scope outside its
// region leaves the block untied.
std::vector<std::string> enclosedSections(const ChoiceCatalogue &choices, const CorrespondenceMap &mapping,
                                          const MarkerRange &block, std::string_view text)
{
    const auto placed = placedScopes(choices, mapping, block);
    const auto &[left, right] = placed;
    if (left.empty() || left.size() != right.size()) return {};
    std::vector<std::string> sections;
    std::array<std::vector<ByteRange>, 2> scopes;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i].second != right[i].second) return {};
        if (i && (left[i].first.begin < left[i - 1].first.end || right[i].first.begin < right[i - 1].first.end))
            return {};
        sections.push_back(left[i].second);
        scopes[0].push_back(left[i].first);
        scopes[1].push_back(right[i].first);
    }
    const auto regions = sideRegions(block);
    const auto leftShared = sharedText(regions[0], scopes[0]);
    const auto rightShared = sharedText(regions[1], scopes[1]);
    for (std::size_t i = 0; i < leftShared.size(); ++i)
        if (slice(text, leftShared[i]) != slice(text, rightShared[i])) return {};
    return sections;
}

// What the decisions of one marker section's block allow. Deleting keeps the
// kept text in order only around one crossing section: the sections before
// it keep no right text and the sections after it keep no left text.
class BlockPlan {
public:
    BlockPlan(const ChoiceCatalogue &choices, const CorrespondenceMap &mapping, const Draft &draft,
              const std::map<std::string, Decision> &decided, const MarkerSection &found)
        : choices_(choices),
          mapping_(mapping),
          draft_(draft),
          decided_(decided),
          found_(found),
          regions_(sideRegions(found.block))
    {
        const auto &block = found.block;
        const auto base = block.base ? block.base->bytes.begin : block.separator.bytes.begin;
        removals_ = {
            {block.begin.bytes, {}, {}}, {{base, block.separator.bytes.end}, {}, {}}, {block.end.bytes, {}, {}}};
        for (const auto &section : found.sections) addSection(section);
        if (!placed_ && found.sections.size() > 1) complete_ = false;
    }

    [[nodiscard]] std::vector<MarkerEdit> edits() const
    {
        if (!touched_ || !complete_) return {};
        if (!placed_) return removals_;
        // MarkerPlanner::sections found the same shared text on both sides.
        const auto left = sharedText(regions_[0], scopes_[0]);
        const auto crossing = crossingSection();
        if (crossing == dropped_.size()) return rebuild(left);
        return removeAround(crossing, left, sharedText(regions_[1], scopes_[1]));
    }

private:
    void addSection(const std::string &section)
    {
        bool accepted = false;
        dropped_.emplace_back();
        cuts_.emplace_back();
        std::array<std::optional<std::uint64_t>, 2> order;
        for (const auto side : {SourceSide::Stage2, SourceSide::Stage3})
            if (const auto *target = choices_.section(section, side)) addSide(*target, order, accepted);
        complete_ = complete_ && accepted;
        rightFirst_.push_back(found_.sections.size() > 1 && order[0] && order[1] && *order[1] < *order[0]);
    }

    void addSide(const ChoiceTarget &target, std::array<std::optional<std::uint64_t>, 2> &order, bool &accepted)
    {
        const auto index = sideIndex(target.source.side);
        const auto region = regions_[index];
        const auto *anchor = mapping_.anchor(target.source.contributionId);
        // A deletion's insertion point lies outside the block.
        if (anchor && anchor->result && contains(region, *anchor->result))
            scopes_[index].push_back(*anchor->result);
        else
            placed_ = false;
        auto ignored = tally(target, order[index], accepted);
        const bool all = ignored.size() == target.decisions.size();
        dropped_.back()[index] = all;
        if (all) ignored = {target.source.contributionId};
        for (const auto &id : ignored) {
            if (all && region.begin == region.end) continue;
            const auto removed = removable(mapping_.anchor(id), region);
            if (!removed) {
                complete_ = false;
                continue;
            }
            removals_.push_back({*removed, id, {}});
            cuts_.back()[index].push_back(*removed);
        }
    }

    // The ignored decisions of a scope, noting undecided and accepted ones.
    std::vector<std::string> tally(const ChoiceTarget &target, std::optional<std::uint64_t> &order, bool &accepted)
    {
        std::vector<std::string> ignored;
        for (const auto &id : target.decisions) {
            touched_ = touched_ || decided_.contains(id);
            const auto value = draft_.decisions.find(id);
            if (value == draft_.decisions.end() || value->second.value == DecisionValue::Undecided) {
                complete_ = false;
            } else if (value->second.value == DecisionValue::Accepted) {
                accepted = true;
                if (value->second.order && (!order || *value->second.order < *order)) order = value->second.order;
            } else {
                ignored.push_back(id);
            }
        }
        return ignored;
    }

    [[nodiscard]] bool crossesAt(std::size_t crossing) const
    {
        if (rightFirst_[crossing]) return false;
        for (std::size_t i = 0; i < dropped_.size(); ++i)
            if ((i < crossing && !dropped_[i][1]) || (i > crossing && !dropped_[i][0])) return false;
        return true;
    }

    [[nodiscard]] std::size_t crossingSection() const
    {
        std::size_t crossing = 0;
        while (crossing < dropped_.size() && !crossesAt(crossing)) ++crossing;
        return crossing;
    }

    // Each section's kept text, left before right unless a combination put
    // right first, between the left copies of the shared lines.
    [[nodiscard]] std::vector<MarkerEdit> rebuild(const std::vector<ByteRange> &left) const
    {
        std::vector<KeptSpan> spans;
        for (std::size_t i = 0; i < found_.sections.size(); ++i) {
            spans.push_back({left[i], {}});
            for (const auto index :
                 rightFirst_[i] ? std::array<std::size_t, 2>{1, 0} : std::array<std::size_t, 2>{0, 1}) {
                auto kept = cuts_[i][index];
                std::sort(kept.begin(), kept.end(), [](const auto &a, const auto &b) { return a.begin < b.begin; });
                for (std::size_t at = 0; at < kept.size(); ++at)
                    if (!contains(scopes_[index][i], kept[at]) || (at && kept[at].begin < kept[at - 1].end)) return {};
                spans.push_back({scopes_[index][i], std::move(kept)});
            }
        }
        spans.push_back({left.back(), {}});
        const auto &block = found_.block;
        return {{{block.begin.bytes.begin, block.end.bytes.end}, {}, std::move(spans)}};
    }

    // The left copy of the shared lines stays up to the crossing section and
    // the right copy after it.
    [[nodiscard]] std::vector<MarkerEdit> removeAround(std::size_t crossing, const std::vector<ByteRange> &left,
                                                       const std::vector<ByteRange> &right) const
    {
        auto result = removals_;
        for (std::size_t i = 0; i < left.size() && i < right.size(); ++i) {
            const auto removed = i <= crossing ? right[i] : left[i];
            if (removed.begin != removed.end) result.push_back({removed, {}, {}});
        }
        return result;
    }

    const ChoiceCatalogue &choices_;
    const CorrespondenceMap &mapping_;
    const Draft &draft_;
    const std::map<std::string, Decision> &decided_;
    const MarkerSection &found_;
    std::array<ByteRange, 2> regions_;
    std::vector<MarkerEdit> removals_;
    bool touched_ = false;
    bool complete_ = true;
    bool placed_ = true;
    // Per section, whether every left and every right decision is ignored,
    // and the ignored text of each side.
    std::vector<std::array<bool, 2>> dropped_;
    std::vector<std::array<std::vector<ByteRange>, 2>> cuts_;
    std::array<std::vector<ByteRange>, 2> scopes_;
    // Per section of a split block, whether a combination ordered its right
    // text before its left text. Each scope still holds its own side.
    std::vector<bool> rightFirst_;
};
}

MarkerScan resultMarkers(std::string_view text, std::size_t markerSize, const CorrespondenceMap *mapping,
                         const LaunchFrames *launch)
{
    MarkerContext context;
    context.markerSize = markerSize;
    if (!mapping) return scanMarkers(text, std::move(context));
    const auto *earlier = mapping->source(SourceSide::Earlier);
    const auto *right = mapping->source(SourceSide::Stage3);
    if (earlier && right) context.sources = RegionSources{*earlier, *right, {}};
    if (context.sources && launch)
        for (const auto &frame : *launch)
            if (!frame.base.empty()) context.sources->bases.emplace_back(frame.base);
    for (const auto side : {SourceSide::Stage2, SourceSide::Stage3, SourceSide::Earlier})
        if (const auto *passages = mapping->passages(side)) addPassages(context, side, *passages);
    return scanMarkers(text, std::move(context));
}

LaunchFrames launchFrames(std::string_view text, const MarkerScan &scan)
{
    LaunchFrames frames;
    for (const auto &block : scan.blocks) frames.insert(frame(block, text));
    return frames;
}

void settleLaunchAnchors(CorrespondenceMap &mapping, std::string_view text, std::size_t markerSize)
{
    const auto scan = resultMarkers(text, markerSize, &mapping);
    if (scan.blocks.empty() || !scan.orphans.empty()) return;
    for (const auto side : {SourceSide::Stage2, SourceSide::Stage3}) {
        std::vector<ByteRange> dropped;
        dropped.reserve(scan.blocks.size() * 2);
        for (const auto &block : scan.blocks) {
            const auto kept = sideRegion(block, side);
            dropped.push_back({block.begin.bytes.begin, kept.begin});
            dropped.push_back({kept.end, block.end.bytes.end});
        }
        mapping.settle(side, dropped);
    }
}

std::vector<MarkerSection> MarkerPlanner::sections(std::string_view text, const CorrespondenceMap &mapping,
                                                   const MarkerScan *scan) const
{
    std::vector<MarkerSection> result;
    const auto scanned = scan ? MarkerScan{} : resultMarkers(text, markerSize_, &mapping, &launch_);
    for (const auto &block : (scan ? *scan : scanned).blocks) {
        if (!launch_.contains(frame(block, text))) continue;
        if (auto section = filledSection(choices_, mapping, block))
            result.push_back({block, {std::move(*section)}});
        else if (auto sections = enclosedSections(choices_, mapping, block, text); !sections.empty())
            result.push_back({block, std::move(sections)});
    }
    return result;
}

std::vector<MarkerEdit> MarkerPlanner::edits(const Draft &draft, const CorrespondenceMap &mapping,
                                             const std::map<std::string, Decision> &decided,
                                             const std::vector<MarkerSection> &sections) const
{
    std::vector<MarkerEdit> result;
    for (const auto &found : sections) {
        const auto edits = BlockPlan(choices_, mapping, draft, decided, found).edits();
        result.insert(result.end(), edits.begin(), edits.end());
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.range.begin > b.range.begin; });
    return result;
}

}
