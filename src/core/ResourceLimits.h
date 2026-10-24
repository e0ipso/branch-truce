#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace branch_truce {

// Supported envelope recorded in tests/fixtures/performance/evidence.md.
// Representative files are 4000 lines. The longest line is 262144 bytes.
// Git process output is 4194304 bytes. A recovery payload is 8388608 bytes.
inline constexpr std::size_t kMaxFileLines = 4000;
inline constexpr std::size_t kMaxLineBytes = 262144;
inline constexpr std::size_t kMaxGitReadBytes = 4194304;
inline constexpr std::size_t kMaxRecoveryPayloadBytes = 8388608;

enum class TextLimit { Within, FileLines, LineBytes, RecoveryPayload };

struct TextAssessment {
    TextLimit limit = TextLimit::Within;
    std::size_t lines = 0;
    std::size_t maxLineBytes = 0;
    std::size_t bytes = 0;
};

// Stops once a limit is exceeded so an oversized buffer is not scanned further.
inline TextAssessment assessText(std::string_view text) noexcept
{
    TextAssessment assessment;
    assessment.bytes = text.size();
    const std::size_t bound = text.size() > kMaxRecoveryPayloadBytes
        ? kMaxRecoveryPayloadBytes + 1 : text.size();
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
            if (assessment.lines > kMaxFileLines) {
                assessment.limit = TextLimit::FileLines;
                return assessment;
            }
        }
    }
    if (current > 0) {
        ++assessment.lines;
        if (current > assessment.maxLineBytes) assessment.maxLineBytes = current;
        if (assessment.lines > kMaxFileLines) assessment.limit = TextLimit::FileLines;
    }
    if (assessment.limit == TextLimit::Within && text.size() > kMaxRecoveryPayloadBytes)
        assessment.limit = TextLimit::RecoveryPayload;
    return assessment;
}

inline bool withinTextEnvelope(std::string_view text) noexcept
{
    return assessText(text).limit == TextLimit::Within;
}

inline std::string textLimitDiagnostic(const TextAssessment &assessment)
{
    switch (assessment.limit) {
    case TextLimit::FileLines:
        return "File of " + std::to_string(assessment.lines)
            + " lines exceeds the supported 4000 line limit";
    case TextLimit::LineBytes:
        return "Line of " + std::to_string(assessment.maxLineBytes)
            + " bytes exceeds the supported 262144 byte line limit";
    case TextLimit::RecoveryPayload:
        return "Text of " + std::to_string(assessment.bytes)
            + " bytes exceeds the supported 8388608 byte recovery payload limit";
    case TextLimit::Within:
        return {};
    }
    return {};
}

}
