---
type: practice
title: Wait for layout before clicking offscreen dialog items
description: >-
  In offscreen UI tests, find popup delegates through the visual item tree and
  click only after their scene position settles.
tags:
  - testing
  - qml
  - ui
  - offscreen
kk_schema_version: 3
kk_id: practice-wait-for-layout-before-clicking-offscreen-dialog-items
kk_derived_from: []
kk_relates_to:
  - practice-keep-unsupported-and-unfinished-work-visible
  - map-journey-acceptance-suite
  - practice-design-desktop-apps-around-screens-and-components
kk_depends_on: []
kk_confidence: high
---
The offscreen Qt Quick helpers live in [tests/fixtures/QuickScene.h](../../../tests/fixtures/QuickScene.h), shared by [ComparisonViewTests.cpp](../../../tests/app/ComparisonViewTests.cpp) and the [journey acceptance suite](../../../tests/acceptance/JourneyAcceptanceTests.cpp). A dialog's rows, such as the handoff dialog's `handoffRoute-<fileId>` buttons and `handoffLeave`, reach their final position only after a layout pass. A click sent as soon as the dialog opens can land somewhere else. Get the item with `waitForLayout(window, name, stableMs)` and then call `clickItem`. `waitForLayout` waits up to 5000 ms until the named item is visible, has a non-zero size, and has a scene center inside the window that has not moved for `stableMs`. With the default of 0 the center only has to match between two checks. On timeout it throws with the last visibility, size and center it saw.

In a scene that changes state just before the click, two matching checks are not enough, because the click can land between the state change and the layout pass that moves the target. The acceptance suite passes `settleMs = 60`. A target clipped by a `Flickable` is not under the pointer either. The suite's `reveal` sets each enclosing `Flickable`'s `contentY` until the target fits, then waits for layout again before clicking.

Repeater delegates inside a popup can be reached only through the visual item tree. `visualItem` walks `childItems()` from the window's content item and matches by `objectName`, so each delegate a test clicks needs its own `objectName`.

<!-- kk:related:start -->
# Related

- Related: [practice-keep-unsupported-and-unfinished-work-visible](practice-keep-unsupported-and-unfinished-work-visible.md)
- Related: [map-journey-acceptance-suite](map-journey-acceptance-suite.md)
- Related: [practice-design-desktop-apps-around-screens-and-components](practice-design-desktop-apps-around-screens-and-components.md)
<!-- kk:related:end -->
