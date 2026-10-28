#include "SessionState.h"
#include "EditingCommands.h"
#include "ResourceLimits.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace branch_truce {
namespace {
bool succeeded(const std::optional<OperationOutcome> &outcome, RevisionId revision)
{
    return outcome && outcome->revision == revision && outcome->outcome == Outcome::Succeeded;
}

std::size_t undecidedCount(const Draft &draft)
{
    return static_cast<std::size_t>(std::count_if(
        draft.decisions.begin(), draft.decisions.end(),
        [](const auto &entry) { return entry.second.value == DecisionValue::Undecided; }));
}

bool exceedsEnvelope(const std::string &text)
{
    return !withinTextEnvelope(text);
}

// Result passages equal to a captured side. Git copies both sides' text into
// the result and adds only the marker lines, so marker-like lines here are
// content. The earlier version reaches the result only in diff3 base regions,
// and a match against it elsewhere could take Git's separator for content, so
// its passages count only inside a base region. Where the earlier version
// aligns ambiguously, a base region can still end with a separator-like line;
// the right side's passages, or the earlier and right side texts Git copied
// around its separator, show which separator line Git wrote. Choices change
// side regions but not base regions, so after launch the launch blocks' base
// regions show it too. Files without a correspondence keep the plain scan.
MarkerScan resultMarkers(std::string_view text, const CorrespondenceMap *mapping,
                         const std::set<std::vector<std::string>> *launch = nullptr)
{
    std::vector<ByteRange> content;
    std::vector<ByteRange> earlier;
    std::vector<ByteRange> uncertain;
    std::vector<ByteRange> right;
    std::optional<RegionSources> sources;
    if (mapping) {
        const auto *earlierText = mapping->source(SourceSide::Earlier);
        const auto *rightText = mapping->source(SourceSide::Stage3);
        if (earlierText && rightText) sources = RegionSources{*earlierText, *rightText, {}};
        if (sources && launch)
            for (const auto &frame : *launch)
                if (!frame[1].empty()) sources->bases.push_back(frame[1]);
        for (const auto side : {SourceSide::Stage2, SourceSide::Stage3, SourceSide::Earlier}) {
            const auto *passages = mapping->passages(side);
            if (!passages) continue;
            for (const auto &passage : *passages) {
                if (side == SourceSide::Earlier && passage.kind == PassageKind::Ambiguous)
                    uncertain.push_back(passage.result);
                if (passage.kind != PassageKind::Equal) continue;
                (side == SourceSide::Earlier ? earlier : content).push_back(passage.result);
                if (side == SourceSide::Stage3) right.push_back(passage.result);
            }
        }
    }
    return scanMarkers(text, std::move(content), std::move(earlier), std::move(uncertain), std::move(right), sources);
}

Draft withMarkers(Draft draft, const CorrespondenceMap *mapping,
                  const std::set<std::vector<std::string>> *launch)
{
    draft.unresolvedMarkers = resultMarkers(draft.text, mapping, launch).count();
    return draft;
}

std::string slice(std::string_view text, ByteRange range)
{
    return std::string(text.substr(range.begin, range.end - range.begin));
}

// Everything in a block except its two side regions. A hand edit to any of it
// leaves a frame that no launch block has.
std::vector<std::string> frame(const MarkerRange &block, std::string_view text)
{
    const auto base = block.base ? block.base->bytes : ByteRange{block.separator.bytes.begin, block.separator.bytes.begin};
    return {slice(text, block.begin.bytes), slice(text, {base.begin, block.separator.bytes.begin}),
            slice(text, block.separator.bytes), slice(text, block.end.bytes)};
}

ByteRange sideRegion(const MarkerRange &block, SourceSide side)
{
    if (side == SourceSide::Stage3) return {block.separator.bytes.end, block.end.bytes.begin};
    return {block.begin.bytes.end, block.base ? block.base->bytes.begin : block.separator.bytes.begin};
}

// Git writes each side's text once per block, so the launch result without
// the marker lines and the other regions holds one side's text. A side whose
// identical lines leave anchors ambiguous in the whole result can still map
// them there. Only a scan that explains every marker line shows that text.
void settle(CorrespondenceMap &mapping, std::string_view text)
{
    const auto scan = resultMarkers(text, &mapping);
    if (scan.blocks.empty() || !scan.orphans.empty()) return;
    for (const auto side : {SourceSide::Stage2, SourceSide::Stage3}) {
        std::vector<ByteRange> dropped;
        for (const auto &block : scan.blocks) {
            const auto kept = sideRegion(block, side);
            dropped.push_back({block.begin.bytes.begin, kept.begin});
            dropped.push_back({kept.end, block.end.bytes.end});
        }
        mapping.settle(side, dropped);
    }
}

// A deletion scope has no text to place, so it fills only an empty region.
bool fills(const CorrespondenceMap &mapping, const ChoiceTarget &target, ByteRange region)
{
    if (target.source.range.begin == target.source.range.end) return region.begin == region.end;
    const auto *anchor = mapping.anchor(target.source.contributionId);
    return anchor && anchor->state == AnchorState::Mapped && anchor->result == region;
}

bool inside(ByteRange range, ByteRange region)
{
    return region.begin <= range.begin && range.end <= region.end;
}

// Ignored text is removed only where it is still exactly what the source put there.
bool removable(const ResultAnchor *anchor, ByteRange region)
{
    return anchor && anchor->state == AnchorState::Mapped && anchor->result && !anchor->handEdited
        && !anchor->manualOverlap && inside(*anchor->result, region);
}

// The section whose two source scopes fill the block's side regions.
std::optional<std::string> filledSection(const std::vector<ChoiceTarget> &choices,
                                         const CorrespondenceMap &mapping, const MarkerRange &block)
{
    for (const auto &left : choices) {
        if (left.scope != ChoiceScope::Section || left.source.side != SourceSide::Stage2
            || !fills(mapping, left, sideRegion(block, SourceSide::Stage2))) continue;
        const auto right = std::find_if(choices.begin(), choices.end(), [&](const auto &target) {
            return target.scope == ChoiceScope::Section && target.section == left.section
                && target.source.side == SourceSide::Stage3;
        });
        if (right != choices.end() && fills(mapping, *right, sideRegion(block, SourceSide::Stage3))) return left.section;
    }
    return std::nullopt;
}

// The text before, between and after a side region's section scopes in text
// order: n + 1 ranges for n scopes. Git copies it into both side regions.
std::vector<ByteRange> sharedText(ByteRange region, const std::vector<ByteRange> &scopes)
{
    std::vector<ByteRange> result;
    auto begin = region.begin;
    for (const auto &scope : scopes) {
        result.push_back({begin, scope.begin});
        begin = scope.end;
    }
    result.push_back({begin, region.end});
    return result;
}

// The sections of a block Git wrote around lines both sides share: each side
// region holds one mapped scope of every section, in the same order on both
// sides, and the same text around them. A deletion's insertion point counts
// only strictly inside its region, where it lies between shared lines; at an
// edge it could belong outside the block. A section with a scope outside its
// region leaves the block untied.
std::vector<std::string> enclosedSections(const std::vector<ChoiceTarget> &choices, const CorrespondenceMap &mapping,
                                          const MarkerRange &block, std::string_view text)
{
    std::array<std::vector<std::pair<ByteRange, std::string>>, 2> placed;
    for (const auto &target : choices) {
        if (target.scope != ChoiceScope::Section || target.source.side == SourceSide::Earlier) continue;
        const auto *anchor = mapping.anchor(target.source.contributionId);
        if (!anchor || anchor->state != AnchorState::Mapped || !anchor->result) continue;
        const auto index = target.source.side == SourceSide::Stage2 ? 0u : 1u;
        const auto region = sideRegion(block, target.source.side);
        const auto range = *anchor->result;
        if (range.begin == range.end ? region.begin < range.begin && range.end < region.end : inside(range, region))
            placed[index].push_back({range, target.section});
    }
    auto &[left, right] = placed;
    if (left.empty() || left.size() != right.size()) return {};
    for (auto &side : placed)
        // A deletion goes before a scope that starts where it is.
        std::sort(side.begin(), side.end(), [](const auto &a, const auto &b) {
            return a.first.begin != b.first.begin ? a.first.begin < b.first.begin : a.first.end < b.first.end;
        });
    std::vector<std::string> sections;
    std::array<std::vector<ByteRange>, 2> scopes;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i].second != right[i].second) return {};
        if (i && (left[i].first.begin < left[i - 1].first.end || right[i].first.begin < right[i - 1].first.end)) return {};
        sections.push_back(left[i].second);
        scopes[0].push_back(left[i].first);
        scopes[1].push_back(right[i].first);
    }
    const auto leftShared = sharedText(sideRegion(block, SourceSide::Stage2), scopes[0]);
    const auto rightShared = sharedText(sideRegion(block, SourceSide::Stage3), scopes[1]);
    for (std::size_t i = 0; i < leftShared.size(); ++i)
        if (slice(text, leftShared[i]) != slice(text, rightShared[i])) return {};
    return sections;
}
}

