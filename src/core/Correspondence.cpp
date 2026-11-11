// Passage correspondence maps captured source ranges to one revision of the editable result.
#include "Correspondence.h"
#include "AnchorRebuild.h"
#include "ResourceLimits.h"
#include "Utf8.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace branch_truce {
namespace {
using anchor_rebuild::invalidate;
using anchor_rebuild::KeptView;
using anchor_rebuild::Rebuild;

constexpr std::array kSides{SourceSide::Earlier, SourceSide::Stage2, SourceSide::Stage3};

std::optional<std::size_t> lineIndex(const std::vector<std::size_t> &starts, std::size_t offset)
{
    const auto found = std::lower_bound(starts.begin(), starts.end(), offset);
    if (found == starts.end() || *found != offset) return {};
    return static_cast<std::size_t>(found - starts.begin());
}

// Line starts of the two compared texts. Binding pairs the lines inside a
// changed passage by index; whole-buffer edits pass none and keep only the
// passage edges.
struct LinePairing {
    const std::vector<std::size_t> &source;
    const std::vector<std::size_t> &result;

    // The result position of a line start strictly inside a changed passage.
    // Source line k pairs with result line k. Source lines beyond the result
    // block's line count map to its end, where they insert.
    [[nodiscard]] std::optional<std::size_t> position(const Passage &passage, std::size_t offset) const
    {
        const auto line = lineIndex(source, offset);
        const auto first = lineIndex(source, passage.source.begin);
        if (!line || !first) return {};
        if (passage.result.begin == passage.result.end) return passage.result.end;
        const auto start = lineIndex(result, passage.result.begin);
        if (!start) return {};
        const auto paired = *start + *line - *first;
        return paired < result.size() && result[paired] < passage.result.end ? result[paired] : passage.result.end;
    }
};

struct Projection {
    AnchorState state = AnchorState::Ambiguous;
    std::optional<ByteRange> range;
    bool changed = false;
};

// How a range meets the passages, in passage order up to the first
// ambiguous passage it overlaps: whether there is one, and whether a changed
// passage came before it.
struct Overlap {
    bool ambiguous = false;
    bool changed = false;
};

Overlap overlapOf(const std::vector<Passage> &passages, ByteRange range)
{
    Overlap result;
    for (const auto &passage : passages) {
        const auto s = passage.source;
        const bool overlap = range.begin == range.end ? s.begin < range.begin && range.begin < s.end
                                                      : s.begin < range.end && range.begin < s.end;
        const bool interiorInsertion = s.begin == s.end && range.begin < s.begin && s.begin < range.end;
        if (!overlap && !interiorInsertion) continue;
        if (passage.kind == PassageKind::Ambiguous) {
            result.ambiguous = true;
            return result;
        }
        if (passage.kind != PassageKind::Equal) result.changed = true;
    }
    return result;
}

// The lowest and highest result offset a source offset can map to.
struct Candidates {
    std::optional<std::size_t> low;
    std::optional<std::size_t> high;
    void add(std::size_t candidate)
    {
        low = low ? std::min(*low, candidate) : candidate;
        high = high ? std::max(*high, candidate) : candidate;
    }
};

// Where a source offset lies in the result. Equal passages map every offset;
// other passages map their edges, and with a pairing the line starts inside a
// changed passage. An empty insertion anchor inside a changed passage still
// has no verified position.
std::optional<std::size_t> position(const std::vector<Passage> &passages, std::size_t offset,
                                    InsertionAffinity insertion, const LinePairing *pairing, bool empty)
{
    Candidates found;
    for (const auto &passage : passages) {
        if (offset < passage.source.begin || offset > passage.source.end) continue;
        if (passage.kind == PassageKind::Equal) {
            found.add(passage.result.begin + offset - passage.source.begin);
            continue;
        }
        if (offset == passage.source.begin) found.add(passage.result.begin);
        if (offset == passage.source.end) found.add(passage.result.end);
        const bool interior = passage.source.begin < offset && offset < passage.source.end;
        const bool pairs = passage.kind == PassageKind::Changed || passage.kind == PassageKind::SourceOnly;
        if (pairing && !empty && interior && pairs)
            if (const auto paired = pairing->position(passage, offset)) found.add(*paired);
    }
    return insertion == InsertionAffinity::Before ? found.low : found.high;
}

Projection project(const std::vector<Passage> &passages, ByteRange range, InsertionAffinity affinity,
                   const LinePairing *pairing = nullptr)
{
    Projection projection;
    const auto overlap = overlapOf(passages, range);
    projection.changed = overlap.changed;
    if (overlap.ambiguous) return projection;
    const bool empty = range.begin == range.end;
    const auto begin = position(passages, range.begin, empty ? affinity : InsertionAffinity::After, pairing, empty);
    const auto end = position(passages, range.end, empty ? affinity : InsertionAffinity::Before, pairing, empty);
    if (!begin || !end || *begin > *end) return projection;
    projection.state = AnchorState::Mapped;
    projection.range = ByteRange{*begin, *end};
    return projection;
}

// Hand-edited ranges after `edit` replaced its range with `inserted` bytes.
// The replaced text leaves them, and a hand edit adds the text it wrote.
std::vector<ByteRange> editedRanges(const std::vector<ByteRange> &ranges, ByteRange edit, std::size_t inserted,
                                    bool manual)
{
    const auto shift = [&](std::size_t offset) { return offset - (edit.end - edit.begin) + inserted; };
    std::vector<ByteRange> result;
    for (const auto &range : ranges) {
        if (range.begin == range.end) {
            if (range.begin <= edit.begin)
                result.push_back(range);
            else if (range.begin >= edit.end)
                result.push_back({shift(range.begin), shift(range.end)});
            continue;
        }
        if (range.begin < edit.begin) result.push_back({range.begin, std::min(range.end, edit.begin)});
        if (range.end > edit.end) result.push_back({shift(std::max(range.begin, edit.end)), shift(range.end)});
    }
    if (manual) result.push_back({edit.begin, edit.begin + inserted});
    return mergeRanges(std::move(result));
}

// Hand-edited ranges after a whole-buffer update with these old-to-new
// passages. A range edge inside changed text widens to cover it, and every
// changed passage is a hand edit too.
std::vector<ByteRange> recomputedRanges(const std::vector<ByteRange> &ranges, const std::vector<Passage> &changes,
                                        std::size_t size)
{
    const auto moved = [&](std::size_t offset, bool end) {
        for (const auto &passage : changes) {
            if (offset > passage.source.end) continue;
            if (offset == passage.source.end) return passage.result.end;
            if (offset == passage.source.begin) return passage.result.begin;
            if (passage.kind == PassageKind::Equal) return passage.result.begin + offset - passage.source.begin;
            return end ? passage.result.end : passage.result.begin;
        }
        return size;
    };
    std::vector<ByteRange> result;
    result.reserve(ranges.size() + changes.size());
    for (const auto &range : ranges) result.push_back({moved(range.begin, false), moved(range.end, true)});
    for (const auto &passage : changes)
        if (passage.kind != PassageKind::Equal) result.push_back(passage.result);
    return mergeRanges(std::move(result));
}

// Insertions at the edges of a nonempty range stay outside it, as in editing.
bool within(const SourceAnchor &outer, const SourceAnchor &inner)
{
    const auto &a = outer.range;
    const auto &b = inner.range;
    return outer.side == inner.side
        && (b.begin == b.end ? a.begin < b.begin && b.begin < a.end : a.begin <= b.begin && b.end <= a.end);
}

// Another nonempty contribution from the chosen side, such as the next surplus
// line at the same insertion point, keeps its own place beside the choice.
bool sibling(const SourceAnchor &chosen, const SourceAnchor &other)
{
    const auto &a = chosen.range;
    const auto &b = other.range;
    return chosen.side == other.side && a.begin != a.end && b.begin != b.end && (a.end <= b.begin || b.end <= a.begin);
}

void transform(ResultAnchor &anchor, const ResultEdit &edit, bool manual, bool selected, bool beside)
{
    if (anchor.state != AnchorState::Mapped || !anchor.result) return;
    auto range = *anchor.result;
    const auto removed = edit.range.end - edit.range.begin;
    const auto inserted = edit.replacement.size();
    const auto shift = [&](std::size_t offset) { return offset - removed + inserted; };
    // A competing anchor on the replaced range follows the chosen text.
    if (selected || (!manual && !beside && range == edit.range)) {
        anchor.result = ByteRange{edit.range.begin, edit.range.begin + inserted};
        if (selected) anchor.manualOverlap = false;
        anchor.handEdited = false;
        return;
    }
    if (range.begin == range.end) {
        auto position = range.begin;
        if (edit.range.begin < position && position < edit.range.end) {
            invalidate(anchor, AnchorState::Invalidated);
            return;
        }
        if (position >= edit.range.end && position != edit.range.begin)
            position = shift(position);
        else if (position == edit.range.begin) {
            position = edit.range.begin + (anchor.source.affinity == InsertionAffinity::After ? inserted : 0);
        }
        anchor.result = ByteRange{position, position};
        return;
    }
    if (edit.range.end <= range.begin) {
        range.begin = shift(range.begin);
        range.end = shift(range.end);
    } else if (edit.range.begin >= range.end) {
        return;
    } else if (edit.range.begin >= range.begin && edit.range.end <= range.end) {
        range.end = shift(range.end);
        anchor.manualOverlap = anchor.manualOverlap || manual;
        anchor.handEdited = anchor.handEdited || manual;
        if (manual && range.begin == range.end) {
            invalidate(anchor, AnchorState::Invalidated);
            return;
        }
    } else {
        // Typing across a boundary still changes this range by hand. A command
        // that replaces all of it leaves no hand edit behind.
        if (manual)
            anchor.handEdited = true;
        else if (edit.range.begin <= range.begin && range.end <= edit.range.end)
            anchor.handEdited = false;
        invalidate(anchor, AnchorState::Invalidated);
        return;
    }
    anchor.result = range;
}
// Where the line holding `position` starts.
std::size_t lineBegin(std::string_view text, std::size_t position) noexcept
{
    const auto newline = position == 0 ? std::string_view::npos : text.rfind('\n', position - 1);
    return newline == std::string_view::npos ? 0 : newline + 1;
}

// Provisional passages after an edit of the result: the passages outside the
// lines the edit touches keep their alignment, those after it shifted, and
// the rest is one Ambiguous passage between them. Equal passages are cut at
// those line edges, so lines both texts share outside the edit stay equal.
std::vector<Passage> shifted(const std::vector<Passage> &passages, std::string_view old, ByteRange edit,
                             std::size_t inserted)
{
    const auto from = lineBegin(old, edit.begin);
    const auto to = edit.end == old.size() ? old.size() : lineAt(old, lineBegin(old, edit.end)).end;
    const auto shift = [&](std::size_t offset) { return offset - (edit.end - edit.begin) + inserted; };
    std::vector<Passage> head;
    std::vector<Passage> tail;
    for (const auto &passage : passages) {
        const auto &source = passage.source;
        const auto &result = passage.result;
        if (result.end <= from) {
            head.push_back(passage);
        } else if (result.begin >= to) {
            tail.push_back({source, {shift(result.begin), shift(result.end)}, passage.kind});
        } else if (passage.kind == PassageKind::Equal) {
            if (result.begin < from)
                head.push_back(
                    {{source.begin, source.begin + from - result.begin}, {result.begin, from}, passage.kind});
            if (result.end > to)
                tail.push_back(
                    {{source.end - (result.end - to), source.end}, {shift(to), shift(result.end)}, passage.kind});
        }
    }
    const ByteRange sourceWindow{head.empty() ? 0 : head.back().source.end,
                                 tail.empty() ? (passages.empty() ? 0 : passages.back().source.end)
                                              : tail.front().source.begin};
    const ByteRange resultWindow{head.empty() ? 0 : head.back().result.end,
                                 tail.empty() ? shift(old.size()) : tail.front().result.begin};
    auto result = std::move(head);
    result.reserve(result.size() + 1 + tail.size());
    if (sourceWindow.begin != sourceWindow.end || resultWindow.begin != resultWindow.end)
        result.push_back({sourceWindow, resultWindow, PassageKind::Ambiguous});
    result.insert(result.end(), tail.begin(), tail.end());
    return result;
}

}

