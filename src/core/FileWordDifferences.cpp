// File word differences place word-level changes inside the changed passages of one file revision.
#include "FileWordDifferences.h"
#include "SessionState.h"

#include <algorithm>

namespace branch_truce {
namespace {
void append(std::vector<ByteRange> &into, const std::vector<ByteRange> &ranges, std::size_t offset)
{
    for (const auto &range : ranges) into.push_back({range.begin + offset, range.end + offset});
}

bool covered(const std::vector<ByteRange> &covers, ByteRange range)
{
    return std::any_of(covers.begin(), covers.end(), [&](const auto &cover) { return contains(cover, range); });
}

bool undecided(const std::map<std::string, Decision> &decisions, const ChoiceTarget &target)
{
    return std::any_of(target.decisions.begin(), target.decisions.end(), [&](const auto &id) {
        const auto found = decisions.find(id);
        return found == decisions.end() || found->second.value == DecisionValue::Undecided;
    });
}

std::optional<ByteRange> mappedResult(const CorrespondenceMap &map, const ChoiceTarget &target)
{
    const auto *anchor = map.anchor(target.source.contributionId);
    if (!anchor || anchor->state != AnchorState::Mapped || !anchor->result
        || !validByteRange(map.text(), *anchor->result))
        return std::nullopt;
    return anchor->result;
}

// Collects word ranges in text order while spending one shared budget.
class Collector {
public:
    explicit Collector(const WordDifferenceInputs &inputs) : inputs_(inputs), result_(inputs.mapping.text()) {}

    // While a section is undecided its two sides are compared with each other.
    void addCompetingSections()
    {
        const auto &left = inputs_.snapshots.stage2;
        const auto &right = inputs_.snapshots.stage3;
        if (!left || !right) return;
        for (const auto &ours : inputs_.choices.targets()) {
            if (ours.scope != ChoiceScope::Section || ours.source.side != SourceSide::Stage2) continue;
            const auto *theirs = inputs_.choices.section(ours.section, SourceSide::Stage3);
            if (!theirs || (!undecided(inputs_.decisions, ours) && !undecided(inputs_.decisions, *theirs))) continue;
            addCompetingPair(ours, *theirs, left->worktree.utf8, right->worktree.utf8);
        }
    }

    // Every other changed passage is compared with its result range.
    void addChangedPassages()
    {
        for (const auto side : {SourceSide::Stage2, SourceSide::Stage3, SourceSide::Earlier}) {
            const auto &source = inputs_.snapshots.source(side);
            const auto *passages = inputs_.mapping.passages(side);
            if (!source || !passages) continue;
            for (const auto &passage : *passages)
                if (passage.kind == PassageKind::Changed) addPassage(side, source->worktree.utf8, passage);
        }
    }

    [[nodiscard]] FileWordDifferences finish()
    {
        if (refused_) return {{}, {}, WordDifferenceState::ResourceLimit};
        for (auto &[side, ranges] : found_.sources) ranges = mergeRanges(std::move(ranges));
        found_.result = mergeRanges(std::move(found_.result));
        std::erase_if(found_.sources, [](const auto &entry) { return entry.second.empty(); });
        return std::move(found_);
    }

private:
    WordDifferences compare(std::string_view left, std::string_view right)
    {
        if (refused_ || left.size() > inputBytes_ || right.size() > inputBytes_ - left.size()) {
            refused_ = true;
            return {{}, {}, WordDifferenceState::ResourceLimit};
        }
        inputBytes_ -= left.size() + right.size();
        auto words = wordDifferences(left, right, budget_);
        refused_ = words.state != WordDifferenceState::Computed;
        return words;
    }
    void addCompetingPair(const ChoiceTarget &ours, const ChoiceTarget &theirs, std::string_view left,
                          std::string_view right)
    {
        const auto l = ours.source.range;
        const auto r = theirs.source.range;
        if (!validByteRange(left, l) || !validByteRange(right, r)) return;
        const auto words = compare(slice(left, l), slice(right, r));
        append(found_.sources[SourceSide::Stage2], words.left, l.begin);
        append(found_.sources[SourceSide::Stage3], words.right, r.begin);
        covered_[SourceSide::Stage2].push_back(l);
        covered_[SourceSide::Stage3].push_back(r);
        const auto mappedLeft = mappedResult(inputs_.mapping, ours);
        const auto mappedRight = mappedResult(inputs_.mapping, theirs);
        // An empty side, or one result range for both, leaves nothing to set
        // side by side; the changed passages cover the result instead.
        if (!mappedLeft || !mappedRight || mappedLeft->begin == mappedLeft->end
            || mappedRight->begin == mappedRight->end || overlaps(*mappedLeft, *mappedRight))
            return;
        const auto inResult = compare(slice(result_, *mappedLeft), slice(result_, *mappedRight));
        append(found_.result, inResult.left, mappedLeft->begin);
        append(found_.result, inResult.right, mappedRight->begin);
        resultCovered_.push_back(*mappedLeft);
        resultCovered_.push_back(*mappedRight);
    }

    void addPassage(SourceSide side, std::string_view text, const Passage &passage)
    {
        if (!validByteRange(text, passage.source) || !validByteRange(result_, passage.result)) return;
        const auto words = compare(slice(text, passage.source), slice(result_, passage.result));
        for (const auto &range : words.left) {
            const ByteRange at{range.begin + passage.source.begin, range.end + passage.source.begin};
            if (!covered(covered_[side], at)) found_.sources[side].push_back(at);
        }
        if (side == SourceSide::Earlier) return;
        for (const auto &range : words.right) {
            const ByteRange at{range.begin + passage.result.begin, range.end + passage.result.begin};
            if (!covered(resultCovered_, at)) found_.result.push_back(at);
        }
    }

    const WordDifferenceInputs &inputs_;
    std::string_view result_;
    std::size_t budget_ = kMaxWordDifferenceCells;
    std::size_t inputBytes_ = 8388608;
    bool refused_ = false;
    FileWordDifferences found_;
    std::map<SourceSide, std::vector<ByteRange>> covered_;
    std::vector<ByteRange> resultCovered_;
};
}

FileWordDifferences fileWordDifferences(const WordDifferenceInputs &inputs)
{
    Collector collector(inputs);
    collector.addCompetingSections();
    collector.addChangedPassages();
    return collector.finish();
}

FileWordDifferences fileWordDifferences(const FileSession &file)
{
    const auto *snapshots = file.snapshots();
    const auto *mapping = file.correspondence();
    if (!snapshots || !mapping || file.support() != Support::Supported) return {};
    return fileWordDifferences({*snapshots, *mapping, file.catalogue(), file.current().draft.decisions});
}

}