FileSession::FileSession(std::string id, std::string path, Draft initial,
                         Support support, std::string supportDetail,
                         std::optional<FileSnapshots> snapshots, bool initializeChoices)
    : id_(std::move(id)), path_(std::move(path)), support_(support),
      supportDetail_(std::move(supportDetail))
{
    if (id_.empty() || path_.empty()) {
        throw std::invalid_argument("A file session needs an identity and path");
    }
    if (snapshots) {
        if (snapshots->path != path_ || snapshots->launchResult.text.utf8 != initial.text) {
            throw std::invalid_argument("Captured sources and launch result must belong to this file session");
        }
        snapshots_ = std::make_shared<const FileSnapshots>(std::move(*snapshots));
    }
    std::shared_ptr<const CorrespondenceMap> correspondence;
    const auto resultBudget = assessText(initial.text);
    if (resultBudget.limit != TextLimit::Within) {
        support_ = Support::Unreadable;
        supportDetail_ = "Load error: " + textLimitDiagnostic(resultBudget);
    } else if (snapshots_) {
        try {
            correspondence = std::make_shared<const CorrespondenceMap>(1, *snapshots_, initial.text);
            choices_ = std::make_shared<const std::vector<ChoiceTarget>>(choiceTargets(*snapshots_));
            auto bound = std::make_shared<CorrespondenceMap>(*correspondence);
            for (const auto &target : *choices_) {
                if (!bound->bind(target.source)) throw std::invalid_argument("Invalid choice scope");
                // Empty deletions have a section control and one decision,
                // without pretending that deleting several lines is a line pick.
                if (target.source.range.begin == target.source.range.end)
                    for (const auto &id : target.decisions) {
                        auto source = target.source;
                        source.contributionId = id;
                        if (!bound->bind(std::move(source))) throw std::invalid_argument("Invalid deletion scope");
                    }
                if (initializeChoices)
                    for (const auto &id : target.decisions) initial.decisions.try_emplace(id);
            }
            nonConflictingTargets_ = std::make_shared<const std::vector<NonConflictingTarget>>(
                branch_truce::nonConflictingTargets(*snapshots_, *choices_));
            for (const auto &target : *nonConflictingTargets_) {
                if (!bound->bind(target.earlier) || !bound->bind(target.source))
                    throw std::invalid_argument("Invalid non-conflicting scope");
            }
            settle(*bound, initial.text);
            correspondence = std::move(bound);
        } catch (const std::exception &error) {
            support_ = Support::Unreadable;
            supportDetail_ = "Load error: could not build correspondence for " + path_ + ": " + error.what();
        }
    }
    if (snapshots_) {
        std::set<std::vector<std::string>> launch;
        for (const auto &block : resultMarkers(initial.text, correspondence.get()).blocks)
            launch.insert(frame(block, initial.text));
        launchBlocks_ = std::make_shared<const std::set<std::vector<std::string>>>(std::move(launch));
    }
    auto draft = withMarkers(std::move(initial), correspondence.get(), launchBlocks_.get());
    revisions_.emplace(1, Revision{1, 1, 1, std::move(draft), std::move(correspondence)});
}