void requireComparable(std::string_view text)
{
    if (!withinTextEnvelope(text)) throw std::invalid_argument("Correspondence text is outside the supported envelope");
    if (!validUtf8(text)) throw std::invalid_argument("Correspondence requires valid UTF-8 text");
}

CorrespondenceMap::CorrespondenceMap(RevisionId revision, std::shared_ptr<const FileSnapshots> sources,
                                     std::string result, std::shared_ptr<const CorrespondenceAlgorithm> algorithm)
    : revision_(revision), sources_(std::move(sources)), algorithm_(std::move(algorithm)), text_(std::move(result))
{
    if (!sources_) throw std::invalid_argument("Correspondence needs captured sources");
    if (!algorithm_) throw std::invalid_argument("Correspondence needs an alignment algorithm");
    requireComparable(text_);
    if (lineCount(text_) > kMaxIndexedLines) throw std::length_error("Comparison line index limit");
    std::map<SourceSide, std::vector<std::size_t>> starts;
    for (const auto side : kSides) {
        if (const auto *text = source(side)) {
            requireComparable(*text);
            if (lineCount(*text) > kMaxIndexedLines) throw std::length_error("Comparison line index limit");
            starts.emplace(side, lineStarts(*text));
        }
    }
    sourceLines_ = std::make_shared<const std::map<SourceSide, std::vector<std::size_t>>>(std::move(starts));
    align();
}

