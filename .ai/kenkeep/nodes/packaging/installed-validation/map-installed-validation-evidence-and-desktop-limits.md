---
type: map
title: Installed validation evidence and desktop limits
description: >-
  Container measurements retain their environment limits and do not establish
  real desktop compatibility.
tags:
  - packaging
  - platform
  - validation
  - wayland
  - accessibility
  - compatibility
kk_schema_version: 3
kk_id: map-installed-validation-evidence-and-desktop-limits
kk_derived_from: []
kk_relates_to:
  - map-per-base-debian-artifacts-and-container-inspection
  - map-branch-truce-desktop-implementation-baseline
  - map-journey-acceptance-suite
kk_depends_on: []
kk_confidence: high
---
[docs/platform-validation.md](../../../../../docs/platform-validation.md) records the results. Word each one as observed in a container of that base. Variables such as `XDG_CURRENT_DESKTOP=KDE` make Qt behave as in a Plasma session, but the container has no KWin, Plasma shell, portals, input method, screen reader or physical display. Rows that need them stay UNAVAILABLE or Not run: both real desktops, Plasma menu launch, KWin output scaling, IME, Orca and real hardware. Nothing in that file is a compatibility claim. For those historical measurements, the only Tuxedo OS host was a headless server and no Kubuntu 26.04 machine was available. The later user-reported installation does not change those dated rows.

The minimum window size in [Main.qml](../../../../../src/ui/Main.qml) is 900x500 logical pixels, so the window fits 1920x1080 at 200% (960x540). When the window is shorter than its content the page scrolls, each text pane keeps at least eight lines, and the row of file actions wraps. The container runs measured 150% and 200% with `QT_SCALE_FACTOR` 1.5 and 2. 125% and KWin's own output scaling were not measured. The window-fit journey in the [acceptance suite](../../ui/map-journey-acceptance-suite.md) checks 1280x720 and 960x540 offscreen.

<!-- kk:related:start -->
# Related

- Related: [map-per-base-debian-artifacts-and-container-inspection → map-developer-debian-artifact-builds-and-inspection](../artifact-paths/map-developer-debian-artifact-builds-and-inspection.md)
- Related: [map-per-base-debian-artifacts-and-container-inspection → map-native-release-package-artifact-flow](../artifact-paths/map-native-release-package-artifact-flow.md)
- Related: [map-per-base-debian-artifacts-and-container-inspection → map-runtime-qml-and-plugin-package-dependencies](../artifact-paths/map-runtime-qml-and-plugin-package-dependencies.md)
- Related: [map-branch-truce-desktop-implementation-baseline → map-desktop-comparison-and-workflow-integration](../../architecture/desktop-baseline/map-desktop-comparison-and-workflow-integration.md)
- Related: [map-branch-truce-desktop-implementation-baseline → map-desktop-platform-and-implementation-stack](../../architecture/desktop-baseline/map-desktop-platform-and-implementation-stack.md)
- Related: [map-branch-truce-desktop-implementation-baseline → map-editing-and-recovery-resource-envelope](../../architecture/desktop-baseline/map-editing-and-recovery-resource-envelope.md)
- Related: [map-branch-truce-desktop-implementation-baseline → map-installed-desktop-application](../../architecture/desktop-baseline/map-installed-desktop-application.md)
- Related: [map-journey-acceptance-suite](../../ui/map-journey-acceptance-suite.md)
<!-- kk:related:end -->
