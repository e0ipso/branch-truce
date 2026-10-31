// Passage correspondence maps captured source ranges to one revision of the editable result.
#include "Correspondence.h"
#include "ResourceLimits.h"

#include <algorithm>
#include <span>
#include <stdexcept>
#include <utility>

namespace branch_truce {
namespace {
bool boundary(std::string_view text, std::size_t position)
{
    return position <= text.size() && (position == text.size()
        || (static_cast<unsigned char>(text[position]) & 0xc0U) != 0x80U);
}

struct Line {
    std::size_t begin;
    std::size_t end;
    std::size_t token;
};

std::vector<Line> lines(std::string_view text,
                       std::map<std::string_view, std::size_t, std::less<>> &tokens)
{
    std::vector<Line> result;
    for (std::size_t begin = 0; begin < text.size();) {
        const auto newline = text.find('\n', begin);
        const auto end = newline == std::string_view::npos ? text.size() : newline + 1;
        const auto [entry, inserted] = tokens.try_emplace(text.substr(begin, end - begin), tokens.size());
        (void)inserted;
        result.push_back({begin, end, entry->second});
        begin = end;
    }
    return result;
}

struct LineMatch {
    std::size_t source;
    std::size_t result;
    bool operator==(const LineMatch &) const = default;
};

std::vector<std::size_t> lcsLengths(std::span<const Line> source,
                                    std::span<const Line> result, bool reverse)
{
    std::vector<std::size_t> row(result.size() + 1);
    for (std::size_t i = 0; i < source.size(); ++i) {
        const auto token = source[reverse ? source.size() - 1 - i : i].token;
        std::size_t diagonal = 0;
        for (std::size_t j = 0; j < result.size(); ++j) {
            const auto previous = row[j + 1];
            row[j + 1] = token == result[reverse ? result.size() - 1 - j : j].token
                ? diagonal + 1 : std::max(row[j], previous);
            diagonal = previous;
        }
    }
    return row;
}

// Hirschberg's divide and conquer keeps only two length rows. Choosing the
// first/last optimal crossing at every split traces the two boundary paths of
// the optimal-alignment graph, including skips at equal lines. A match shared
// by both boundaries is shared by every optimal alignment.
void boundaryAlignment(std::span<const Line> source, std::span<const Line> result,
                       std::size_t sourceOffset, std::size_t resultOffset,
                       bool last, std::vector<LineMatch> &matches)
{
    if (source.empty() || result.empty()) return;
    if (source.size() == 1) {
        for (std::size_t offset = 0; offset < result.size(); ++offset) {
            const auto j = last ? result.size() - 1 - offset : offset;
            if (source.front().token == result[j].token) {
                matches.push_back({sourceOffset, resultOffset + j});
                return;
            }
        }
        return;
    }
    const auto middle = source.size() / 2;
    std::size_t split = 0;
    {
        const auto forward = lcsLengths(source.first(middle), result, false);
        const auto backward = lcsLengths(source.subspan(middle), result, true);
        std::size_t best = 0;
        for (std::size_t j = 0; j <= result.size(); ++j) {
            const auto length = forward[j] + backward[result.size() - j];
            if (length > best || (last && length == best)) {
                best = length;
                split = j;
            }
        }
    } // Release the rows before descending, keeping workspace linear.
    boundaryAlignment(source.first(middle), result.first(split), sourceOffset,
                      resultOffset, last, matches);
    boundaryAlignment(source.subspan(middle), result.subspan(split), sourceOffset + middle,
                      resultOffset + split, last, matches);
}

std::vector<std::size_t> lineStarts(std::string_view text)
{
    std::vector<std::size_t> starts;
    for (std::size_t begin = 0; begin < text.size();) {
        starts.push_back(begin);
        const auto newline = text.find('\n', begin);
        begin = newline == std::string_view::npos ? text.size() : newline + 1;
    }
    return starts;
}

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

Projection project(const std::vector<Passage> &passages, ByteRange range,
                   InsertionAffinity affinity, const LinePairing *pairing = nullptr)
{
    Projection projection;
    for (const auto &passage : passages) {
        const auto s = passage.source;
        const bool overlap = range.begin == range.end
            ? s.begin < range.begin && range.begin < s.end
            : s.begin < range.end && range.begin < s.end;
        const bool interiorInsertion = s.begin == s.end
            && range.begin < s.begin && s.begin < range.end;
        if ((overlap || interiorInsertion) && passage.kind == PassageKind::Ambiguous)
            return projection;
        if ((overlap || interiorInsertion) && passage.kind != PassageKind::Equal)
            projection.changed = true;
    }
    const bool empty = range.begin == range.end;
    const auto position = [&](std::size_t offset, InsertionAffinity insertion) -> std::optional<std::size_t> {
        std::optional<std::size_t> low;
        std::optional<std::size_t> high;
        const auto add = [&](std::size_t candidate) {
            low = low ? std::min(*low, candidate) : candidate;
            high = high ? std::max(*high, candidate) : candidate;
        };
        for (const auto &passage : passages) {
            if (offset < passage.source.begin || offset > passage.source.end) continue;
            if (passage.kind == PassageKind::Equal) {
                add(passage.result.begin + offset - passage.source.begin);
            } else {
                if (offset == passage.source.begin) add(passage.result.begin);
                if (offset == passage.source.end) add(passage.result.end);
                // Only lines pair. An empty insertion anchor inside a changed
                // passage still has no verified position.
                if (pairing && !empty && passage.source.begin < offset && offset < passage.source.end
                    && (passage.kind == PassageKind::Changed || passage.kind == PassageKind::SourceOnly))
                    if (const auto paired = pairing->position(passage, offset)) add(*paired);
            }
        }
        return insertion == InsertionAffinity::Before ? low : high;
    };
    const auto begin = position(range.begin, empty ? affinity : InsertionAffinity::After);
    const auto end = position(range.end, empty ? affinity : InsertionAffinity::Before);
    if (!begin || !end || *begin > *end) return projection;
    projection.state = AnchorState::Mapped;
    projection.range = ByteRange{*begin, *end};
    return projection;
}

void invalidate(ResultAnchor &anchor, AnchorState state)
{
    anchor.state = state;
    anchor.result.reset();
}

// Ranges in text order, with overlapping or touching ones joined.
std::vector<ByteRange> joined(std::vector<ByteRange> ranges)
{
    std::sort(ranges.begin(), ranges.end(), [](const auto &a, const auto &b) {
        return a.begin != b.begin ? a.begin < b.begin : a.end < b.end;
    });
    std::vector<ByteRange> result;
    for (const auto &range : ranges) {
        if (!result.empty() && range.begin <= result.back().end)
            result.back().end = std::max(result.back().end, range.end);
        else result.push_back(range);
    }
    return result;
}

// Hand-edited ranges after `edit` replaced its range with `inserted` bytes.
// The replaced text leaves them, and a hand edit adds the text it wrote.
std::vector<ByteRange> editedRanges(const std::vector<ByteRange> &ranges, ByteRange edit,
                                    std::size_t inserted, bool manual)
{
    const auto shift = [&](std::size_t offset) { return offset - (edit.end - edit.begin) + inserted; };
    std::vector<ByteRange> result;
    for (const auto &range : ranges) {
        if (range.begin == range.end) {
            if (range.begin <= edit.begin) result.push_back(range);
            else if (range.begin >= edit.end) result.push_back({shift(range.begin), shift(range.end)});
            continue;
        }
        if (range.begin < edit.begin) result.push_back({range.begin, std::min(range.end, edit.begin)});
        if (range.end > edit.end) result.push_back({shift(std::max(range.begin, edit.end)), shift(range.end)});
    }
    if (manual) result.push_back({edit.begin, edit.begin + inserted});
    return joined(std::move(result));
}

// Hand-edited ranges after a whole-buffer update with these old-to-new
// passages. A range edge inside changed text widens to cover it, and every
// changed passage is a hand edit too.
std::vector<ByteRange> recomputedRanges(const std::vector<ByteRange> &ranges,
                                        const std::vector<Passage> &changes, std::size_t size)
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
    for (const auto &range : ranges) result.push_back({moved(range.begin, false), moved(range.end, true)});
    for (const auto &passage : changes)
        if (passage.kind != PassageKind::Equal) result.push_back(passage.result);
    return joined(std::move(result));
}

// Insertions at the edges of a nonempty range stay outside it, as in editing.
bool within(const SourceAnchor &outer, const SourceAnchor &inner)
{
    const auto &a = outer.range;
    const auto &b = inner.range;
    return outer.side == inner.side && (b.begin == b.end ? a.begin < b.begin && b.begin < a.end
        : a.begin <= b.begin && b.end <= a.end);
}

// Another nonempty contribution from the chosen side, such as the next surplus
// line at the same insertion point, keeps its own place beside the choice.
bool sibling(const SourceAnchor &chosen, const SourceAnchor &other)
{
    const auto &a = chosen.range;
    const auto &b = other.range;
    return chosen.side == other.side && a.begin != a.end && b.begin != b.end
        && (a.end <= b.begin || b.end <= a.begin);
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
        if (position >= edit.range.end && position != edit.range.begin) position = shift(position);
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
        if (manual) anchor.handEdited = true;
        else if (edit.range.begin <= range.begin && range.end <= edit.range.end) anchor.handEdited = false;
        invalidate(anchor, AnchorState::Invalidated);
        return;
    }
    anchor.result = range;
}
}

