#pragma once

#include "FileSnapshot.h"
#include "RevisionId.h"
#include <map>
#include <memory>
#include <optional>
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
    bool manualOverlap = false;
    bool operator==(const ResultAnchor &) const = default;
};

struct ResultEdit {
    ByteRange range;
    std::string replacement;
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
    [[nodiscard]] const ResultAnchor *anchor(const std::string &id) const noexcept;
    [[nodiscard]] bool bind(SourceAnchor source);
    [[nodiscard]] CorrespondenceMap atRevision(RevisionId revision) const;
    [[nodiscard]] CorrespondenceMap edited(RevisionId revision, const ResultEdit &edit,
                                         bool manual = true,
                                         const std::string &selectedAnchor = {}) const;
    // Whole-buffer editor updates lack an edit location. Transform only where
    // every optimal old-to-new alignment agrees, otherwise retain ambiguity.
    [[nodiscard]] CorrespondenceMap recomputed(RevisionId revision, std::string result) const;

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
    std::map<std::string, ResultAnchor> anchors_;
};

}
