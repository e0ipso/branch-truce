// Text and passage helpers shared by the correspondence and line-alignment suites.
#pragma once
#include "core/Correspondence.h"
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

using namespace branch_truce;

namespace {
inline const char *name(PassageKind kind)
{
    switch (kind) {
    case PassageKind::Equal: return "equal";
    case PassageKind::Changed: return "changed";
    case PassageKind::SourceOnly: return "source only";
    case PassageKind::ResultOnly: return "result only";
    case PassageKind::Ambiguous: break;
    }
    return "ambiguous";
}

inline std::string show(const std::vector<Passage> &passages)
{
    std::string text;
    for (const auto &passage : passages) {
        text += name(passage.kind);
        text += ' ';
        text += std::to_string(passage.source.begin);
        text += '-';
        text += std::to_string(passage.source.end);
        text += ' ';
        text += std::to_string(passage.result.begin);
        text += '-';
        text += std::to_string(passage.result.end);
        text += '\n';
    }
    return text;
}

inline std::string uniqueLines(std::string_view prefix, std::size_t count)
{
    std::string text;
    for (std::size_t line = 0; line < count; ++line) {
        text += prefix;
        text += std::to_string(line);
        text += '\n';
    }
    return text;
}

// Every text of up to `most` lines over "a" and "b", in a fixed order.
inline std::vector<std::vector<std::string>> allTexts(std::size_t most)
{
    std::vector<std::vector<std::string>> texts{{}};
    for (std::size_t from = 0; from < texts.size(); ++from)
        if (texts[from].size() < most)
            for (const auto *line : {"a\n", "b\n"}) {
                auto longer = texts[from];
                longer.emplace_back(line);
                texts.push_back(std::move(longer));
            }
    return texts;
}

inline std::string joined(const std::vector<std::string> &lines)
{
    std::string text;
    for (const auto &line : lines) text += line;
    return text;
}
}