const std::vector<ChoiceTarget> &FileSession::choices() const noexcept
{
    static const std::vector<ChoiceTarget> empty;
    return choices_ ? *choices_ : empty;
}

const std::vector<NonConflictingTarget> &FileSession::nonConflictingTargets() const noexcept
{
    static const std::vector<NonConflictingTarget> empty;
    return nonConflictingTargets_ ? *nonConflictingTargets_ : empty;
}

MarkerScan FileSession::markers() const
{
    return resultMarkers(current().draft.text, correspondence(), launchBlocks_.get());
}

std::vector<MarkerSection> FileSession::markerSections() const
{
    const auto *mapping = correspondence();
    return mapping ? markerSections(current().draft.text, *mapping) : std::vector<MarkerSection>{};
}

std::vector<MarkerSection> FileSession::markerSections(const std::string &text,
                                                       const CorrespondenceMap &mapping) const
{
    std::vector<MarkerSection> result;
    if (!launchBlocks_ || !choices_) return result;
    for (const auto &block : resultMarkers(text, &mapping, launchBlocks_.get()).blocks) {
        if (!launchBlocks_->contains(frame(block, text))) continue;
        if (auto section = filledSection(*choices_, mapping, block)) result.push_back({block, {std::move(*section)}});
        else if (auto sections = enclosedSections(*choices_, mapping, block, text); !sections.empty())
            result.push_back({block, std::move(sections)});
    }
    return result;
}

