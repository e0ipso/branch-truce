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
  - practice-design-desktop-apps-around-screens-and-components
kk_depends_on: []
kk_confidence: high
---
In the offscreen Qt Quick harness in [ComparisonViewTests.cpp](../../../tests/app/ComparisonViewTests.cpp), a dialog's rows, such as the handoff dialog's `handoffRoute-<fileId>` buttons and `handoffLeave`, reach their final position only after a layout pass. A click sent as soon as the dialog opens can land somewhere else. Use `waitForLayout(fixture, name)` to get the item and then `clickItem`. `waitForLayout` waits up to 5000 ms until the named item is visible, has a non-zero size, and has a scene center that stays put between two checks and falls inside the window. On timeout it throws with the last visibility, size and center it saw.

Repeater delegates inside a popup can be reached only through the visual item tree. `visualItem` walks `childItems()` from the window's content item and matches by `objectName`, so each delegate a test clicks needs its own `objectName`.

<!-- kk:related:start -->
# Related

- Related: [practice-keep-unsupported-and-unfinished-work-visible](practice-keep-unsupported-and-unfinished-work-visible.md)
- Related: [practice-design-desktop-apps-around-screens-and-components](practice-design-desktop-apps-around-screens-and-components.md)
<!-- kk:related:end -->