bool validUtf8(std::string_view text) noexcept
{
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first <= 0x7fU) continue;
        std::size_t count = 0;
        std::uint32_t value = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2U && first <= 0xdfU) { count = 1; value = first & 0x1fU; minimum = 0x80; }
        else if (first >= 0xe0U && first <= 0xefU) { count = 2; value = first & 0x0fU; minimum = 0x800; }
        else if (first >= 0xf0U && first <= 0xf4U) { count = 3; value = first & 0x07U; minimum = 0x10000; }
        else return false;
        if (count > text.size() - i) return false;
        for (std::size_t n = 0; n < count; ++n) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0U) != 0x80U) return false;
            value = (value << 6U) | (next & 0x3fU);
        }
        if (value < minimum || value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU)) return false;
    }
    return true;
}

bool validByteRange(std::string_view text, ByteRange range) noexcept
{
    return range.begin <= range.end && boundary(text, range.begin) && boundary(text, range.end);
}

std::vector<Passage> ExactLineCorrespondence::compare(std::string_view source, std::string_view result) const
{
    if (!validUtf8(source) || !validUtf8(result))
        throw std::invalid_argument("Correspondence requires valid UTF-8 text");
    const auto sourceBudget = assessText(source);
    const auto resultBudget = assessText(result);
    if (sourceBudget.limit != TextLimit::Within)
        throw std::invalid_argument(textLimitDiagnostic(sourceBudget));
    if (resultBudget.limit != TextLimit::Within)
        throw std::invalid_argument(textLimitDiagnostic(resultBudget));
    if (source == result) return {{{0, source.size()}, {0, result.size()}, PassageKind::Equal}};
    std::map<std::string_view, std::size_t, std::less<>> tokens;
    const auto a = lines(source, tokens);
    const auto b = lines(result, tokens);
    std::vector<LineMatch> first;
    std::vector<LineMatch> last;
    boundaryAlignment(a, b, 0, 0, false, first);
    boundaryAlignment(a, b, 0, 0, true, last);
    std::vector<Passage> passages;
    const auto emit = [&](Passage passage) {
        if (!passages.empty() && passage.kind == PassageKind::Equal
            && passages.back().kind == PassageKind::Equal
            && passages.back().source.end == passage.source.begin
            && passages.back().result.end == passage.result.begin) {
            passages.back().source.end = passage.source.end;
            passages.back().result.end = passage.result.end;
        } else passages.push_back(passage);
    };
    const auto sourceOffset = [&](std::size_t i) { return i == a.size() ? source.size() : a[i].begin; };
    const auto resultOffset = [&](std::size_t j) { return j == b.size() ? result.size() : b[j].begin; };
    std::size_t sourceStart = 0;
    std::size_t resultStart = 0;
    const auto gap = [&](std::size_t sourceEnd, std::size_t resultEnd, bool ambiguous) {
        if (sourceStart == sourceEnd && resultStart == resultEnd) return;
        const auto kind = ambiguous ? PassageKind::Ambiguous
            : sourceStart == sourceEnd ? PassageKind::ResultOnly
            : resultStart == resultEnd ? PassageKind::SourceOnly : PassageKind::Changed;
        emit({{sourceOffset(sourceStart), sourceOffset(sourceEnd)},
              {resultOffset(resultStart), resultOffset(resultEnd)}, kind});
    };
    std::size_t nextMatch = 0;
    for (std::size_t k = 0; k < first.size(); ++k) {
        if (first[k] != last[k]) continue;
        const auto [i, j] = first[k];
        gap(i, j, nextMatch != k);
        emit({{a[i].begin, a[i].end}, {b[j].begin, b[j].end}, PassageKind::Equal});
        sourceStart = i + 1;
        resultStart = j + 1;
        nextMatch = k + 1;
    }
    gap(a.size(), b.size(), nextMatch != first.size());
    return passages;
}