// For each marker section the decided contributions touch, once all of its
// contributions are decided and each of its sections has one accepted: the
// marker lines, base text and ignored text to delete, each with the anchor
// that keeps its empty range, last range first. Text both sides share around
// several sections stays once. Deleting keeps the left text before the right
// text, so a block whose kept text it cannot put in section order, or whose
// section a combination ordered right first, is rebuilt from that text and one
// copy of the shared lines instead. A section whose ignored text holds other
// work keeps all.
std::vector<FileSession::MarkerEdit> FileSession::markerEdits(
    const Draft &draft, const CorrespondenceMap &mapping,
    const std::map<std::string, Decision> &decided) const
{
    std::vector<MarkerEdit> result;
    for (const auto &found : markerSections(draft.text, mapping)) {
        const auto &block = found.block;
        const std::array regions{sideRegion(block, SourceSide::Stage2), sideRegion(block, SourceSide::Stage3)};
        std::vector<MarkerEdit> removals{{block.begin.bytes, {}, {}},
            {{block.base ? block.base->bytes.begin : block.separator.bytes.begin, block.separator.bytes.end}, {}, {}},
            {block.end.bytes, {}, {}}};
        bool touched = false, complete = true, placed = true;
        // Per section, whether every left and every right decision is ignored,
        // and the ignored text of each side.
        std::vector<std::array<bool, 2>> dropped;
        std::vector<std::array<std::vector<ByteRange>, 2>> cuts;
        std::array<std::vector<ByteRange>, 2> scopes;
        // Per section of a split block, whether a combination ordered its
        // right text before its left text. Each scope still holds its own side.
        std::vector<bool> rightFirst;
        for (const auto &section : found.sections) {
            bool accepted = false;
            auto &sides = dropped.emplace_back();
            auto &cut = cuts.emplace_back();
            std::array<std::optional<std::uint64_t>, 2> order;
            for (const auto &side : *choices_) {
                if (side.scope != ChoiceScope::Section || side.section != section) continue;
                const auto index = side.source.side == SourceSide::Stage2 ? 0u : 1u;
                const auto region = regions[index];
                const auto *anchor = mapping.anchor(side.source.contributionId);
                // A deletion's insertion point lies outside the block.
                if (anchor && anchor->result && inside(*anchor->result, region)) scopes[index].push_back(*anchor->result);
                else placed = false;
                std::vector<std::string> ignored;
                for (const auto &id : side.decisions) {
                    touched = touched || decided.contains(id);
                    const auto value = draft.decisions.find(id);
                    if (value == draft.decisions.end() || value->second.value == DecisionValue::Undecided) complete = false;
                    else if (value->second.value == DecisionValue::Accepted) {
                        accepted = true;
                        if (value->second.order && (!order[index] || *value->second.order < *order[index]))
                            order[index] = value->second.order;
                    } else ignored.push_back(id);
                }
                sides[index] = ignored.size() == side.decisions.size();
                if (sides[index]) ignored = {side.source.contributionId};
                for (const auto &id : ignored) {
                    const auto *removed = mapping.anchor(id);
                    if (sides[index] && region.begin == region.end) continue;
                    if (!removable(removed, region)) complete = false;
                    else {
                        removals.push_back({*removed->result, id, {}});
                        cut[index].push_back(*removed->result);
                    }
                }
            }
            complete = complete && accepted;
            rightFirst.push_back(found.sections.size() > 1 && order[0] && order[1] && *order[1] < *order[0]);
        }
        if (!placed && found.sections.size() > 1) complete = false;
        if (!touched || !complete) continue;
        // Deleting keeps the kept text in order only around one crossing
        // section: the sections before it keep no right text and the sections
        // after it keep no left text. It keeps left text before right text.
        const auto crossesAt = [&](std::size_t crossing) {
            if (rightFirst[crossing]) return false;
            for (std::size_t i = 0; i < dropped.size(); ++i)
                if ((i < crossing && !dropped[i][1]) || (i > crossing && !dropped[i][0])) return false;
            return true;
        };
        std::size_t crossing = 0;
        while (crossing < dropped.size() && !crossesAt(crossing)) ++crossing;
        if (!placed) {
            result.insert(result.end(), removals.begin(), removals.end());
            continue;
        }
        // markerSections found the same shared text on both sides.
        const auto left = sharedText(regions[0], scopes[0]);
        const auto right = sharedText(regions[1], scopes[1]);
        if (crossing == dropped.size()) {
            // Each section's kept text, left before right unless a combination
            // put right first, between the left copies of the shared lines.
            std::vector<KeptSpan> spans;
            for (std::size_t i = 0; i < found.sections.size(); ++i) {
                spans.push_back({left[i], {}});
                for (const auto index : rightFirst[i] ? std::array{1u, 0u} : std::array{0u, 1u}) {
                    auto kept = cuts[i][index];
                    std::sort(kept.begin(), kept.end(), [](const auto &a, const auto &b) { return a.begin < b.begin; });
                    for (std::size_t at = 0; at < kept.size(); ++at)
                        if (!inside(kept[at], scopes[index][i]) || (at && kept[at].begin < kept[at - 1].end)) complete = false;
                    spans.push_back({scopes[index][i], std::move(kept)});
                }
            }
            spans.push_back({left.back(), {}});
            if (complete) result.push_back({{block.begin.bytes.begin, block.end.bytes.end}, {}, std::move(spans)});
            continue;
        }
        // The left copy stays up to the crossing section and the right copy after it.
        for (std::size_t i = 0; i < left.size() && i < right.size(); ++i) {
            const auto removed = i <= crossing ? right[i] : left[i];
            if (removed.begin != removed.end) removals.push_back({removed, {}, {}});
        }
        result.insert(result.end(), removals.begin(), removals.end());
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.range.begin > b.range.begin; });
    return result;
}

bool FileSession::completesMarkers(const EditingCommand &command) const
{
    const auto *mapping = correspondence();
    if (!mapping || command.scope != EditingScope::Mapped) return false;
    auto draft = current().draft;
    for (const auto &[id, value] : command.decisions) draft.decisions[id] = value;
    return !markerEdits(draft, *mapping, command.decisions).empty();
}

RevisionId FileSession::advance(const Draft &edited,
                                std::shared_ptr<const CorrespondenceMap> correspondence)
{
    if (next_ == std::numeric_limits<RevisionId>::max()) {
        throw std::overflow_error("Session revision space exhausted");
    }
    const auto &previous = current();
    const auto id = next_;
    if (!correspondence && previous.correspondence) {
        correspondence = std::make_shared<const CorrespondenceMap>(
            previous.correspondence->recomputed(id, edited.text));
    }
    // Counted against this revision's own passages, so replay derives the same count.
    const auto draft = withMarkers(edited, correspondence.get(), launchBlocks_.get());
    const auto result = draft.text == previous.draft.text ? previous.result : id;
    const auto decisions = draft.decisions == previous.draft.decisions
            && draft.unresolvedMarkers == previous.draft.unresolvedMarkers
        ? previous.decisions : id;
    revisions_.emplace(id, Revision{id, result, decisions, draft, std::move(correspondence)});
    ++next_;
    current_ = id;
    return id;
}

