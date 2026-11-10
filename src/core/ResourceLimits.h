// Shared size limits bound the files, Git output and recovery payloads the app supports.
#pragma once

#include "ByteRanges.h"
#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace branch_truce {

// Supported envelope recorded in tests/fixtures/performance/evidence.md.
// Representative files are 4000 lines. The longest line is 262144 bytes.
// Git process output is 4194304 bytes. A recovery payload is 8388608 bytes.
// More than the representative line count needs confirmation. Independent
// index and metadata budgets still apply after that confirmation.
inline constexpr std::size_t kLargeFileLines = 4000;
inline constexpr std::size_t kMaxLineBytes = 262144;
inline constexpr std::size_t kMaxGitReadBytes = 4194304;
inline constexpr std::size_t kMaxRecoveryPayloadBytes = 8388608;
// Longest conflict marker the engine recognizes. Git's default is 7, and
// projects that change it with conflict-marker-size use a few dozen.
inline constexpr std::size_t kMaxConflictMarkerSize = 1024;
// Line pairs one alignment of a source with a result may compare: the
// product of their line counts. The line alignment takes time in proportion
// to it, so a larger pair is one Ambiguous passage instead: its anchors stay
// unmapped and only direct editing applies there. This allows 8192 lines
// against 8192, twice the representative size in each text.
inline constexpr std::size_t kMaxAlignmentCells = 67108864;
inline constexpr std::size_t kMaxIndexedLines = 65536;
// Derived choices, anchors and decision identifiers have their own admission
// budget. Bytes are a conservative estimate, not a process-memory quota.
inline constexpr std::size_t kMaxMetadataEntries = 65536;
inline constexpr std::size_t kMaxMetadataBytes = 16777216;
struct MetadataBudget {
    std::size_t entries = kMaxMetadataEntries;
    std::size_t bytes = kMaxMetadataBytes;
    bool take(std::size_t count, std::size_t bytesEach) noexcept
    {
        if (count > entries || (count && bytesEach > bytes / count)) return false;
        entries -= count;
        bytes -= count * bytesEach;
        return true;
    }
};
// Retained text and metadata estimate, including container capacity and node
// overhead. Protected revisions survive both limits; oldest undo steps go first.
inline constexpr std::size_t kMaxHistoryBytes = 268435456;
inline constexpr std::size_t kMaxHistoryEntries = 1024;

enum class TextLimit { Within, LineBytes, RecoveryPayload };

struct TextAssessment {
    TextLimit limit = TextLimit::Within;
    std::size_t lines = 0;
    std::size_t maxLineBytes = 0;
    std::size_t bytes = 0;
    bool operator==(const TextAssessment &) const = default;
};

// Stops once a limit is exceeded so an oversized buffer is not scanned further.
inline TextAssessment assessText(std::string_view text) noexcept
{
    TextAssessment assessment;
    assessment.bytes = text.size();
    const std::size_t bound = text.size() > kMaxRecoveryPayloadBytes ? kMaxRecoveryPayloadBytes + 1 : text.size();
    std::size_t current = 0;
    for (std::size_t i = 0; i < bound; ++i) {
        const char c = text[i];
        ++current;
        if (current > kMaxLineBytes) {
            assessment.maxLineBytes = current;
            assessment.limit = TextLimit::LineBytes;
            return assessment;
        }
        if (c == '\n') {
            ++assessment.lines;
            if (current > assessment.maxLineBytes) assessment.maxLineBytes = current;
            current = 0;
        }
    }
    if (current > 0) {
        ++assessment.lines;
        if (current > assessment.maxLineBytes) assessment.maxLineBytes = current;
    }
    if (assessment.limit == TextLimit::Within && text.size() > kMaxRecoveryPayloadBytes)
        assessment.limit = TextLimit::RecoveryPayload;
    return assessment;
}

// Check discontiguous candidate text before copying any of its parts.
inline bool partsFit(std::span<const std::string_view> parts) noexcept
{
    std::size_t bytes = 0;
    std::size_t column = 0;
    std::size_t lines = 0;
    for (const auto part : parts) {
        if (part.size() > kMaxRecoveryPayloadBytes - bytes) return false;
        bytes += part.size();
        for (const char c : part) {
            if (++column > kMaxLineBytes) return false;
            if (c == '\n') {
                if (++lines > kMaxIndexedLines) return false;
                column = 0;
            }
        }
    }
    return lines + (column != 0) <= kMaxIndexedLines;
}

inline bool replacementFits(std::string_view text, ByteRange range, std::string_view replacement) noexcept
{
    return partsFit(std::array{text.substr(0, range.begin), replacement, text.substr(range.end)});
}

inline bool withinTextEnvelope(std::string_view text) noexcept { return assessText(text).limit == TextLimit::Within; }

// Counts lines the way assessText does: a final line without a newline counts.
inline std::size_t lineCount(std::string_view text) noexcept
{
    std::size_t lines = 0;
    for (const char c : text)
        if (c == '\n') ++lines;
    return lines + (!text.empty() && text.back() != '\n' ? 1 : 0);
}

}