CorrespondenceMap::CorrespondenceMap(RevisionId revision, const FileSnapshots &sources,
                                     std::string result,
                                     std::shared_ptr<const CorrespondenceAlgorithm> algorithm)
    : revision_(revision), sessionId_(sources.provenance.sessionId), path_(sources.path),
      algorithm_(algorithm ? std::move(algorithm)
        : std::make_shared<ExactLineCorrespondence>()), text_(std::move(result))
{
    if (!validUtf8(text_)) throw std::invalid_argument("Correspondence requires valid UTF-8 text");
    std::map<SourceSide, std::string> texts;
    const auto capture = [&](SourceSide side, const std::optional<SourceSnapshot> &snapshot) {
        if (snapshot) {
            texts.emplace(side, snapshot->worktree.utf8);
            sourceObjectIds_.emplace(side, snapshot->objectId);
        }
    };
    capture(SourceSide::Earlier, sources.earlier);
    capture(SourceSide::Stage2, sources.stage2);
    capture(SourceSide::Stage3, sources.stage3);
    std::map<SourceSide, std::vector<std::size_t>> starts;
    for (const auto &[side, text] : texts) starts.emplace(side, lineStarts(text));
    sources_ = std::make_shared<const std::map<SourceSide, std::string>>(std::move(texts));
    sourceLines_ = std::make_shared<const std::map<SourceSide, std::vector<std::size_t>>>(std::move(starts));
    align();
}

