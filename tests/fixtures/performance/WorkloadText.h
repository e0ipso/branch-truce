#pragma once

// The generated text the workload measurements load: representative files of fixed-width lines
// and adverse single long lines. Qt-free, so any measurement can share it.

#include <cstddef>
#include <cstdio>
#include <string>

namespace test_support {

// Bytes on an ordinary generated line, the newline included.
inline constexpr std::size_t kWorkloadLineWidth = 96;

// `lineCount` lines of kWorkloadLineWidth bytes. Every 40th line is a shared `repeat-passage`
// line and every 100th line from the 7th carries `café`. A `variant` of 0 or more changes the
// last letter of every line whose number is that residue modulo 25; -1 changes none.
inline std::string workloadFile(std::size_t lineCount, int variant)
{
    std::string out;
    out.reserve(lineCount * (kWorkloadLineWidth + 16));
    for (std::size_t i = 0; i < lineCount; ++i) {
        if (i % 40 == 0) {
            out += "repeat-passage\n";
            continue;
        }
        std::string line;
        char prefix[32];
        const int wrote = std::snprintf(prefix, sizeof prefix, "L%06zu-", i);
        if (wrote > 0) line.append(prefix, static_cast<std::size_t>(wrote));
        if (i % 100 == 7) line += "caf\xc3\xa9-";
        const bool changed = variant >= 0 && (i % 25) == static_cast<std::size_t>(variant);
        if (line.size() + 2 <= kWorkloadLineWidth) line.append(kWorkloadLineWidth - 2 - line.size(), 'a');
        line.push_back(changed ? 'Z' : 'a');
        line.push_back('\n');
        out += line;
    }
    return out;
}

// One line of `bytes` bytes without a newline, ending in `last`.
inline std::string workloadLongLine(std::size_t bytes, char last)
{
    if (bytes == 0) return {};
    std::string text(bytes, 'x');
    text.back() = last;
    return text;
}

} // namespace test_support
