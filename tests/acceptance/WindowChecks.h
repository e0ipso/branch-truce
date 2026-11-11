#pragma once

// Checks of the window's geometry: what fits a logical screen size, which wrapped texts break
// words, how the wheel moves the page and where the first undecided region sits.

#include "acceptance/JourneyScene.h"

#include <QQuickItem>

#include <optional>
#include <string>
#include <vector>

namespace journey {

// Every named control of the main view, in reading order.
inline constexpr const char *mainControls[] = {
    "openHandoff",   "previousUndecided", "previousUnfinished", "nextUnfinished",    "undecidedCounter",
    "nextUndecided", "navigateMenu",      "toggleFileList",     "fileList",          "selectedPath",
    "saveDraft",     "markResolved",      "discardUnsaved",     "undoCommand",       "redoCommand",
    "copyResult",    "pasteResult",       "sourceChoicesMenu",  "showSourceChoices", "openPassageSearch",
    "wrapLines",     "showWhitespace",    "stage2Label",        "resultLabel",       "stage3Label",
    "showEarlier",   "workflowStatus",
};
inline constexpr const char *textPanes[] = {"stage2Viewport", "resultViewport", "stage3Viewport"};

// An item's name, scene position, size and visibility, for a failure message.
std::string geometry(QQuickItem *item);
// mainControls and textPanes.
std::vector<const char *> everyControl();
// Resizes to a logical screen size and requires the named items inside the window, scrolling an
// enclosing view first as a user would. Returns the smallest text pane height.
qreal checkFits(Scene &scene, int width, int height, const std::vector<const char *> &names = everyControl());
// Turns the mouse wheel over the named label at the top of the page. The page moves only when its
// content is taller than the window. Returns whether it was.
bool checkWheelScroll(Scene &scene, const char *over);
// Every visible wrapped text under root keeps its words whole. Returns how many texts wrap.
int expectWholeWords(QQuickItem *root, const std::string &where);
// Empty when the first line of the earliest undecided region on each side lies inside its pane's
// viewport, else where each pane stands. `snapshot` describes every pane even when it is.
std::string firstUndecidedInView(Scene &scene, bool snapshot = false);

} // namespace journey
