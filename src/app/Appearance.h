// The appearance chooses the Qt Quick Controls style and the icon themes once, at startup.
#pragma once

class QQmlEngine;

namespace branch_truce {
// The KDE style when engine can import it, with Fusion for the controls it lacks, and otherwise
// Fusion. The KDE style takes its colours from the user's KDE colour scheme.
// QT_QUICK_CONTROLS_STYLE wins.
void selectControlsStyle(const QQmlEngine &engine);
// Breeze as the fallback icon theme, and hicolor when the platform names no theme. Off Plasma the
// icon theme is hicolor or none, so icon.name falls back to Breeze. Platforms such as offscreen or
// vnc name no icon theme; with no desktop named either, Kirigami then swaps in a bundled theme and
// search path that distro builds lack. Naming hicolor, as Qt does on xcb and Wayland, keeps the
// system path, so the Breeze fallback still applies.
void selectIconThemes();
}
