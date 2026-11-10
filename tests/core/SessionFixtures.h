// Session fixtures: helpers the engine tests share for choices, blockers, case titles and limit-sized text.
#pragma once

#include "core/EditingCommands.h"
#include "core/ResourceLimits.h"
#include "fixtures/ChoiceFile.h"
#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace test_support {
// True when the file's facts list `blocker`.
inline bool has(const branch_truce::FileSession &file, branch_truce::Blocker blocker)
{
    const auto blockers = file.facts().blockers;
    return std::find(blockers.begin(), blockers.end(), blocker) != blockers.end();
}

// Previews `action` on the `occurrence`-th scope of `side` and applies it,
// confirming any replacement.
inline branch_truce::Transition choose(branch_truce::FileSession &file, branch_truce::SourceSide side,
                                       branch_truce::ChoiceAction action,
                                       branch_truce::ChoiceScope scope = branch_truce::ChoiceScope::Section,
                                       std::size_t occurrence = 0)
{
    const auto &target = choiceTarget(file, side, scope, occurrence);
    return applyChoice(file, previewChoice(file, file.current().id, target.source.contributionId, action), true);
}

// Replays `draft` the way recovery does, as the revision after the current
// one, with the recorded hand edits.
inline branch_truce::Transition replay(branch_truce::FileSession &file, const branch_truce::Draft &draft,
                                       std::set<std::string> handEdited = {},
                                       std::vector<branch_truce::ByteRange> handEditedRanges = {})
{
    const auto &current = file.current();
    const auto id = current.id + 1;
    const auto result = draft.text == current.draft.text ? current.result : id;
    return file.replayDraft(current.id, {id, result, draft, std::move(handEdited), std::move(handEditedRanges)});
}

// The value inside `optional`; an empty one fails the case.
template <typename T> const T &engaged(const std::optional<T> &optional)
{
    if (!optional) throw std::runtime_error("expected a value, found none");
    return *optional;
}

template <typename T> T &engaged(std::optional<T> &optional)
{
    if (!optional) throw std::runtime_error("expected a value, found none");
    return *optional;
}

// A case title followed by its parameters, as in "title (line, left first)".
inline std::string titled(std::string title, std::initializer_list<const char *> parameters)
{
    const char *separator = " (";
    for (const auto *parameter : parameters) {
        title += separator;
        title += parameter;
        separator = ", ";
    }
    title += ')';
    return title;
}

// `count` lines of one character each.
inline std::string lines(std::size_t count)
{
    std::string text;
    text.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) text += "x\n";
    return text;
}

// `count` lines of exactly kMaxLineBytes bytes each, newline included.
inline std::string longestLines(std::size_t count)
{
    std::string text;
    text.reserve(count * branch_truce::kMaxLineBytes);
    for (std::size_t i = 0; i < count; ++i) {
        text.append(branch_truce::kMaxLineBytes - 1, 'p');
        text += '\n';
    }
    return text;
}
}