void CorrespondenceMap::align()
{
    resultLines_.reset();
    for (const auto &[side, source] : *sources_) passages_[side] = algorithm_->compare(source, text_);
}

const std::vector<Passage> *CorrespondenceMap::passages(SourceSide side) const noexcept
{
    const auto found = passages_.find(side);
    return found == passages_.end() ? nullptr : &found->second;
}

const std::string *CorrespondenceMap::source(SourceSide side) const noexcept
{
    const auto found = sources_->find(side);
    return found == sources_->end() ? nullptr : &found->second;
}

const ResultAnchor *CorrespondenceMap::anchor(const std::string &id) const noexcept
{
    const auto found = anchors_.find(id);
    return found == anchors_.end() ? nullptr : &found->second;
}

bool CorrespondenceMap::bind(SourceAnchor source)
{
    const auto text = sources_->find(source.side);
    if (source.contributionId.empty() || text == sources_->end()
        || !validByteRange(text->second, source.range) || anchors_.contains(source.contributionId)) return false;
    if (!resultLines_) resultLines_ = std::make_shared<const std::vector<std::size_t>>(lineStarts(text_));
    const LinePairing pairing{sourceLines_->at(source.side), *resultLines_};
    const auto projected = project(passages_.at(source.side), source.range, source.affinity, &pairing);
    const auto id = source.contributionId;
    const AnchorOrigin origin{sessionId_, path_, sourceObjectIds_.at(source.side)};
    // Launch text that differs from its source needs confirmation to replace,
    // but nobody has edited it by hand yet.
    anchors_.emplace(id, ResultAnchor{origin, std::move(source), revision_, projected.state,
        projected.range, projected.changed && projected.range && projected.range->begin != projected.range->end,
        false});
    return true;
}

