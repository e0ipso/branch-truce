---
type: map
title: Literal project text in native controls
description: >-
  File rows and tooltips keep native backing text empty so retained auto-text
  labels never receive repository markup.
tags:
  - ui
  - qml
  - paths
  - testing
kk_schema_version: 3
kk_id: map-literal-project-text-in-native-controls
kk_derived_from:
  - 'eafaf840-7389-45ad-aa53-003771d300c2:map:2'
kk_relates_to:
  - practice-keep-unsupported-and-unfinished-work-visible
  - map-journey-acceptance-suite
  - map-startup-style-colour-scheme-and-icon-theme
kk_depends_on: []
kk_confidence: medium
---
[FileRow](../../../../src/ui/FileRow.qml) keeps the native `ItemDelegate.text` empty and explicitly names the row with `Accessible.name: row.path`. Custom `PlainLabel` items render the path and status. A Qt style can retain an unused auto-text label after `contentItem` is replaced, so repository text must not enter that backing property.

[PlainToolTip](../../../../src/ui/PlainToolTip.qml) accepts repository data through `literalText`, renders it with `Text.PlainText` and leaves inherited `text` empty for the same reason. The [crafted-path scene regression](../../../../tests/app/ComparisonSessionTests.cpp) exercises the real file list, rendered handoff, notices and tooltip, checking literal text and zero image requests. A traversal that changes text formats only after native labels are created does not by itself prove initial image requests were prevented.

Native Kirigami toolbar delegates can create independent tooltips from `Action.tooltip`, including for actions with a custom display component. [MenuAction](../../../../src/ui/MenuAction.qml) accepts project-derived guidance through `literalToolTip` and sends it only to an owned `PlainToolTip`; [FileToolBar](../../../../src/ui/FileToolBar.qml) uses this for the full source-choice path guidance. Keep project text out of the inherited native action tooltip. The crafted-path regression opens this owned popup, checks the complete guidance and plain-text format, and still requires zero image requests across the scene.

<!-- kk:related:start -->
# Related

- Related: [practice-keep-unsupported-and-unfinished-work-visible](../product/practice-keep-unsupported-and-unfinished-work-visible.md)
- Related: [map-journey-acceptance-suite](map-journey-acceptance-suite.md)
- Related: [map-startup-style-colour-scheme-and-icon-theme](map-startup-style-colour-scheme-and-icon-theme.md)
<!-- kk:related:end -->

<!-- kk:citations:start -->
# Citations

[1] eafaf840-7389-45ad-aa53-003771d300c2:map:2
<!-- kk:citations:end -->
