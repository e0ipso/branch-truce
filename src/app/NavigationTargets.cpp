// Navigation targets translate conflict ranges into positions the result editor can reveal.
#include "NavigationTargets.h"
#include <QString>
#include <algorithm>
#include <limits>

namespace branch_truce {
int utf16Offset(const std::string &utf8, std::size_t bytes)
{
    if (bytes > utf8.size()) return -1;
    const auto prefix = QString::fromUtf8(utf8.data(), static_cast<qsizetype>(bytes));
    if (prefix.size() > std::numeric_limits<int>::max()) return -1;
    return static_cast<int>(prefix.size());
}

bool undecidedChoice(const FileSession &file, const ChoiceTarget &target)
{
    if (target.decisions.empty()) return false;
    for (const auto &id : target.decisions) {
        const auto found = file.current().draft.decisions.find(id);
        if (found == file.current().draft.decisions.end() || found->second.value != DecisionValue::Undecided)
            return false;
    }
    return true;
}

bool sectionHasLine(const FileSession &file, const ChoiceTarget &section)
{
    return std::any_of(file.choices().begin(), file.choices().end(), [&](const auto &target) {
        return target.scope == ChoiceScope::Line && target.section == section.section
            && target.source.side == section.source.side;
    });
}

std::vector<ScopeTarget> differenceTargets(const FileSession &file)
{
    const auto *mapping = file.correspondence();
    if (!mapping) return {};
    std::vector<ScopeTarget> targets;
    const auto &result = file.current().draft.text;
    for (const auto side : {SourceSide::Stage2, SourceSide::Stage3}) {
        const auto *passages = mapping->passages(side);
        if (!passages) continue;
        for (const auto &passage : *passages) {
            if (passage.kind == PassageKind::Equal) continue;
            if (passage.result.end < passage.result.begin || passage.result.end > result.size()) continue;
            if (utf16Offset(result, passage.result.begin) < 0 || utf16Offset(result, passage.result.end) < 0) continue;
            targets.push_back({passage.result.begin, passage.result.end, true, {}, side});
        }
    }
    std::sort(targets.begin(), targets.end(), [](const auto &left, const auto &right) {
        if (left.byteBegin != right.byteBegin) return left.byteBegin < right.byteBegin;
        return left.byteEnd < right.byteEnd;
    });
    targets.erase(std::unique(targets.begin(), targets.end(), [](const auto &left, const auto &right) {
        return left.byteBegin == right.byteBegin && left.byteEnd == right.byteEnd;
    }), targets.end());
    return targets;
}

std::vector<ScopeTarget> conflictTargets(const FileSession &file)
{
    std::vector<ScopeTarget> targets;
    for (const auto &region : file.conflictRegions())
        targets.push_back({region.begin, region.end, true, {}, SourceSide::Stage2});
    return targets;
}

std::vector<ScopeTarget> undecidedTargets(const FileSession &file)
{
    const auto *mapping = file.correspondence();
    if (!mapping) return {};
    std::vector<ScopeTarget> targets;
    const auto &result = file.current().draft.text;
    for (const auto &target : file.choices()) {
        if (!undecidedChoice(file, target)) continue;
        if (target.scope == ChoiceScope::Section && sectionHasLine(file, target)) continue;
        const auto *anchor = mapping->anchor(target.source.contributionId);
        const bool mapped = anchor && anchor->state == AnchorState::Mapped && anchor->result
            && anchor->result->end >= anchor->result->begin && anchor->result->end <= result.size()
            && utf16Offset(result, anchor->result->begin) >= 0 && utf16Offset(result, anchor->result->end) >= 0;
        if (mapped) {
            targets.push_back({anchor->result->begin, anchor->result->end, true,
                target.source.contributionId, target.source.side});
        } else {
            targets.push_back({target.source.range.begin, target.source.range.end, false,
                target.source.contributionId, target.source.side});
        }
    }
    std::sort(targets.begin(), targets.end(), [](const auto &left, const auto &right) {
        if (left.resultRange != right.resultRange) return left.resultRange && !right.resultRange;
        if (left.byteBegin != right.byteBegin) return left.byteBegin < right.byteBegin;
        if (left.byteEnd != right.byteEnd) return left.byteEnd < right.byteEnd;
        return left.contributionId < right.contributionId;
    });
    return targets;
}
}