void CorrespondenceMap::align(std::size_t cells)
{
    requireComparable(text_);
    resultLines_.reset();
    MetadataBudget work{cells, cells};
    const auto count = lineCount(text_);
    for (const auto side : kSides)
        if (const auto *text = source(side)) {
            if (*text == text_ || work.take(lineCount(*text), count))
                passages_[side] = algorithm_->compare(*text, text_);
            else
                passages_[side] = {{{0, text->size()}, {0, text_.size()}, PassageKind::Ambiguous}};
        }
    aligned_ = true;
}

void CorrespondenceMap::shiftPassages(std::string_view old, ByteRange edit, std::size_t inserted)
{
    resultLines_.reset();
    for (auto &[side, passages] : passages_) {
        (void)side;
        passages = shifted(passages, old, edit, inserted);
    }
    aligned_ = false;
}

CorrespondenceMap CorrespondenceMap::realigned() const
{
    auto result = *this;
    result.align();
    return result;
}

bool CorrespondenceMap::sameAnchors(const CorrespondenceMap &other) const
{
    return revision_ == other.revision_ && sources_ == other.sources_ && text_ == other.text_
        && anchors_ == other.anchors_ && handEditedRanges_ == other.handEditedRanges_;
}

const std::vector<Passage> *CorrespondenceMap::passages(SourceSide side) const noexcept
{
    const auto found = passages_.find(side);
    return found == passages_.end() ? nullptr : &found->second;
}

