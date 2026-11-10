// Passage correspondence maps captured source ranges to one revision of the editable result.
#pragma once

#include "ByteRanges.h"
#include "FileSnapshot.h"
#include "RevisionId.h"
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace branch_truce {

enum class InsertionAffinity { Before, After };
enum class PassageKind { Equal, Changed, SourceOnly, ResultOnly, Ambiguous };

struct Passage {
    ByteRange source;
    ByteRange result;
    PassageKind kind;
    bool operator==(const Passage &) const = default;
};

// Aligns a source text with a result text. CorrespondenceMap, the choice
// catalogue and FileSession call it through this interface, so a session can
// be given another implementation.
//
// The contract every implementation keeps:
// - compare is called only with valid UTF-8 inside the text envelope of
//   ResourceLimits.h. CorrespondenceMap and FileSession check both first.
// - The passages partition both texts completely and in order: each starts
//   where the previous one ended, and the last ends at each text's end.
// - An Equal passage covers identical bytes on both sides. Where alignments
//   that are equally good disagree, the passage is Ambiguous, never a guess.
// - compare is deterministic, keeps no state between calls and may run on
//   several threads at once.
// - It fails only by throwing std::bad_alloc.
// - identity() names it. Choice and decision IDs come from the passages it
//   returns, so recovery journals record the name and replay refuses a
//   journal another algorithm wrote. Two implementations, or two versions of
//   one, that can return different passages for the same texts have
//   different names.
class CorrespondenceAlgorithm {
public:
    virtual ~CorrespondenceAlgorithm() = default;
    [[nodiscard]] virtual std::vector<Passage> compare(std::string_view source, std::string_view result) const = 0;
    [[nodiscard]] virtual std::string_view identity() const noexcept = 0;
};

// Exact line LCS, retaining only matches shared by every optimal alignment.
// Texts whose line counts multiply to more than kMaxAlignmentCells are one
// Ambiguous passage. See Correspondence.md for the cost, conservative range
// policy and evidence.
// Called directly, it also rejects text outside the contract's inputs with
// std::invalid_argument.
class ExactLineCorrespondence final : public CorrespondenceAlgorithm {
public:
    [[nodiscard]] std::vector<Passage> compare(std::string_view source, std::string_view result) const override;
    [[nodiscard]] std::string_view identity() const noexcept override { return "exact-line/1"; }
};

struct SourceAnchor {
    std::string contributionId;
    SourceSide side;
    ByteRange range;
    // A nonempty range excludes insertions at either edge. An empty range
    // follows this affinity when text is inserted at its exact position.
    InsertionAffinity affinity = InsertionAffinity::After;
    bool operator==(const SourceAnchor &) const = default;
};

enum class AnchorState { Mapped, Ambiguous, Invalidated };
struct AnchorOrigin {
    std::string sessionId;
    std::string path;
    std::string sourceObjectId;
    bool operator==(const AnchorOrigin &) const = default;
};
struct ResultAnchor {
    AnchorOrigin origin;
    SourceAnchor source;
    RevisionId revision;
    AnchorState state;
    // Absent when unsafe. The immutable source range remains navigable.
    std::optional<ByteRange> result;
    // The range may hold work other than its source, from launch or a later
    // edit, so replacing it needs confirmation.
    bool manualOverlap = false;
    // Only editor changes inside the range set this. A choice that replaces
    // the whole range clears it.
    bool handEdited = false;
    bool operator==(const ResultAnchor &) const = default;
};

struct ResultEdit {
    ByteRange range;
    std::string replacement;
    bool operator==(const ResultEdit &) const = default;
};

// When an edited map aligns its sources with the new text again: at once, or
// later through realigned(), so the caller can do that work on another thread.
enum class Realignment { Now, Deferred };

// Part of a range a choice rebuilds: `span` without the `dropped` ranges inside
// it, which are disjoint and in text order.
struct KeptSpan {
    ByteRange span;
    std::vector<ByteRange> dropped;
    bool operator==(const KeptSpan &) const = default;
};

[[nodiscard]] bool validUtf8(std::string_view text) noexcept;
[[nodiscard]] bool validByteRange(std::string_view text, ByteRange range) noexcept;

// A value snapshot, stored with its result transaction. Recomputing passage
// navigation never reattaches an issued anchor by searching for similar text.
//
// Every text it aligns is checked against the algorithm's input contract
// first; a text outside it throws std::invalid_argument before the algorithm
// runs. A map is immutable once built, bound and settled: copies share its
// source texts, and const members may run on several threads at once.
class CorrespondenceMap {
public:
    // Aligns each captured source with `result`. The map shares `sources`
    // instead of copying their text.
    CorrespondenceMap(RevisionId revision, std::shared_ptr<const FileSnapshots> sources, std::string result,
                      std::shared_ptr<const CorrespondenceAlgorithm> algorithm = {});

    [[nodiscard]] RevisionId revision() const noexcept { return revision_; }
    [[nodiscard]] const std::string &text() const noexcept { return text_; }
    // nullptr means unavailable; a present empty source has an alignment.
    [[nodiscard]] const std::vector<Passage> *passages(SourceSide side) const noexcept;
    // The source text those passages align, or nullptr when unavailable.
    [[nodiscard]] const std::string *source(SourceSide side) const noexcept;
    [[nodiscard]] const ResultAnchor *anchor(const std::string &id) const noexcept;
    [[nodiscard]] bool bind(SourceAnchor source);
    // Git copies a side's text of a conflict into that side's region only, so
    // the marker lines, base text and other side regions, passed as `dropped`,
    // hold none of it. Each ambiguous anchor of `side` is mapped where every
    // optimal alignment of its source with the result without them agrees, if
    // its range lies inside one stretch of kept text. An empty range at the
    // edge of dropped text stays ambiguous.
    void settle(SourceSide side, const std::vector<ByteRange> &dropped);
    [[nodiscard]] CorrespondenceMap atRevision(RevisionId revision) const;
    // The map after `edit`. Its anchors, hand edits and text follow the edit
    // at once. With Realignment::Deferred the passages are provisional
    // instead of aligned again: those before and after the edited lines keep
    // their alignment, shifted, and the edited lines with every passage they
    // touch become one Ambiguous passage. That costs time in proportion to the
    // passages, not to the product of line counts.
    [[nodiscard]] CorrespondenceMap edited(RevisionId revision, const ResultEdit &edit, bool manual = true,
                                           const std::string &selectedAnchor = {},
                                           Realignment realignment = Realignment::Now) const;
    // False while the passages are provisional, after a deferred edit.
    [[nodiscard]] bool aligned() const noexcept { return aligned_; }
    // This map with every source aligned with its text again: the map a
    // Realignment::Now edit would have built. Const and pure, so a worker
    // thread may run it while others read this map. Throws std::bad_alloc.
    [[nodiscard]] CorrespondenceMap realigned() const;
    // Whether `other` is this map with its passages aligned again: the same
    // revision, text, anchors and hand edits.
    [[nodiscard]] bool sameAnchors(const CorrespondenceMap &other) const;
    // A choice that replaces `range` with the text its `spans` keep, in the
    // given order. An anchor inside a span keeps its place in that span's text,
    // and one on exactly a dropped range is left empty where that range was.
    // Any other anchor inside `range` or across its edges is invalidated.
    [[nodiscard]] CorrespondenceMap rebuilt(RevisionId revision, ByteRange range,
                                            const std::vector<KeptSpan> &spans) const;
    // Whole-buffer editor updates lack an edit location. Transform only where
    // every optimal old-to-new alignment agrees, otherwise retain ambiguity.
    // A whole-file command passes manual false and clears every hand edit.
    [[nodiscard]] CorrespondenceMap recomputed(RevisionId revision, std::string text, bool manual = true) const;
    [[nodiscard]] std::set<std::string> handEditedAnchors() const;
    // Recovery replays whole drafts, which cannot tell typing from a choice,
    // so it restores the recorded flags. False if an anchor is unknown.
    [[nodiscard]] bool restoreHandEdits(const std::set<std::string> &anchors);
    // The result text editor changes wrote, inside choice regions or not, in
    // text order. Ranges neither overlap nor touch, and an empty one is where
    // text was deleted. A choice drops the text it replaces from them, and a
    // whole-file command clears them.
    [[nodiscard]] const std::vector<ByteRange> &handEditedRanges() const noexcept { return handEditedRanges_; }
    // Recovery restores the recorded ranges, as it does the anchor flags.
    // False unless they are in that form on character boundaries of text().
    [[nodiscard]] bool restoreHandEditedRanges(std::vector<ByteRange> ranges);

private:
    void align();
    // Provisional passages after `edit` replaced its range of the old text with
    // `inserted` bytes.
    void shiftPassages(std::string_view old, ByteRange edit, std::size_t inserted);
    RevisionId revision_;
    std::shared_ptr<const FileSnapshots> sources_;
    std::shared_ptr<const CorrespondenceAlgorithm> algorithm_;
    std::string text_;
    std::map<SourceSide, std::vector<Passage>> passages_;
    // Line starts that pair lines inside changed passages when binding. The
    // result's are built by the first bind at a text and dropped by align().
    std::shared_ptr<const std::map<SourceSide, std::vector<std::size_t>>> sourceLines_;
    std::shared_ptr<const std::vector<std::size_t>> resultLines_;
    std::map<std::string, ResultAnchor> anchors_;
    std::vector<ByteRange> handEditedRanges_;
    bool aligned_ = true;
};

}
