---
type: map
title: 'Startup style, colour scheme and icon theme'
description: >-
  main.cpp picks org.kde.desktop or Fusion, names hicolor when no icon theme is
  set; kdeglobals sets colours; a preloaded probe tests it.
tags:
  - ui
  - startup
  - kde
  - icons
  - testing
  - preview
kk_schema_version: 3
kk_id: map-startup-style-colour-scheme-and-icon-theme
kk_derived_from: []
kk_relates_to:
  - map-branch-truce-desktop-implementation-baseline
  - map-per-base-debian-artifacts-and-container-inspection
  - map-recovery-journal-and-acknowledgment-boundary
  - map-installed-artifact-validation-in-base-containers
kk_depends_on: []
kk_confidence: high
---
[src/app/main.cpp](../../../src/app/main.cpp) sets the window's look before it loads `Main`. It selects the `org.kde.desktop` Qt Quick Controls style when an `org/kde/desktop/qmldir` exists on the engine's import path, with Fusion as the fallback style, and Fusion alone otherwise. A non-empty `QT_QUICK_CONTROLS_STYLE` wins and skips the selection. [docs/platform-validation.md](../../../docs/platform-validation.md) records the measurements behind these rules.

Colours come from the user's `kdeglobals`, which names the scheme under `[General] ColorScheme` and holds its colour groups. The `org.kde.desktop` plugin reads them through KColorScheme and links `libKF6ColorScheme.so.6` itself, so the app does not link `KF6::ColorScheme`. `KDE_COLOR_SCHEME_PATH` in the environment has no effect, because KF6 reads that name only as an application property set by apps with their own scheme picker. To force a scheme, write a `kdeglobals` into a private `XDG_CONFIG_HOME`.

`QIcon::setFallbackThemeName("breeze")` lets `icon.name` resolve off Plasma. When the platform names no icon theme, as offscreen, minimal and vnc do, and `XDG_CURRENT_DESKTOP` is unset, Kirigami's controls plugin swaps in a `breeze-internal` theme and its own search path, which distro builds do not ship, and every Breeze lookup returns null. So `main` names `hicolor` whenever `QIcon::themeName()` is empty, as Qt does on xcb and Wayland. The platform-validation notes have the before/after table per platform.

`main` sets the application name and organization domain but no organization name. Setting one would insert it into `QStandardPaths::AppLocalDataLocation` and move the recovery root away from `<XDG_DATA_HOME>/Branch Truce/recovery/v1/`. Existing journals would then be orphaned.

Startup code is not covered by the offscreen `Main` scene harness, which never runs `main`. [tests/fixtures/AppearanceProbe.cpp](../../../tests/fixtures/AppearanceProbe.cpp) is a `MODULE` library loaded with `LD_PRELOAD` into the real `branch-truce` binary. Once a window exists it prints one `BRANCH_TRUCE_APPEARANCE=` JSON line with the style, icon theme, fallback theme, whether `document-save` resolves, and the display name. [tests/app/StartupAppearanceTests.cpp](../../../tests/app/StartupAppearanceTests.cpp), the ctest `app.startup-appearance`, and the package inspection's icon step both use it. Test new startup behaviour the same way.

[tools/preview-app.sh](../../../tools/preview-app.sh) captures BreezeLight and BreezeDark PNGs of a build on the current sway session, using a private home with a per-scheme `kdeglobals` and a merge fixture from `make_merge_fixture` in [tools/merge-fixture.sh](../../../tools/merge-fixture.sh). The installed-artifact validator uses the same function. The window goes fullscreen so other windows do not resize it, and the capture waits for the session's recovery journal to appear before it settles and shoots. [docs/build.md](../../../docs/build.md) lists its options.

<!-- kk:related:start -->
# Related

- Related: [map-branch-truce-desktop-implementation-baseline](map-branch-truce-desktop-implementation-baseline.md)
- Related: [map-per-base-debian-artifacts-and-container-inspection](map-per-base-debian-artifacts-and-container-inspection.md)
- Related: [map-recovery-journal-and-acknowledgment-boundary](map-recovery-journal-and-acknowledgment-boundary.md)
- Related: [map-installed-artifact-validation-in-base-containers](map-installed-artifact-validation-in-base-containers.md)
<!-- kk:related:end -->
