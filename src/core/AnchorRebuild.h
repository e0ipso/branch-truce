// Anchor rebuilds move result anchors and hand edits when a choice drops text or rebuilds a range.
#pragma once

#include "Correspondence.h"
#include "Utf8.h"
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Internal to CorrespondenceMap: settle() and rebuilt() use these.
namespace branch_truce::anchor_rebuild {

// Leaves the anchor without a result range.
inline void invalidate(ResultAnchor &anchor, AnchorState state)
{
    anchor.state = state;
    anchor.result.reset();
}

// A text without some disjoint ranges of it: its kept pieces in order, each
// with where it starts in the shorter text.
class KeptView {
public:
    KeptView(std::string_view text, std::vector<ByteRange> dropped) : size_(text.size())
    {
        std::sort(dropped.begin(), dropped.end(), [](const auto &a, const auto &b) { return a.begin < b.begin; });
        std::size_t from = 0;
        for (const auto &cut : dropped) {
            if (cut.begin < from || cut.end < cut.begin || text.size() < cut.end || !validByteRange(text, cut))
                throw std::invalid_argument("Dropped result ranges must be disjoint");
            if (cut.begin == cut.end) continue;
            keep(text, from, cut.begin);
            from = cut.end;
        }
        keep(text, from, text.size());
    }

    [[nodiscard]] const std::string &text() const noexcept { return view_; }

    // A range of the shorter text in the original one. Nonempty text lies
    // inside one piece. An empty position next to dropped text could be on
    // either side of it, so it has no place.
    [[nodiscard]] std::optional<ByteRange> place(ByteRange range) const
    {
        std::vector<std::size_t> found;
        if (range.begin == 0 && (pieces_.empty() || pieces_.front().second.begin != 0)) found.push_back(0);
        if (range.begin == view_.size() && (pieces_.empty() || pieces_.back().second.end != size_))
            found.push_back(size_);
        for (const auto &[at, piece] : pieces_) {
            const auto end = at + piece.end - piece.begin;
            if (range.begin == range.end ? at <= range.begin && range.begin <= end
                                         : at <= range.begin && range.end <= end)
                found.push_back(piece.begin + range.begin - at);
        }
        if (found.size() != 1) return std::nullopt;
        return ByteRange{found.front(), found.front() + range.end - range.begin};
    }

private:
    void keep(std::string_view text, std::size_t from, std::size_t end)
    {
        if (from >= end) return;
        pieces_.emplace_back(view_.size(), ByteRange{from, end});
        view_.append(text.substr(from, end - from));
    }

    std::size_t size_;
    std::vector<std::pair<std::size_t, ByteRange>> pieces_;
    std::string view_;
};

// A choice that rebuilds `range` from the text its spans keep, in their
// order. Offsets inside a kept span move with its text.
class Rebuild {
public:
    Rebuild(std::string_view text, ByteRange range, const std::vector<KeptSpan> &spans) : range_(range), spans_(spans)
    {
        if (!validByteRange(text, range))
            throw std::invalid_argument("A rebuilt range must use UTF-8 character boundaries");
        starts_.reserve(spans.size());
        for (const auto &kept : spans) {
            if (!validByteRange(text, kept.span) || !contains(range, kept.span))
                throw std::invalid_argument("Kept text must lie inside the rebuilt range");
            starts_.push_back(range.begin + replacement_.size());
            auto from = kept.span.begin;
            for (const auto &cut : kept.dropped) {
                if (!validByteRange(text, cut) || cut.begin < from || kept.span.end < cut.end)
                    throw std::invalid_argument("Dropped text must lie in order inside its span");
                replacement_.append(text.substr(from, cut.begin - from));
                from = cut.end;
            }
            replacement_.append(text.substr(from, kept.span.end - from));
        }
    }

    [[nodiscard]] const std::string &replacement() const noexcept { return replacement_; }

