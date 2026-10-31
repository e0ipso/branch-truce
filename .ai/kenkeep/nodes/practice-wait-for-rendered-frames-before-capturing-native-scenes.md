---
type: practice
title: Wait for rendered frames before capturing native scenes
description: >-
  Capture native replay after publication, layout and rendered frames settle
  while preserving real controls and focus.
tags:
  - testing
  - qml
  - ui
  - capture
  - readiness
kk_schema_version: 3
kk_id: practice-wait-for-rendered-frames-before-capturing-native-scenes
kk_derived_from: []
kk_relates_to:
  - practice-wait-for-layout-before-clicking-offscreen-dialog-items
  - map-startup-style-colour-scheme-and-icon-theme
kk_depends_on: []
kk_confidence: high
---
Native replay captures must await the private compositor's publication, an exposed loaded window, durable file state, completed resolution and settled scene layout. Use the maintained external [native replay tool](../../../tools/native-replay/README.md#L21) with real UI controls and native focus, retaining accessible labels and the pinned source/result bytes.

Named-control geometry alone does not guarantee that a capture shows the current scene. Anonymous icon and colour transitions can keep painting after geometry settles. In [Driver.cpp](../../../tools/native-replay/Driver.cpp#L150), `capture-ready` samples `QQuickWindow::grabWindow()` and requires unchanged frames for 700 ms after the scene snapshot has settled. Keep this rendering check when changing replay readiness; the loaded and layout checks in [the same driver](../../../tools/native-replay/Driver.cpp#L88) serve separate conditions.

[replay.py](../../../tools/native-replay/replay.py#L259) requests `capture-ready` before saving the compositor output with `grim`. Compare independently replayed decoded pixels on the same executable and environment, as [the replay comparison](../../../tools/native-replay/replay.py#L312) does, to detect captures taken before rendering settles. Preserve the native scene when stabilizing capture; do not replace controls, relabel accessibility objects or inject synthetic model state.

<!-- kk:related:start -->
# Related

- Related: [practice-wait-for-layout-before-clicking-offscreen-dialog-items](practice-wait-for-layout-before-clicking-offscreen-dialog-items.md)
- Related: [map-startup-style-colour-scheme-and-icon-theme](map-startup-style-colour-scheme-and-icon-theme.md)
<!-- kk:related:end -->
