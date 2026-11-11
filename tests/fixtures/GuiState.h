#pragma once

// Process-wide GUI state one case may change, put back when the case leaves the scope, failed or
// not, so the next case in the same QGuiApplication starts from the same state.
//
//   ScopedClipboard     clears the clipboard on entry and on exit.
//   ScopedQuitOnClose   sets QGuiApplication::quitOnLastWindowClosed and restores it.

#include <QClipboard>
#include <QGuiApplication>

namespace test_support {

class ScopedClipboard {
public:
    ScopedClipboard() { QGuiApplication::clipboard()->clear(); }
    ~ScopedClipboard() { QGuiApplication::clipboard()->clear(); }
    ScopedClipboard(const ScopedClipboard &) = delete;
    ScopedClipboard &operator=(const ScopedClipboard &) = delete;
};

class ScopedQuitOnClose {
public:
    explicit ScopedQuitOnClose(bool quit) : previous_(QGuiApplication::quitOnLastWindowClosed())
    {
        QGuiApplication::setQuitOnLastWindowClosed(quit);
    }
    ~ScopedQuitOnClose() { QGuiApplication::setQuitOnLastWindowClosed(previous_); }
    ScopedQuitOnClose(const ScopedQuitOnClose &) = delete;
    ScopedQuitOnClose &operator=(const ScopedQuitOnClose &) = delete;

private:
    bool previous_;
};

} // namespace test_support
