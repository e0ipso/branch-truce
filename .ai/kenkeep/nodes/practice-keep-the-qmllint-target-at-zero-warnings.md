---
type: practice
title: Keep the qmllint target at zero warnings
description: >-
  branch_truce_ui_qmllint exits 0 while it prints warnings; read the output,
  type QML properties concretely, anchor at use sites.
tags:
  - qml
  - lint
  - ui
  - ci
kk_schema_version: 3
kk_id: practice-keep-the-qmllint-target-at-zero-warnings
kk_derived_from: []
kk_relates_to:
  - map-journey-acceptance-suite
kk_depends_on: []
kk_confidence: high
---
`cmake --build build/desktop --target branch_truce_ui_qmllint` (see [docs/build.md](../../../docs/build.md)) exits 0 even when qmllint prints `Warning:` lines, so a green exit status does not mean the QML is clean. Read the output and keep the warning count at 0.

Type QML properties with the concrete type the code uses. The whitespace overlay in [ComparisonView.qml](../../../src/ui/ComparisonView.qml) declares `required property TextEdit editor`; as `Item`, qmllint cannot resolve `color`, `text`, `positionAt` or `positionToRectangle` and reports `missing-property`.

Set `anchors.fill` where an inline component is used, inside a plain `Item`, rather than on the component's root. qmllint treats an anchored inline component root as a child managed by a layout and warns about anchors on a layout-managed item.

<!-- kk:related:start -->
# Related

- Related: [map-journey-acceptance-suite](map-journey-acceptance-suite.md)
<!-- kk:related:end -->
