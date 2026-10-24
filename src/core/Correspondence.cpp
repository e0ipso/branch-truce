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

struct Projection {
    AnchorState state = AnchorState::Ambiguous;
    std::optional<ByteRange> range;
    bool changed = false;
};

Projection project(const std::vector<Passage> &passages, ByteRange range,
                   InsertionAffinity affinity)
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
            }
        }
        return insertion == InsertionAffinity::Before ? low : high;
    };
    const bool empty = range.begin == range.end;
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

void transform(ResultAnchor &anchor, const ResultEdit &edit, bool manual, bool selected)
{
    if (anchor.state != AnchorState::Mapped || !anchor.result) return;
    auto range = *anchor.result;
    const auto removed = edit.range.end - edit.range.begin;
    const auto inserted = edit.replacement.size();
    const auto shift = [&](std::size_t offset) { return offset - removed + inserted; };
    if (selected || (!manual && range == edit.range)) {
        anchor.result = ByteRange{edit.range.begin, edit.range.begin + inserted};
        if (selected) anchor.manualOverlap = false;
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
        if (manual && range.begin == range.end) {
            invalidate(anchor, AnchorState::Invalidated);
            return;
        }
    } else {
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
    sources_ = std::make_shared<const std::map<SourceSide, std::string>>(std::move(texts));
    align();
}

void CorrespondenceMap::align()
{
    for (const auto &[side, source] : *sources_) passages_[side] = algorithm_->compare(source, text_);
}

const std::vector<Passage> *CorrespondenceMap::passages(SourceSide side) const noexcept
{
    const auto found = passages_.find(side);
    return found == passages_.end() ? nullptr : &found->second;
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
    const auto projected = project(passages_.at(source.side), source.range, source.affinity);
    const auto id = source.contributionId;
    const AnchorOrigin origin{sessionId_, path_, sourceObjectIds_.at(source.side)};
    anchors_.emplace(id, ResultAnchor{origin, std::move(source), revision_, projected.state,
        projected.range, projected.changed && projected.range && projected.range->begin != projected.range->end});
    return true;
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
    for (auto &[id, anchor] : result.anchors_) transform(anchor, edit, manual, id == selectedAnchor);
    result.align();
    return result;
}

CorrespondenceMap CorrespondenceMap::recomputed(RevisionId revision, std::string text) const
{
    if (text_ == text) return atRevision(revision);
    const auto changes = algorithm_->compare(text_, text);
    auto result = atRevision(revision);
    for (auto &[id, anchor] : result.anchors_) {
        (void)id;
        if (anchor.state != AnchorState::Mapped || !anchor.result) continue;
        const auto old = *anchor.result;
        const auto projected = project(changes, old, anchor.source.affinity);
        anchor.state = projected.state;
        anchor.result = projected.range;
        anchor.manualOverlap = anchor.manualOverlap || projected.changed;
        if (old.begin != old.end && projected.range && projected.range->begin == projected.range->end)
            invalidate(anchor, AnchorState::Invalidated);
    }
    result.text_ = std::move(text);
    result.align();
    return result;
}

}
