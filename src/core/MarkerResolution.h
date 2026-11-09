// Marker resolution ties conflict-marker blocks to choice sections and plans the edits that remove them.
#pragma once

#include "ChoiceCatalogue.h"
#include "ConflictMarkers.h"
#include "Correspondence.h"
#include "Draft.h"
#include <algorithm>
#include <compare>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace branch_truce {

// Everything in a block except its two side regions. A hand edit to any of it
// leaves a frame that no launch block has.
struct BlockFrame {
    std::string begin;
    // The base region including its ||||||| line, empty without one.
    std::string base;
    std::string separator;
    std::string end;
    auto operator<=>(const BlockFrame &) const = default;
};
using LaunchFrames = std::set<BlockFrame>;

// A complete conflict-marker block and the choice sections it encloses, in
// text order. Either the two source scopes of one section fill its side
// regions, or each side region holds one scope of every section, in the same
// order, around lines both sides share.
struct MarkerSection {
    MarkerRange block;
    std::vector<std::string> sections;
    [[nodiscard]] bool encloses(const std::string &section) const
    {
        return std::find(sections.begin(), sections.end(), section) != sections.end();
    }
    bool operator==(const MarkerSection &) const = default;
};

// Text a choice deletes to resolve a marker section, leaving `anchor` empty,
// or a whole block it rebuilds from the text its sections keep.
struct MarkerEdit {
    ByteRange range;
    std::string anchor;
    std::optional<std::vector<KeptSpan>> rebuilt;
};

// Conflict-marker blocks and orphan marker lines in a result. Git copies both
// sides' text into the result and adds only the marker lines, so a line the
// mapping shows equal to a captured side is content. The earlier version
// reaches the result only in diff3 base regions, so its passages count only
// there. Where it aligns ambiguously, the right side's passages, the earlier
// and right texts Git copied around its separator, and the launch blocks'
// base regions show which separator line Git wrote. Without a mapping the
// plain scan applies.
[[nodiscard]] MarkerScan resultMarkers(std::string_view text, std::size_t markerSize, const CorrespondenceMap *mapping,
                                       const LaunchFrames *launch = nullptr);

// The frames of a scan's complete blocks.
[[nodiscard]] LaunchFrames launchFrames(std::string_view text, const MarkerScan &scan);

// Git writes each side's text once per block, so the launch result without
// the marker lines and the other regions holds one side's text. A side whose
// identical lines leave anchors ambiguous in the whole result can still map
// them there. Only a scan that explains every marker line shows that text.
void settleLaunchAnchors(CorrespondenceMap &mapping, std::string_view text, std::size_t markerSize);

// Ties the blocks of one file's results to its choice sections and plans the
// edits that resolve them. It holds references, so it lives only as long as a
// single command.
class MarkerPlanner {
public:
    MarkerPlanner(const ChoiceCatalogue &choices, const LaunchFrames &launch, std::size_t markerSize) noexcept
        : choices_(choices), launch_(launch), markerSize_(markerSize)
    {}

    // The marker sections of `text`. A block whose marker lines or base text
    // no launch block has is orphaned. `scan`, when given, is the scan of `text`.
    [[nodiscard]] std::vector<MarkerSection> sections(std::string_view text, const CorrespondenceMap &mapping,
                                                      const MarkerScan *scan = nullptr) const;

    // For each marker section the decided contributions touch, once all of
    // its contributions are decided and each of its sections has one
    // accepted: the marker lines, base text and ignored text to delete, each
    // with the anchor that keeps its empty range, last range first. Text both
    // sides share around several sections stays once. Deleting keeps the left
    // text before the right text, so a block whose kept text it cannot put in
    // section order, or whose section a combination ordered right first, is
    // rebuilt from that text and one copy of the shared lines instead. A
    // section whose ignored text holds other work keeps all.
    [[nodiscard]] std::vector<MarkerEdit> edits(const Draft &draft, const CorrespondenceMap &mapping,
                                                const std::map<std::string, Decision> &decided,
                                                const MarkerScan *scan = nullptr) const;

private:
    const ChoiceCatalogue &choices_;
    const LaunchFrames &launch_;
    std::size_t markerSize_;
};

}
