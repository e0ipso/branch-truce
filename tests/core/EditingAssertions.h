// Assertions shared by the scoped, batch, combination and marker editing suites.
#pragma once
#include "TestHarness.h"
#include "core/EditingCommands.h"
#include "core/SessionFixtures.h"
#include "fixtures/ChoiceFile.h"
#include <algorithm>
#include <array>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace branch_truce;
using test_support::choose;
using test_support::engaged;
using test_support::has;
using test_support::titled;

namespace {
inline const char *name(ChoiceScope scope) { return scope == ChoiceScope::Line ? "line" : "section"; }

inline const char *name(ChoiceAction action)
{
    switch (action) {
    case ChoiceAction::Accept: return "accept";
    case ChoiceAction::Ignore: return "ignore";
    case ChoiceAction::ResolveUsingSide: return "resolve using side";
    case ChoiceAction::CombineLeftFirst: return "left first";
    case ChoiceAction::CombineRightFirst: break;
    }
    return "right first";
}

inline const char *name(NonConflictingScope scope)
{
    switch (scope) {
    case NonConflictingScope::Left: return "left";
    case NonConflictingScope::Right: return "right";
    case NonConflictingScope::Both: break;
    }
    return "both sides";
}

inline const char *name(SourceSide side)
{
    switch (side) {
    case SourceSide::Earlier: return "earlier";
    case SourceSide::Stage2: return "left";
    case SourceSide::Stage3: break;
    }
    return "right";
}

// The captured sources; a fixture without them fails the case.
inline const FileSnapshots &sourcesOf(const FileSession &file)
{
    const auto *sources = file.snapshots();
    CHECK(sources != nullptr);
    return *sources;
}

// The capture of `side`; a fixture without one fails the case.
inline const SourceSnapshot &capturedSide(const FileSession &file, SourceSide side)
{
    const auto *sources = file.snapshots();
    CHECK(sources != nullptr);
    const SourceSnapshot *captured = nullptr;
    if (sources != nullptr) {
        const auto &source = sources->source(side);
        if (source.has_value()) captured = &source.value();
    }
    CHECK(captured != nullptr);
    return *captured;
}

struct Observed {
    RevisionId id = 0;
    Draft draft;
    bool undo = false;
    bool redo = false;
};

inline Observed observe(const FileSession &file)
{
    return {file.current().id, file.current().draft, file.canUndo(), file.canRedo()};
}

inline void expectUnchanged(const FileSession &file, const Observed &before)
{
    CHECK(file.current().id == before.id);
    CHECK(file.current().draft == before.draft);
    CHECK(file.canUndo() == before.undo);
    CHECK(file.canRedo() == before.redo);
}

inline void expectCurrent(const FileSession &file, const Draft &draft)
{
    CHECK(file.current().draft == draft);
    CHECK(file.correspondence() != nullptr);
    CHECK(file.correspondence()->revision() == file.current().id);
}

// Undoes every step back to the first and redoes them all, checking each draft.
inline void replay(FileSession &file, const std::vector<Draft> &steps)
{
    CHECK(steps.size() >= 2);
    expectCurrent(file, steps.back());
    for (std::size_t index = steps.size() - 1; index > 0; --index) {
        CHECK(file.undo(file.current().id) == Transition::Applied);
        expectCurrent(file, steps[index - 1]);
    }
    CHECK(!file.canUndo() && file.canRedo());
    for (std::size_t index = 1; index < steps.size(); ++index) {
        CHECK(file.redo(file.current().id) == Transition::Applied);
        expectCurrent(file, steps[index]);
    }
    CHECK(file.canUndo() && !file.canRedo());
}

inline ResultAnchor anchor(const FileSession &file, const std::string &id = "choice")
{
    const auto *found = file.correspondence() ? file.correspondence()->anchor(id) : nullptr;
    CHECK(found != nullptr);
    return *found;
}

// The `scope` target of `side` whose source text is exactly `line`.
inline const ChoiceTarget &targetFor(const FileSession &file, SourceSide side, ChoiceScope scope, std::string_view line)
{
    const std::string_view text = capturedSide(file, side).worktree.utf8;
    for (const auto &target : file.choices()) {
        if (target.source.side != side || target.scope != scope) continue;
        if (text.substr(target.source.range.begin, target.source.range.end - target.source.range.begin) == line)
            return target;
    }
    throw std::runtime_error("fixture has no requested choice scope");
}

inline void expectDecision(const FileSession &file, const ChoiceTarget &target, Decision expected)
{
    CHECK(!target.decisions.empty());
    for (const auto &id : target.decisions) CHECK(file.current().draft.decisions.at(id) == expected);
}

const Decision kIgnored{DecisionValue::Ignored, {}};
const Decision kUndecided{};

inline bool contains(const std::vector<std::string> &ids, const std::string &id)
{
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

}