void CorrespondenceMap::settle(SourceSide side, const std::vector<ByteRange> &dropped)
{
    const auto source = sources_->find(side);
    if (source == sources_->end() || std::none_of(anchors_.begin(), anchors_.end(), [&](const auto &entry) {
            return entry.second.source.side == side && entry.second.state == AnchorState::Ambiguous;
        })) return;
    auto cuts = dropped;
    std::sort(cuts.begin(), cuts.end(), [](const auto &a, const auto &b) { return a.begin < b.begin; });
    // The result's kept pieces in order, each with where it starts in the view.
    std::vector<std::pair<std::size_t, ByteRange>> pieces;
    std::string view;
    std::size_t from = 0;
    const auto keep = [&](std::size_t end) {
        if (from >= end) return;
        pieces.push_back({view.size(), {from, end}});
        view.append(text_, from, end - from);
    };
    for (const auto &cut : cuts) {
        if (cut.begin < from || cut.end < cut.begin || text_.size() < cut.end
            || !validByteRange(text_, cut)) throw std::invalid_argument("Dropped result ranges must be disjoint");
        if (cut.begin == cut.end) continue;
        keep(cut.begin);
        from = cut.end;
    }
    keep(text_.size());
    // A view range in the result. Nonempty text lies inside one piece. An
    // empty position next to dropped text could be on either side of it.
    const auto place = [&](ByteRange range) -> std::optional<ByteRange> {
        std::vector<std::size_t> found;
        if (range.begin == 0 && (pieces.empty() || pieces.front().second.begin != 0)) found.push_back(0);
        if (range.begin == view.size() && (pieces.empty() || pieces.back().second.end != text_.size()))
            found.push_back(text_.size());
        for (const auto &[at, piece] : pieces) {
            const auto end = at + piece.end - piece.begin;
            if (range.begin == range.end ? at <= range.begin && range.begin <= end
                    : at <= range.begin && range.end <= end)
                found.push_back(piece.begin + range.begin - at);
        }
        if (found.size() != 1) return std::nullopt;
        return ByteRange{found.front(), found.front() + range.end - range.begin};
    };
    const auto passages = algorithm_->compare(source->second, view);
    const auto viewLines = lineStarts(view);
    const LinePairing pairing{sourceLines_->at(side), viewLines};
    for (auto &entry : anchors_) {
        auto &anchor = entry.second;
        if (anchor.source.side != side || anchor.state != AnchorState::Ambiguous) continue;
        const auto projected = project(passages, anchor.source.range, anchor.source.affinity, &pairing);
        const auto range = projected.range ? place(*projected.range) : std::nullopt;
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

CorrespondenceMap CorrespondenceMap::edited(RevisionId revision, const ResultEdit &edit,
                                           bool manual, const std::string &selectedAnchor) const
{
    if (!validByteRange(text_, edit.range) || !validUtf8(edit.replacement))
        throw std::invalid_argument("Result edit must use UTF-8 character boundaries");
    auto result = atRevision(revision);
    result.text_.replace(edit.range.begin, edit.range.end - edit.range.begin, edit.replacement);
    const auto *selected = manual ? nullptr : anchor(selectedAnchor);
    std::optional<std::vector<std::size_t>> lines;
    // A competing line loses its range when the result line it pairs with is
    // deleted by hand. Where its source pairs in the text before this choice
    // tells whether the choice replaced that place.
    const auto replaced = [&](const SourceAnchor &source) {
        if (edit.range.begin == edit.range.end) return false;
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
        transform(value, edit, manual, id == selectedAnchor, selected && sibling(selected->source, value.source));
    }
    result.handEditedRanges_ = editedRanges(handEditedRanges_, edit.range, edit.replacement.size(), manual);
    result.align();
    return result;
}

CorrespondenceMap CorrespondenceMap::rebuilt(RevisionId revision, ByteRange range,
                                            const std::vector<KeptSpan> &spans) const
{
    if (!validByteRange(text_, range)) throw std::invalid_argument("A rebuilt range must use UTF-8 character boundaries");
    std::string replacement;
    // Where each span's kept text starts in the new text.
    std::vector<std::size_t> starts;
    for (const auto &kept : spans) {
        if (!validByteRange(text_, kept.span) || kept.span.begin < range.begin || range.end < kept.span.end)
            throw std::invalid_argument("Kept text must lie inside the rebuilt range");
        starts.push_back(range.begin + replacement.size());
        auto from = kept.span.begin;
        for (const auto &cut : kept.dropped) {
            if (!validByteRange(text_, cut) || cut.begin < from || kept.span.end < cut.end)
                throw std::invalid_argument("Dropped text must lie in order inside its span");
            replacement.append(text_, from, cut.begin - from);
            from = cut.end;
        }
        replacement.append(text_, from, kept.span.end - from);
    }
    // The new offset of `offset` in span `i`, which no dropped range encloses.
    const auto place = [&](std::size_t i, std::size_t offset) {
        auto moved = starts[i] + offset - spans[i].span.begin;
        for (const auto &cut : spans[i].dropped)
            if (cut.end <= offset) moved -= cut.end - cut.begin;
        return moved;
    };
    const auto overlaps = [](ByteRange a, ByteRange b) { return a.begin < b.end && b.begin < a.end; };
    // The anchor's range in the new text, empty for one on exactly a dropped range.
    const auto relocated = [&](ByteRange old) -> std::optional<ByteRange> {
        for (std::size_t i = 0; i < spans.size(); ++i) {
            const auto &kept = spans[i];
            if (old.begin < kept.span.begin || kept.span.end < old.end) continue;
            const auto cut = std::find_if(kept.dropped.begin(), kept.dropped.end(), [&](const auto &dropped) {
                return dropped == old || (old.begin == old.end ? dropped.begin < old.begin && old.begin < dropped.end
                    : overlaps(dropped, old) && (dropped.begin < old.begin || old.end < dropped.end));
            });
            if (cut == kept.dropped.end()) return ByteRange{place(i, old.begin), place(i, old.end)};
            if (*cut == old) return ByteRange{place(i, old.begin), place(i, old.begin)};
            return std::nullopt;
        }
        return std::nullopt;
    };
    auto result = atRevision(revision);
    const auto shift = [&](std::size_t offset) { return offset - (range.end - range.begin) + replacement.size(); };
    for (auto &[id, anchor] : result.anchors_) {
        (void)id;
        if (anchor.state != AnchorState::Mapped || !anchor.result) continue;
        const auto old = *anchor.result;
        if (old.begin == old.end && old.begin == range.begin) {
            const auto position = range.begin + (anchor.source.affinity == InsertionAffinity::After ? replacement.size() : 0);
            anchor.result = ByteRange{position, position};
        } else if (old.end <= range.begin && old.begin < range.begin) {
            continue;
        } else if (old.begin >= range.end) {
            anchor.result = ByteRange{shift(old.begin), shift(old.end)};
        } else if (const auto moved = relocated(old)) {
            if (moved->begin == moved->end && old.begin != old.end) anchor.handEdited = false;
            anchor.result = moved;
        } else {
            invalidate(anchor, AnchorState::Invalidated);
        }
    }
    // Hand edits keep the text the spans keep. The rest of the range is the choice's.
    std::vector<ByteRange> edited;
    const auto keptPieces = [&](std::size_t i, ByteRange inside) {
        auto from = std::max(inside.begin, spans[i].span.begin);
        const auto to = std::min(inside.end, spans[i].span.end);
        for (const auto &cut : spans[i].dropped) {
            if (from >= to || cut.begin >= to) break;
            if (cut.end <= from) continue;
            if (from < cut.begin) edited.push_back({place(i, from), place(i, cut.begin)});
            from = cut.end;
        }
        if (from < to) edited.push_back({place(i, from), place(i, to)});
    };
    const auto keptPosition = [&](std::size_t position) {
        for (std::size_t i = 0; i < spans.size(); ++i) {
            const auto &kept = spans[i];
            if (position < kept.span.begin || kept.span.end < position) continue;
            if (std::none_of(kept.dropped.begin(), kept.dropped.end(), [&](const auto &cut) {
                    return cut.begin < position && position < cut.end;
                })) {
                edited.push_back({place(i, position), place(i, position)});
                return;
            }
        }
    };
    for (const auto &old : handEditedRanges_) {
        if (old.begin == old.end) {
            if (old.begin <= range.begin) edited.push_back(old);
            else if (old.begin >= range.end) edited.push_back({shift(old.begin), shift(old.end)});
            else keptPosition(old.begin);
            continue;
        }
        if (old.begin < range.begin) edited.push_back({old.begin, std::min(old.end, range.begin)});
        if (old.end > range.end) edited.push_back({shift(std::max(old.begin, range.end)), shift(old.end)});
        const ByteRange inside{std::max(old.begin, range.begin), std::min(old.end, range.end)};
        for (std::size_t i = 0; inside.begin < inside.end && i < spans.size(); ++i) keptPieces(i, inside);
    }
    result.handEditedRanges_ = joined(std::move(edited));
    result.text_.replace(range.begin, range.end - range.begin, replacement);
    result.align();
    return result;
}

CorrespondenceMap CorrespondenceMap::recomputed(RevisionId revision, std::string text, bool manual) const
{
    auto result = atRevision(revision);
    if (!manual) {
        for (auto &[id, anchor] : result.anchors_) {
            (void)id;
            anchor.handEdited = false;
        }
        result.handEditedRanges_.clear();
    }
    if (text_ == text) return result;
    const auto changes = algorithm_->compare(text_, text);
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
    result.align();
    return result;
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