    // An anchor inside a span keeps its place in that span's text, and one on
    // exactly a dropped range is left empty where that range was. Any other
    // anchor inside the range or across its edges is invalidated.
    void move(ResultAnchor &anchor) const
    {
        if (anchor.state != AnchorState::Mapped || !anchor.result) return;
        const auto old = *anchor.result;
        if (old.begin == old.end && old.begin == range_.begin) {
            const auto position =
                range_.begin + (anchor.source.affinity == InsertionAffinity::After ? replacement_.size() : 0);
            anchor.result = ByteRange{position, position};
        } else if (old.end <= range_.begin && old.begin < range_.begin) {
            return;
        } else if (old.begin >= range_.end) {
            anchor.result = ByteRange{shift(old.begin), shift(old.end)};
        } else if (const auto moved = relocated(old)) {
            if (moved->begin == moved->end && old.begin != old.end) anchor.handEdited = false;
            anchor.result = moved;
        } else {
            invalidate(anchor, AnchorState::Invalidated);
        }
    }

    // Hand edits keep the text the spans keep. The rest of the range is the choice's.
    [[nodiscard]] std::vector<ByteRange> handEdits(const std::vector<ByteRange> &ranges) const
    {
        std::vector<ByteRange> edited;
        for (const auto &old : ranges) {
            if (old.begin == old.end) {
                if (old.begin <= range_.begin)
                    edited.push_back(old);
                else if (old.begin >= range_.end)
                    edited.push_back({shift(old.begin), shift(old.end)});
                else
                    keptPosition(edited, old.begin);
                continue;
            }
            if (old.begin < range_.begin) edited.push_back({old.begin, std::min(old.end, range_.begin)});
            if (old.end > range_.end) edited.push_back({shift(std::max(old.begin, range_.end)), shift(old.end)});
            const ByteRange inside{std::max(old.begin, range_.begin), std::min(old.end, range_.end)};
            for (std::size_t i = 0; inside.begin < inside.end && i < spans_.size(); ++i) keptPieces(edited, i, inside);
        }
        return mergeRanges(std::move(edited));
    }

private:
    [[nodiscard]] std::size_t shift(std::size_t offset) const
    {
        return offset - (range_.end - range_.begin) + replacement_.size();
    }

    // The new offset of `offset` in span `i`, which no dropped range encloses.
    [[nodiscard]] std::size_t place(std::size_t i, std::size_t offset) const
    {
        auto moved = starts_[i] + offset - spans_[i].span.begin;
        for (const auto &cut : spans_[i].dropped)
            if (cut.end <= offset) moved -= cut.end - cut.begin;
        return moved;
    }

    // The anchor's range in the new text, empty for one on exactly a dropped range.
    [[nodiscard]] std::optional<ByteRange> relocated(ByteRange old) const
    {
        for (std::size_t i = 0; i < spans_.size(); ++i) {
            const auto &kept = spans_[i];
            if (!contains(kept.span, old)) continue;
            const auto cut = std::find_if(kept.dropped.begin(), kept.dropped.end(), [&](const auto &dropped) {
                return dropped == old
                    || (old.begin == old.end
                            ? dropped.begin < old.begin && old.begin < dropped.end
                            : overlaps(dropped, old) && (dropped.begin < old.begin || old.end < dropped.end));
            });
            if (cut == kept.dropped.end()) return ByteRange{place(i, old.begin), place(i, old.end)};
            if (*cut == old) return ByteRange{place(i, old.begin), place(i, old.begin)};
            return std::nullopt;
        }
        return std::nullopt;
    }

    void keptPieces(std::vector<ByteRange> &edited, std::size_t i, ByteRange inside) const
    {
        auto from = std::max(inside.begin, spans_[i].span.begin);
        const auto to = std::min(inside.end, spans_[i].span.end);
        for (const auto &cut : spans_[i].dropped) {
            if (from >= to || cut.begin >= to) break;
            if (cut.end <= from) continue;
            if (from < cut.begin) edited.push_back({place(i, from), place(i, cut.begin)});
            from = cut.end;
        }
        if (from < to) edited.push_back({place(i, from), place(i, to)});
    }

    void keptPosition(std::vector<ByteRange> &edited, std::size_t position) const
    {
        for (std::size_t i = 0; i < spans_.size(); ++i) {
            const auto &kept = spans_[i];
            if (position < kept.span.begin || kept.span.end < position) continue;
            if (std::none_of(kept.dropped.begin(), kept.dropped.end(),
                             [&](const auto &cut) { return cut.begin < position && position < cut.end; })) {
                edited.push_back({place(i, position), place(i, position)});
                return;
            }
        }
    }

    ByteRange range_;
    const std::vector<KeptSpan> &spans_;
    std::string replacement_;
    std::vector<std::size_t> starts_;
};

}
