// Shared size limits bound the files, Git output and recovery payloads the app supports.
#pragma once

#include <cstddef>
#include <string_view>

namespace branch_truce {

// Supported envelope recorded in tests/fixtures/performance/evidence.md.
// Representative files are 4000 lines. The longest line is 262144 bytes.
// Git process output is 4194304 bytes. A recovery payload is 8388608 bytes.
// The line count is not a hard limit: a file with more lines than the measured
// size opens only after the user confirms it, because editing it may be slow.
inline constexpr std::size_t kLargeFileLines = 4000;
inline constexpr std::size_t kMaxLineBytes = 262144;
inline constexpr std::size_t kMaxGitReadBytes = 4194304;
inline constexpr std::size_t kMaxRecoveryPayloadBytes = 8388608;
// Longest conflict marker the engine recognizes. Git's default is 7, and
// projects that change it with conflict-marker-size use a few dozen.
inline constexpr std::size_t kMaxConflictMarkerSize = 1024;
// Text the undo history of one file may hold. Each revision counts its result
// text and its correspondence's copy of it. Older undo steps go first, so
// even a file at the recovery payload limit keeps about 16 of them.
inline constexpr std::size_t kMaxHistoryBytes = 268435456;

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