RevisionId FileSession::restore(const Revision &revision)
{
    auto correspondence = revision.correspondence
        ? std::make_shared<const CorrespondenceMap>(revision.correspondence->atRevision(next_)) : nullptr;
    const auto id = advance(revision.draft, std::move(correspondence));
    // Restored content can still be the manually saved content. The transaction
    // identity is fresh, so restoring it never restores review.
    auto &restored = revisions_.at(id);
    restored.result = revision.result;
    restored.decisions = revision.decisions;
    return id;
}

void FileSession::appendHistory(RevisionId revision)
{
    history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(cursor_ + 1), history_.end());
    history_.push_back(revision);
    cursor_ = history_.size() - 1;
}

Transition FileSession::replaceDraft(RevisionId expected, Draft draft,
                                     const std::optional<std::set<std::string>> &handEdited,
                                     std::vector<ByteRange> handEditedRanges)
{
    if (expected != current_) return Transition::StaleRevision;
    if (support_ != Support::Supported) return Transition::Blocked;
    if (exceedsEnvelope(draft.text)) return Transition::Blocked;
    if (!handEdited && !handEditedRanges.empty()) return Transition::InvalidAnchor;
    // The marker count follows from the text, so a supplied count changes nothing.
    const bool unchanged = draft.text == current().draft.text && draft.decisions == current().draft.decisions;
    std::shared_ptr<const CorrespondenceMap> mapping;
    if (handEdited && correspondence()) {
        auto replayed = correspondence()->recomputed(next_, draft.text);
        if (!replayed.restoreHandEdits(*handEdited) || !replayed.restoreHandEditedRanges(std::move(handEditedRanges)))
            return Transition::InvalidAnchor;
        // A recorded choice can change only the hand-edit flags and ranges.
        if (unchanged && replayed.handEditedAnchors() == correspondence()->handEditedAnchors()
            && replayed.handEditedRanges() == correspondence()->handEditedRanges())
            return Transition::NoChange;
        mapping = std::make_shared<const CorrespondenceMap>(std::move(replayed));
    } else if (unchanged) return Transition::NoChange;
    else if (handEdited && (!handEdited->empty() || !handEditedRanges.empty())) return Transition::InvalidAnchor;
    appendHistory(advance(draft, std::move(mapping)));
    return Transition::Applied;
}

Transition FileSession::bindSourceAnchor(RevisionId expected, SourceAnchor source)
{
    if (expected != current_) return Transition::StaleRevision;
    if (support_ != Support::Supported) return Transition::Blocked;
    const auto *mapping = correspondence();
    if (!mapping) return Transition::InvalidAnchor;
    if (const auto *existing = mapping->anchor(source.contributionId))
        return existing->source == source ? Transition::NoChange : Transition::InvalidAnchor;
    auto updated = std::make_shared<CorrespondenceMap>(*mapping);
    if (!updated->bind(std::move(source))) return Transition::InvalidAnchor;
    revisions_.at(current_).correspondence = std::move(updated);
    return Transition::Applied;
}

Transition FileSession::editResult(RevisionId expected, ResultEdit edit)
{
    if (expected != current_) return Transition::StaleRevision;
    if (support_ != Support::Supported) return Transition::Blocked;
    if (!validUtf8(current().draft.text) || !validByteRange(current().draft.text, edit.range)
        || !validUtf8(edit.replacement))
        return Transition::InvalidAnchor;
    auto draft = current().draft;
    draft.text.replace(edit.range.begin, edit.range.end - edit.range.begin, edit.replacement);
    if (exceedsEnvelope(draft.text)) return Transition::Blocked;
    if (draft == current().draft) return Transition::NoChange;
    auto mapping = correspondence()
        ? std::make_shared<const CorrespondenceMap>(correspondence()->edited(next_, edit)) : nullptr;
    appendHistory(advance(draft, std::move(mapping)));
    return Transition::Applied;
}