const std::string *CorrespondenceMap::source(SourceSide side) const noexcept
{
    const auto &captured = sources_->source(side);
    return captured ? &captured->worktree.utf8 : nullptr;
}

const ResultAnchor *CorrespondenceMap::anchor(const std::string &id) const noexcept
{
    const auto found = anchors_.find(id);
    return found == anchors_.end() ? nullptr : &found->second;
}

bool CorrespondenceMap::bind(const SourceAnchor &source)
{
    const auto &captured = sources_->source(source.side);
    if (source.contributionId.empty() || !captured || !validByteRange(captured->worktree.utf8, source.range)
        || anchors_.contains(source.contributionId))
        return false;
    auto budget = anchorBudget_;
    if (!budget.take(1,
                     sizeof(ResultAnchor) + 4 * sizeof(void *) + 2 * source.contributionId.size()
                         + sources_->path.size() + sources_->provenance.sessionId.size() + captured->objectId.size())
        || (!resultLines_ && lineCount(text_) > kMaxIndexedLines))
        throw std::length_error("Anchor metadata limit");
    if (!resultLines_) resultLines_ = std::make_shared<const std::vector<std::size_t>>(lineStarts(text_));
    const LinePairing pairing{sourceLines_->at(source.side), *resultLines_};
    const auto projected = project(passages_.at(source.side), source.range, source.affinity, &pairing);
    const auto id = source.contributionId;
    const AnchorOrigin origin{sources_->provenance.sessionId, sources_->path, captured->objectId};
    // Launch text that differs from its source needs confirmation to replace,
    // but nobody has edited it by hand yet.
    anchors_.emplace(
        id,
        ResultAnchor{origin, source, revision_, projected.state, projected.range,
                     projected.changed && projected.range && projected.range->begin != projected.range->end, false});
    anchorBudget_ = budget;
    return true;
}

