// Platform journeys: a right-to-left layout, a fractional scale factor, and the accessible roles
// and keyboard focus chain of the main view's controls. CTest runs this group with
// QT_SCALE_FACTOR=1.5, so every journey here also runs scaled.
#include "acceptance/JourneyData.h"
#include "acceptance/JourneySteps.h"
#include "acceptance/JourneySuite.h"
#include "acceptance/WindowChecks.h"
#include "fixtures/FileToolbar.h"
#include "fixtures/QuickScene.h"

#include <QAccessible>
#include <QGuiApplication>
#include <QSet>

#include <cmath>
#include <cstdlib>

using namespace branch_truce;

namespace journey {
namespace {

// Sets the application's layout direction while it lives. Kirigami.ApplicationWindow mirrors its
// whole item tree from it, as it does under a right-to-left locale.
class LayoutDirection {
public:
    explicit LayoutDirection(Qt::LayoutDirection direction) : previous_(QGuiApplication::layoutDirection())
    {
        QGuiApplication::setLayoutDirection(direction);
    }
    ~LayoutDirection() { QGuiApplication::setLayoutDirection(previous_); }
    LayoutDirection(const LayoutDirection &) = delete;
    LayoutDirection &operator=(const LayoutDirection &) = delete;

private:
    Qt::LayoutDirection previous_;
};

// Turns accessibility on while it lives, as an attached assistive technology does, so Qt Quick
// Controls publish their roles.
class AccessibilityActive {
public:
    AccessibilityActive() : previous_(QAccessible::isActive()) { QAccessible::setActive(true); }
    ~AccessibilityActive() { QAccessible::setActive(previous_); }
    AccessibilityActive(const AccessibilityActive &) = delete;
    AccessibilityActive &operator=(const AccessibilityActive &) = delete;

private:
    bool previous_;
};

// Every screen size a window journey requires, with eight rows in each text pane.
void expectFitsWithEightRows(Scene &scene)
{
    const auto lineHeight = scene.item("comparisonView")->property("rowHeight").toReal();
    CHECK(lineHeight > 0);
    for (const auto &screen : {QSize(1280, 720), QSize(1280, 800)}) {
        const auto pane = checkFits(scene, screen.width(), screen.height());
        if (pane < 8 * lineHeight)
            throw std::runtime_error("text panes " + std::to_string(pane) + " px high at "
                                     + std::to_string(screen.width()) + "x" + std::to_string(screen.height())
                                     + ", less than eight " + std::to_string(lineHeight) + " px lines");
    }
}

// The scene x of a named item's left edge.
qreal leftEdge(Scene &scene, const char *name) { return sceneOrigin(scene.item(name)).x(); }

void rightToLeftMirrorsTheWindow(Sandbox &sandbox)
{
    const LayoutDirection rightToLeft(Qt::RightToLeft);
    Repository repo(journeyPaths());
    Scene scene(sandbox, repo);
    clickFile(scene, "story.txt");
    // Each pair of toolbar steps reads from the right: the earlier step sits right of the later one.
    for (const auto &[earlier, later] :
         {std::pair{"previousUndecided", "nextUndecided"}, std::pair{"previousUnfinished", "nextUnfinished"}}) {
        if (!(leftEdge(scene, earlier) > leftEdge(scene, later)))
            throw std::runtime_error(std::string(earlier) + " is not right of " + later + ": "
                                     + geometry(scene.item(earlier)) + ", " + geometry(scene.item(later)));
    }
    expectFitsWithEightRows(scene);
    checkOffline(scene);
}

void scaledWindowKeepsEveryControlReachable(Sandbox &sandbox)
{
    const auto requested = qEnvironmentVariable("QT_SCALE_FACTOR");
    bool parsed = false;
    const auto factor = requested.toDouble(&parsed);
    if (!parsed || factor == 1.0) test_support::skip("set QT_SCALE_FACTOR to a factor other than 1 to run this");
    Repository repo(journeyPaths());
    Scene scene(sandbox, repo);
    // The window renders at the requested scale, so the fit below is measured scaled.
    if (std::abs(scene.window()->devicePixelRatio() - factor) > 0.01)
        throw std::runtime_error("device pixel ratio " + std::to_string(scene.window()->devicePixelRatio())
                                 + " under QT_SCALE_FACTOR=" + requested.toStdString());
    clickFile(scene, "story.txt");
    expectFitsWithEightRows(scene);
    checkOffline(scene);
}

// Every item the keyboard reaches with Tab from the window's root, in order.
QSet<QQuickItem *> focusChain(QQuickWindow *window)
{
    QSet<QQuickItem *> reached;
    auto *start = window->contentItem();
    auto *item = start->nextItemInFocusChain(true);
    while (item && item != start && !reached.contains(item)) {
        reached.insert(item);
        item = item->nextItemInFocusChain(true);
    }
    return reached;
}

bool buttonRole(QAccessible::Role role)
{
    return role == QAccessible::Button || role == QAccessible::CheckBox || role == QAccessible::RadioButton
        || role == QAccessible::ButtonMenu || role == QAccessible::ButtonDropDown;
}

void controlsHaveRolesAndTakeKeyboardFocus(Sandbox &sandbox)
{
    const AccessibilityActive accessibility;
    Repository repo(journeyPaths());
    Scene scene(sandbox, repo);
    clickFile(scene, "story.txt");
    for (const auto *name : mainControls) {
        const bool overflow = test_support::fileOverflowControl(name);
        auto *control = overflow ? test_support::fileToolbarControl(scene.window(), name) : scene.item(name);
        const auto *face = test_support::requireAccessible(control, name);
        CHECK(control->isVisible());
        if (control->inherits("QQuickAbstractButton")) {
            if (!buttonRole(face->role()) && !(overflow && face->role() == QAccessible::MenuItem))
                throw std::runtime_error(std::string(name) + " has an unexpected accessible button role");
            if (control->isEnabled()) {
                if (!overflow && !focusChain(scene.window()).contains(control))
                    throw std::runtime_error(std::string(name) + " is not in the keyboard focus chain");
                // Popup entries use the menu's keyboard navigation rather than the window Tab chain.
                control->forceActiveFocus(Qt::TabFocusReason);
                test_support::settleFor(std::string(name) + " accessible keyboard focus",
                                        [&] { return control->hasActiveFocus() && face->state().focused; });
            }
        }
        if (overflow) test_support::closeFileToolbarMenu(scene.window());
    }
    checkOffline(scene);
}
} // namespace

void addPlatformJourneys(JourneySuite &suite)
{
    const auto add = [&](const char *name, JourneySuite::Journey journey) {
        suite.add("platform", name, std::move(journey));
    };
    add("a right-to-left layout mirrors the toolbar and keeps every control and text pane reachable",
        rightToLeftMirrorsTheWindow);
    add("a fractional scale factor keeps every control and text pane reachable",
        scaledWindowKeepsEveryControlReachable);
    add("every named control has an accessible role and each enabled button takes keyboard focus",
        controlsHaveRolesAndTakeKeyboardFocus);
}

} // namespace journey