Transition FileSession::applyCommand(const EditingCommand &command, bool confirmReplacement)
{
    if (command.expected != current_) return Transition::StaleRevision;
    if (support_ != Support::Supported) return Transition::Blocked;
    const auto *mapping = correspondence();
    if (!mapping) return Transition::InvalidAnchor;
    const bool wholeFile = command.scope == EditingScope::WholeFile;
    if (wholeFile) {
        if (!command.targets.empty() || command.edits.size() != 1
            || command.edits.front().range != ByteRange{0, current().draft.text.size()})
            return Transition::InvalidAnchor;
        if (!confirmReplacement) return Transition::ConfirmationRequired;
    } else if (command.targets.empty()) return Transition::InvalidAnchor;
    for (const auto &target : command.targets) {
        if (target.revision != current_) return Transition::StaleRevision;
        const auto *bound = mapping->anchor(target.source.contributionId);
        if (!bound || *bound != target) return Transition::InvalidAnchor;
        if (target.state == AnchorState::Ambiguous) return Transition::AmbiguousTarget;
        if (target.state != AnchorState::Mapped || !target.result) return Transition::InvalidAnchor;
    }
    auto edits = command.edits;
    std::sort(edits.begin(), edits.end(), [](const auto &a, const auto &b) { return a.range.begin > b.range.begin; });
    for (std::size_t i = 0; i < edits.size(); ++i) {
        const auto &edit = edits[i];
        if (!validByteRange(current().draft.text, edit.range) || !validUtf8(edit.replacement)) return Transition::InvalidAnchor;
        if (i && (edit.range.end > edits[i - 1].range.begin || edit.range.begin == edits[i - 1].range.begin))
            return Transition::AmbiguousTarget;
        if (wholeFile) continue;
        const auto target = std::find_if(command.targets.begin(), command.targets.end(),
            [&](const auto &anchor) { return anchor.result == edit.range; });
        if (target == command.targets.end()) return Transition::InvalidAnchor;
        if (!confirmReplacement && std::any_of(command.targets.begin(), command.targets.end(), [&](const auto &anchor) {
            return anchor.result == edit.range && anchor.manualOverlap;
        })) return Transition::ConfirmationRequired;
    }
    auto draft = current().draft;
    for (const auto &[id, value] : command.decisions) {
        if (!mapping->anchor(id)) return Transition::InvalidAnchor;
        draft.decisions[id] = value;
    }
    auto updated = std::make_shared<CorrespondenceMap>(*mapping);
    for (const auto &edit : edits) {
        draft.text.replace(edit.range.begin, edit.range.end - edit.range.begin, edit.replacement);
        if (exceedsEnvelope(draft.text)) return Transition::Blocked;
        if (wholeFile) {
            *updated = updated->recomputed(next_, draft.text, false);
            continue;
        }
        const auto target = std::find_if(command.targets.begin(), command.targets.end(),
            [&](const auto &anchor) { return anchor.result == edit.range; });
        *updated = updated->edited(next_, edit, false, target->source.contributionId);
    }
    // Deciding a marker section's last contribution resolves its block in
    // this same history entry, so one undo brings the markers back.
    if (!wholeFile) {
        for (const auto &edit : markerEdits(draft, *updated, command.decisions)) {
            *updated = edit.rebuilt ? updated->rebuilt(next_, edit.range, *edit.rebuilt)
                : updated->edited(next_, {edit.range, {}}, false, edit.anchor);
            draft.text = updated->text();
        }
    }
    // Choosing the text a hand edit left unchanged still replaces that edit.
    if (draft == current().draft && updated->handEditedAnchors() == mapping->handEditedAnchors()
        && updated->handEditedRanges() == mapping->handEditedRanges())
        return Transition::NoChange;
    appendHistory(advance(draft, std::make_shared<const CorrespondenceMap>(updated->atRevision(next_))));
    return Transition::Applied;
}

Transition FileSession::replaceMapped(RevisionId expected, const ResultAnchor &anchor,
                                      std::string replacement, std::optional<Decision> decision,
                                      bool confirmManual)
{
    if (expected != current_ || anchor.revision != current_) return Transition::StaleRevision;
    if (support_ != Support::Supported) return Transition::Blocked;
    const auto *mapping = correspondence();
    const auto *bound = mapping ? mapping->anchor(anchor.source.contributionId) : nullptr;
    if (!bound || *bound != anchor) return Transition::InvalidAnchor;
    if (anchor.state == AnchorState::Ambiguous) return Transition::AmbiguousTarget;
    if (anchor.state != AnchorState::Mapped || !anchor.result
        || !validByteRange(current().draft.text, *anchor.result) || !validUtf8(replacement))
        return Transition::InvalidAnchor;
    if (anchor.manualOverlap && !confirmManual) return Transition::ConfirmationRequired;
    auto draft = current().draft;
    draft.text.replace(anchor.result->begin, anchor.result->end - anchor.result->begin, replacement);
    if (exceedsEnvelope(draft.text)) return Transition::Blocked;
    if (decision) draft.decisions[anchor.source.contributionId] = *decision;
    if (draft == current().draft) return Transition::NoChange;
    const ResultEdit edit{*anchor.result, std::move(replacement)};
    auto updated = std::make_shared<const CorrespondenceMap>(
        mapping->edited(next_, edit, false, anchor.source.contributionId));
    appendHistory(advance(draft, std::move(updated)));
    return Transition::Applied;
}

Transition FileSession::undo(RevisionId expected)
{
    if (expected != current_) return Transition::StaleRevision;
    if (support_ != Support::Supported) return Transition::Blocked;
    if (!canUndo()) return Transition::NoChange;
    restore(revisions_.at(history_[cursor_ - 1]));
    --cursor_;
    return Transition::Applied;
}

Transition FileSession::redo(RevisionId expected)
{
    if (expected != current_) return Transition::StaleRevision;
    if (support_ != Support::Supported) return Transition::Blocked;
    if (!canRedo()) return Transition::NoChange;
    restore(revisions_.at(history_[cursor_ + 1]));
    ++cursor_;
    return Transition::Applied;
}

