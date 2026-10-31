// Passage correspondence maps captured source ranges to one revision of the editable result.
#pragma once

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

// All ranges are half-open UTF-8 byte offsets, including empty insertion sites.
struct ByteRange {
    std::size_t begin = 0;
    std::size_t end = 0;
    bool operator==(const ByteRange &) const = default;
};

enum class SourceSide { Earlier, Stage2, Stage3 };
enum class InsertionAffinity { Before, After };
enum class PassageKind { Equal, Changed, SourceOnly, ResultOnly, Ambiguous };

struct Passage {
    ByteRange source;
    ByteRange result;
    PassageKind kind;
    bool operator==(const Passage &) const = default;
};

// Implementations return an ordered, complete partition of both texts. Changed
// gaps have verified boundaries; uncertain alignment is explicitly Ambiguous.
class CorrespondenceAlgorithm {
public:
    virtual ~CorrespondenceAlgorithm() = default;
    [[nodiscard]] virtual std::vector<Passage> compare(
        std::string_view source, std::string_view result) const = 0;
};

// Exact line LCS, retaining only matches shared by every optimal alignment.
// See Correspondence.md for the cost, conservative range policy and evidence.
class ExactLineCorrespondence final : public CorrespondenceAlgorithm {
public:
    [[nodiscard]] std::vector<Passage> compare(
        std::string_view source, std::string_view result) const override;
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
class CorrespondenceMap {
public:
    CorrespondenceMap(RevisionId revision, const FileSnapshots &sources, std::string result,
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
    [[nodiscard]] CorrespondenceMap edited(RevisionId revision, const ResultEdit &edit,
                                         bool manual = true,
                                         const std::string &selectedAnchor = {}) const;
    // A choice that replaces `range` with the text its `spans` keep, in the
    // given order. An anchor inside a span keeps its place in that span's text,
    // and one on exactly a dropped range is left empty where that range was.
    // Any other anchor inside `range` or across its edges is invalidated.
    [[nodiscard]] CorrespondenceMap rebuilt(RevisionId revision, ByteRange range,
                                          const std::vector<KeptSpan> &spans) const;
    // Whole-buffer editor updates lack an edit location. Transform only where
    // every optimal old-to-new alignment agrees, otherwise retain ambiguity.
    // A whole-file command passes manual false and clears every hand edit.
    [[nodiscard]] CorrespondenceMap recomputed(RevisionId revision, std::string result,
                                             bool manual = true) const;
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
    RevisionId revision_;
    std::string sessionId_;
    std::string path_;
    std::map<SourceSide, std::string> sourceObjectIds_;
    std::shared_ptr<const std::map<SourceSide, std::string>> sources_;
    std::shared_ptr<const CorrespondenceAlgorithm> algorithm_;
    std::string text_;
    std::map<SourceSide, std::vector<Passage>> passages_;
    // Line starts that pair lines inside changed passages when binding. The
    // result's are built by the first bind at a text and dropped by align().
    std::shared_ptr<const std::map<SourceSide, std::vector<std::size_t>>> sourceLines_;
    std::shared_ptr<const std::vector<std::size_t>> resultLines_;
    std::map<std::string, ResultAnchor> anchors_;
    std::vector<ByteRange> handEditedRanges_;
};

}