void CorrespondenceMap::settle(SourceSide side, const std::vector<ByteRange> &dropped)
{
    const auto *source = this->source(side);
    if (!source || std::none_of(anchors_.begin(), anchors_.end(), [&](const auto &entry) {
            return entry.second.source.side == side && entry.second.state == AnchorState::Ambiguous;
        }))
        return;
    const KeptView kept(text_, dropped);
    const auto passages = algorithm_->compare(*source, kept.text());
    const auto viewLines = lineStarts(kept.text());
    const LinePairing pairing{sourceLines_->at(side), viewLines};
    for (auto &entry : anchors_) {
        auto &anchor = entry.second;
        if (anchor.source.side != side || anchor.state != AnchorState::Ambiguous) continue;
        const auto projected = project(passages, anchor.source.range, anchor.source.affinity, &pairing);
        const auto range = projected.range ? kept.place(*projected.range) : std::nullopt;
        if (projected.state != AnchorState::Mapped || !range) continue;
        anchor.state = AnchorState::Mapped;
        anchor.result = range;
        anchor.manualOverlap = projected.changed && range->begin != range->end;
    }
}

CorrespondenceMap CorrespondenceMap::atRevision(RevisionId revision) const
{
    auto result = *this;
    result.revision_ = revision;
    for (auto &[id, anchor] : result.anchors_) {
        (void)id;
        anchor.revision = revision;
    }
    return result;
}

CorrespondenceMap CorrespondenceMap::edited(RevisionId revision, const ResultEdit &edit, bool manual,
                                            const std::string &selectedAnchor, Realignment realignment) &&
{
    if (!validByteRange(text_, edit.range)) throw std::invalid_argument("Result edit must use UTF-8 boundaries");
    if (!replacementFits(text_, edit.range, edit.replacement))
        throw std::invalid_argument("Result edit is outside the supported envelope");
    if (!validUtf8(edit.replacement)) throw std::invalid_argument("Result edit must use UTF-8 text");
    auto &result = *this;
    result.revision_ = revision;
    const bool changed = slice(text_, edit.range) != edit.replacement;
    const auto *selected = manual ? nullptr : anchor(selectedAnchor);
    std::optional<std::vector<std::size_t>> lines;
    const bool indexed = resultLines_ || lineCount(text_) <= kMaxIndexedLines;
    // A competing line loses its range when the result line it pairs with is
    // deleted by hand. Where its source pairs in the text before this choice
    // tells whether the choice replaced that place.
    const auto replaced = [&](const SourceAnchor &source) {
        if (edit.range.begin == edit.range.end) return false;
        if (!indexed) return false;
        if (!resultLines_ && !lines) lines = lineStarts(text_);
        const LinePairing pairing{sourceLines_->at(source.side), resultLines_ ? *resultLines_ : *lines};
        const auto projected = project(passages_.at(source.side), source.range, source.affinity, &pairing);
        return projected.range && edit.range.begin <= projected.range->begin && projected.range->end <= edit.range.end;
    };
    for (auto &[id, value] : result.anchors_) {
        // An unmapped anchor has no result range to compare, but a source range
        // inside the selected one means the choice replaced its text too.
        if (selected && value.state != AnchorState::Mapped && value.handEdited
            && (within(selected->source, value.source) || replaced(value.source)))
            value.handEdited = false;
        value.revision = revision;
        transform(value, edit, manual, id == selectedAnchor, selected && sibling(selected->source, value.source));
    }
    result.handEditedRanges_ = editedRanges(handEditedRanges_, edit.range, edit.replacement.size(), manual);
    if (changed) result.shiftPassages(text_, edit.range, edit.replacement.size());
    result.text_.replace(edit.range.begin, edit.range.end - edit.range.begin, edit.replacement);
    if (realignment == Realignment::Now && !result.aligned_) result.align();
    return std::move(result);
}