Transition FileSession::discardToCheckpoint(RevisionId expected)
{
    if (expected != current_) return Transition::StaleRevision;
    if (support_ != Support::Supported) return Transition::Blocked;
    const auto &target = checkpoint_ ? *checkpoint_ : revisions_.at(1);
    // Matching text after undo can still have pending Save callbacks from a
    // discarded revision. Only the checkpoint's own successful transaction is
    // a no-op; otherwise restore its identity and fence those callbacks below.
    if (target.id == current_
        && (!checkpointOutcome_ || succeeded(checkpointOutcome_, target.id)))
        return Transition::NoChange;
    appendHistory(restore(target));
    outcomeFloor_ = current_;
    checkpointOutcome_ = checkpoint_
        ? std::optional<OperationOutcome>{{checkpoint_->id, Outcome::Succeeded, {}}}
        : std::nullopt;
    return Transition::Applied;
}

Transition FileSession::beginMarkResolved(RevisionId expected, bool confirmUndecided)
{
    if (expected != current_) return Transition::StaleRevision;
    if (support_ != Support::Supported || current().draft.unresolvedMarkers != 0)
        return Transition::Blocked;
    if (undecidedCount(current().draft) != 0 && !confirmUndecided)
        return Transition::ConfirmationRequired;
    reviewed_ = Review{current(), confirmUndecided};
    reviewCheckpointSucceeded_ = checkpoint_ && checkpoint_->id == expected
        && succeeded(checkpointOutcome_, expected);
    return Transition::Applied;
}

Transition FileSession::recordCheckpoint(RevisionId revision, Outcome outcome, std::string detail)
{
    const auto found = revisions_.find(revision);
    if (found == revisions_.end()
        || (revision < outcomeFloor_ && revision != current_)
        || (checkpointOutcome_ && revision < checkpointOutcome_->revision))
        return Transition::StaleRevision;
    checkpointOutcome_ = OperationOutcome{revision, outcome, std::move(detail)};
    if (outcome == Outcome::Succeeded) checkpoint_ = found->second;
    if (reviewed_ && reviewed_->revision.id == revision)
        reviewCheckpointSucceeded_ = outcome == Outcome::Succeeded;
    return Transition::Applied;
}

Transition FileSession::recordWrite(RevisionId revision, Outcome outcome, std::string detail)
{
    if (!reviewed_ || reviewed_->revision.id != revision
        || (revision < outcomeFloor_ && revision != current_)) return Transition::StaleRevision;
    if (!reviewCheckpointSucceeded_) return Transition::InvalidOutcome;
    writeOutcome_ = OperationOutcome{revision, outcome, std::move(detail)};
    if (outcome == Outcome::Succeeded) written_ = reviewed_->revision;
    return Transition::Applied;
}

Transition FileSession::recordStage(RevisionId revision, Outcome outcome, std::string objectId,
                                    std::string mode, std::string detail)
{
    if (!reviewed_ || reviewed_->revision.id != revision
        || (revision < outcomeFloor_ && revision != current_)) return Transition::StaleRevision;
    if (!written_ || written_->id != revision || !succeeded(writeOutcome_, revision))
        return Transition::InvalidOutcome;
    if (outcome == Outcome::Succeeded && (objectId.empty() || mode.empty()))
        return Transition::InvalidOutcome;
    stageOutcome_ = OperationOutcome{revision, outcome, std::move(detail)};
    if (outcome == Outcome::Succeeded) {
        staged_ = StagedResult{reviewed_->revision, std::move(objectId), std::move(mode),
                               reviewed_->confirmedUndecided};
    }
    return Transition::Applied;
}

Transition FileSession::discardToStaged(RevisionId expected)
{
    if (expected != current_) return Transition::StaleRevision;
    if (support_ != Support::Supported || !staged_ || !written_
        || written_->id != staged_->revision.id
        || !succeeded(writeOutcome_, staged_->revision.id)
        || !succeeded(stageOutcome_, staged_->revision.id))
        return Transition::Blocked;
    if (current_ == staged_->revision.id) return Transition::NoChange;
    // Selecting this previously saved, verified state deliberately discards the
    // newer draft/checkpoint. No external state changes or new review occur.
    current_ = staged_->revision.id;
    outcomeFloor_ = next_;
    checkpoint_ = staged_->revision;
    checkpointOutcome_ = OperationOutcome{current_, Outcome::Succeeded, {}};
    reviewed_ = Review{staged_->revision, staged_->confirmedUndecided};
    reviewCheckpointSucceeded_ = true;
    appendHistory(current_);
    return Transition::Applied;
}

Transition FileSession::setSupport(Support support, std::string detail)
{
    if (support_ == support && supportDetail_ == detail) return Transition::NoChange;
    support_ = support;
    supportDetail_ = std::move(detail);
    return Transition::Applied;
}

