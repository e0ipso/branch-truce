#pragma once

#include "ComparisonScene.h"
#include "fixtures/FileToolbar.h"
#include <QKeyEvent>
#include <QTest>

namespace comparison_scene {
inline void select(Fixture &fixture, int begin, int end)
{
    CHECK(QMetaObject::invokeMethod(fixture.item("resultText"), "select", Q_ARG(int, begin), Q_ARG(int, end)));
}
// A typed key leaves the file's passages provisional until a worker aligns them; this waits
// for that, as a user who looks at the result would.
inline void key(Fixture &fixture, int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier, const QString &text = {})
{
    auto *editor = fixture.item("resultText");
    editor->forceActiveFocus();
    QKeyEvent event(QEvent::KeyPress, code, modifiers, text);
    QCoreApplication::sendEvent(editor, &event);
    if (modifiers.testFlag(Qt::ControlModifier))
        fixture.settleCommand();
    else
        fixture.settleAligned();
}
// Types each character with no event-loop turn in between, so no pause ends the burst.
inline void type(Fixture &fixture, const QString &text)
{
    auto *editor = fixture.item("resultText");
    editor->forceActiveFocus();
    for (const auto character : text) {
        // Qt's key codes for Latin letters and digits are their upper-case code points.
        QKeyEvent event(QEvent::KeyPress, static_cast<int>(character.toUpper().unicode()), Qt::NoModifier,
                        QString(character));
        QCoreApplication::sendEvent(editor, &event);
    }
    fixture.settleAligned();
}
inline void click(Fixture &fixture, const char *name)
{
    auto *target = test_support::fileOverflowControl(name)
        ? test_support::fileToolbarControl(fixture.window, name)
        : test_support::waitForStable(
              fixture.window, name,
              [&] {
                  auto *candidate = fixture.item(name);
                  return test_support::reachable(fixture.window, candidate) ? candidate : nullptr;
              },
              60);
    test_support::clickItem(fixture.window, target);
    fixture.settleCommand();
}
inline bool toolbarEnabled(Fixture &fixture, const char *name)
{
    auto *button = test_support::fileOverflowControl(name) ? test_support::fileToolbarControl(fixture.window, name)
                                                           : fixture.item(name);
    const bool enabled = button->isEnabled();
    if (test_support::fileOverflowControl(name)) test_support::closeFileToolbarMenu(fixture.window);
    return enabled;
}
// Open a toolbar menu and click one of its entries once the menu has laid out, then wait
// until the menu has finished closing. The KDE style fades a menu out; reopening it during
// that fade lets the end of the fade close the reopened menu.
inline void clickMenuEntry(Fixture &fixture, const char *menu, const char *entry)
{
    click(fixture, menu);
    const auto name = QString::fromLatin1(entry);
    test_support::clickItem(fixture.window, test_support::waitForLayout(fixture.window, name, 60));
    test_support::settleFor(std::string("the menu holding ") + entry + " to close",
                            [&] { return !test_support::visualItem(fixture.window->contentItem(), name); });
}
inline void clickItem(Fixture &fixture, QQuickItem *target) { test_support::clickItem(fixture.window, target); }

inline QQuickItem *waitForLayout(Fixture &fixture, const QString &name)
{
    return test_support::waitForLayout(fixture.window, name);
}

// Waits for the result's cursor to come to rest at position.
inline void expectCursor(Fixture &fixture, const std::string &when, int position)
{
    auto *editor = fixture.item("resultText");
    const auto describe = [&] {
        return when + ": the cursor is at " + std::to_string(editor->property("cursorPosition").toInt()) + ", not "
            + std::to_string(position);
    };
    test_support::waitUntil([&] { return editor->property("cursorPosition").toInt() == position; }, describe);
    // At rest: nothing queued moves it on.
    test_support::waitForFrames(fixture.window, when);
    CHECK_EQ(editor->property("cursorPosition").toInt(), position);
}

} // namespace comparison_scene