CorrespondenceMap CorrespondenceMap::rebuilt(RevisionId revision, ByteRange range, const std::vector<KeptSpan> &spans,
                                             Realignment realignment) &&
{
    const Rebuild rebuild(text_, range, spans);
    if (!replacementFits(text_, range, rebuild.replacement()))
        throw std::invalid_argument("Rebuilt result is outside the supported envelope");
    auto &result = *this;
    result.revision_ = revision;
    for (auto &[id, anchor] : result.anchors_) {
        (void)id;
        anchor.revision = revision;
        rebuild.move(anchor);
    }
    result.handEditedRanges_ = rebuild.handEdits(handEditedRanges_);
    result.shiftPassages(text_, range, rebuild.replacement().size());
    result.text_.replace(range.begin, range.end - range.begin, rebuild.replacement());
    if (realignment == Realignment::Now) result.align();
    return std::move(result);
}

CorrespondenceMap CorrespondenceMap::recomputed(RevisionId revision, std::string text, bool manual) const
{
    if (!partsFit(std::array{std::string_view(text)}))
        throw std::invalid_argument("Result text is outside the supported envelope");
    requireComparable(text);
    auto result = atRevision(revision);
    if (!manual) {
        for (auto &[id, anchor] : result.anchors_) {
            (void)id;
            anchor.handEdited = false;
        }
        result.handEditedRanges_.clear();
    }
    if (text_ == text) return result;
    MetadataBudget work{kMaxAlignmentCells, kMaxAlignmentCells};
    const auto changes = work.take(lineCount(text_), lineCount(text))
        ? algorithm_->compare(text_, text)
        : std::vector<Passage>{{{0, text_.size()}, {0, text.size()}, PassageKind::Ambiguous}};
    if (manual) result.handEditedRanges_ = recomputedRanges(handEditedRanges_, changes, text.size());
    for (auto &[id, anchor] : result.anchors_) {
        (void)id;
        if (anchor.state != AnchorState::Mapped || !anchor.result) continue;
        const auto old = *anchor.result;
        const auto projected = project(changes, old, anchor.source.affinity);
        anchor.state = projected.state;
        anchor.result = projected.range;
        anchor.manualOverlap = anchor.manualOverlap || projected.changed;
        anchor.handEdited = anchor.handEdited || (manual && projected.changed);
        if (old.begin != old.end && projected.range && projected.range->begin == projected.range->end)
            invalidate(anchor, AnchorState::Invalidated);
    }
    result.text_ = std::move(text);
    result.align(work.bytes);
    return result;
}

std::size_t CorrespondenceMap::retainedBytes() const noexcept
{
    std::size_t bytes = sizeof(*this) + text_.capacity() + 1 + handEditedRanges_.capacity() * sizeof(ByteRange);
    for (const auto &[side, passages] : passages_)
        bytes += sizeof(decltype(passages_)::value_type) + 4 * sizeof(void *) + passages.capacity() * sizeof(Passage);
    if (sourceLines_)
        for (const auto &[side, lines] : *sourceLines_)
            bytes += sizeof(decltype(sourceLines_)::element_type::value_type) + 4 * sizeof(void *)
                + lines.capacity() * sizeof(std::size_t);
    if (resultLines_) bytes += sizeof(std::vector<std::size_t>) + resultLines_->capacity() * sizeof(std::size_t);
    for (const auto &[id, anchor] : anchors_)
        bytes += sizeof(decltype(anchors_)::value_type) + 4 * sizeof(void *) + id.capacity() + 1
            + anchor.source.contributionId.capacity() + 1 + anchor.origin.sessionId.capacity() + 1
            + anchor.origin.path.capacity() + 1 + anchor.origin.sourceObjectId.capacity() + 1;
    return bytes;
}

std::set<std::string> CorrespondenceMap::handEditedAnchors() const
{
    std::set<std::string> result;
    for (const auto &[id, anchor] : anchors_)
        if (anchor.handEdited) result.insert(id);
    return result;
}

bool CorrespondenceMap::restoreHandEditedRanges(std::vector<ByteRange> ranges)
{
    for (std::size_t i = 0; i < ranges.size(); ++i)
        if (!validByteRange(text_, ranges[i]) || (i && ranges[i].begin <= ranges[i - 1].end)) return false;
    handEditedRanges_ = std::move(ranges);
    return true;
}

bool CorrespondenceMap::restoreHandEdits(const std::set<std::string> &anchors)
{
    if (std::any_of(anchors.begin(), anchors.end(), [&](const auto &id) { return !anchors_.contains(id); }))
        return false;
    for (auto &[id, anchor] : anchors_) anchor.handEdited = anchors.contains(id);
    return true;
}

}