FileFacts FileSession::facts() const
{
    const auto undecided = undecidedCount(current().draft);
    const bool reviewed = reviewed_ && reviewed_->revision.id == current_;
    const bool checkpoint = checkpoint_
        && checkpoint_->result == current().result
        && checkpoint_->decisions == current().decisions;
    const bool written = written_ && written_->id == current_ && succeeded(writeOutcome_, current_);
    const bool staged = staged_ && staged_->revision.id == current_ && succeeded(stageOutcome_, current_);
    const bool resolved = support_ == Support::Supported && reviewed && checkpoint && written
        && staged && succeeded(checkpointOutcome_, current_) && current().draft.unresolvedMarkers == 0;
    FileFacts facts{undecided, !checkpoint, !reviewed, staged_ && staged_->revision.id != current_,
                    resolved, FileStatus::ReviewNeeded, {}};
    auto &blockers = facts.blockers;
    if (support_ == Support::OutsideApp) blockers.push_back(Blocker::OutsideApp);
    if (support_ == Support::Unreadable) blockers.push_back(Blocker::Unreadable);
    // A file the app cannot edit has no draft to Save, mark resolved, write or
    // stage, so only its support blocker and recorded failures apply. It stays
    // unresolved because resolution requires Support::Supported.
    if (support_ == Support::Supported) {
        if (current().draft.unresolvedMarkers != 0) blockers.push_back(Blocker::UnresolvedMarkers);
        if (undecided != 0 && !(reviewed && reviewed_->confirmedUndecided))
            blockers.push_back(Blocker::Undecided);
        if (facts.unsaved) blockers.push_back(Blocker::UnsavedDraft);
        if (facts.needsReview) blockers.push_back(Blocker::ReviewNeeded);
        if (!written) blockers.push_back(Blocker::WriteRequired);
        if (!staged) blockers.push_back(Blocker::StageRequired);
        if (facts.earlierStaged) blockers.push_back(Blocker::EarlierStagedResult);
    }
    const auto addFailure = [&](const auto &outcome, Blocker failure) {
        if (!outcome) return;
        if (outcome->outcome == Outcome::Failed) blockers.push_back(failure);
        if (outcome->outcome == Outcome::Uncertain
            && std::find(blockers.begin(), blockers.end(), Blocker::UncertainOutcome) == blockers.end())
            blockers.push_back(Blocker::UncertainOutcome);
    };
    addFailure(checkpointOutcome_, Blocker::CheckpointFailed);
    addFailure(writeOutcome_, Blocker::WriteFailed);
    addFailure(stageOutcome_, Blocker::StageFailed);
    // Failures remain outstanding even if an earlier matching snapshot exists.
    facts.resolved = facts.resolved && blockers.empty();
    const auto has = [&](Blocker blocker) {
        return std::find(blockers.begin(), blockers.end(), blocker) != blockers.end();
    };
    if (support_ == Support::OutsideApp) facts.status = FileStatus::OutsideApp;
    else if (support_ == Support::Unreadable) facts.status = FileStatus::Unreadable;
    else if (has(Blocker::UncertainOutcome)) facts.status = FileStatus::OutcomeUncertain;
    else if (has(Blocker::CheckpointFailed)) facts.status = FileStatus::CheckpointFailed;
    else if (has(Blocker::WriteFailed)) facts.status = FileStatus::WriteFailed;
    else if (has(Blocker::StageFailed)) facts.status = FileStatus::StageFailed;
    else if (facts.earlierStaged) facts.status = FileStatus::EarlierStaged;
    else if (facts.resolved) facts.status = FileStatus::Resolved;
    else if (written_ && succeeded(writeOutcome_, written_->id))
        facts.status = FileStatus::WrittenUnresolved;
    else if (checkpoint) facts.status = FileStatus::SavedDraft;
    else if (undecided != 0) facts.status = FileStatus::Undecided;
    return facts;
}

const char *statusText(FileStatus status) noexcept
{
    switch (status) {
    case FileStatus::OutsideApp: return "Needs work outside Branch Truce";
    case FileStatus::Unreadable: return "File could not be read";
    case FileStatus::OutcomeUncertain: return "Outcome uncertain; verification required";
    case FileStatus::CheckpointFailed: return "Draft save failed";
    case FileStatus::WriteFailed: return "Repository write failed";
    case FileStatus::StageFailed: return "Written, unresolved";
    case FileStatus::EarlierStaged: return "Earlier result staged; current draft not applied";
    case FileStatus::Resolved: return "Written and resolved";
    case FileStatus::WrittenUnresolved: return "Written, unresolved";
    case FileStatus::SavedDraft: return "Draft saved in Branch Truce; repository file unchanged";
    case FileStatus::Undecided: return "Source choices remain undecided";
    case FileStatus::ReviewNeeded: return "Current result needs review";
    }
    return "";
}

const char *blockerText(Blocker blocker) noexcept
{
    switch (blocker) {
    case Blocker::OutsideApp: return "Handle this file outside Branch Truce";
    case Blocker::Unreadable: return "File content unavailable";
    case Blocker::UnresolvedMarkers: return "Known conflict markers remain";
    case Blocker::Undecided: return "Undecided source choices need confirmation";
    case Blocker::UnsavedDraft: return "Current draft has no manual Save checkpoint";
    case Blocker::ReviewNeeded: return "Current result needs Mark resolved";
    case Blocker::WriteRequired: return "Current result has not been verified written";
    case Blocker::StageRequired: return "Current result has not been verified staged";
    case Blocker::EarlierStagedResult: return "Earlier result staged; current draft not applied";
    case Blocker::CheckpointFailed: return "Draft save failed; retry or discard explicitly";
    case Blocker::WriteFailed: return "Repository write failed";
    case Blocker::StageFailed: return "Git resolution failed";
    case Blocker::UncertainOutcome: return "Inspect uncertain mutation before finishing";
    }
    return "";
}
}
