#pragma once

#include "TestHarness.h"

namespace comparison_scene {
// Which cases one run executes. The deletion-at-limit cases edit multi-megabyte buffers in the
// real editor, so ctest runs them as their own entry with a timeout sized for them. Display
// runs the viewport cases that a real display can draw differently, on any platform; ctest
// does not run it, because ctest has no display.
enum class SceneSet { All, Scenes, Boundary, Display };

// Forwards a case to the suite only when its selection includes it.
class Group {
public:
    Group(test_support::Suite &suite, bool selected) : suite_(suite), selected_(selected) {}
    template <typename Test> void run(const std::string &name, Test test)
    {
        if (selected_) suite_.run(name, std::move(test));
    }

private:
    test_support::Suite &suite_;
    bool selected_;
};

// The groups one case can belong to.
struct Groups {
    Group &scenes;
    Group &boundary;
    Group &display;
};

void addLayoutCases(Groups groups);
void addEditingCases(Groups groups);
void addTextCases(Groups groups);
void addMarkCases(Groups groups);
void addFoldingCases(Groups groups);
void addNavigationCases(Groups groups);
void addSessionCases(Groups groups);
void addBoundaryCases(Groups groups);
} // namespace comparison_scene
