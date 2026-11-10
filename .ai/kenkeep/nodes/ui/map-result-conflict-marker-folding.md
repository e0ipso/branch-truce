---
type: map
title: Result conflict-marker folding
description: >-
  TextBlocks folds recognized conflict content without changing result bytes,
  line numbers or application history.
tags:
  - comparison
  - qml
  - editing
  - markers
kk_schema_version: 3
kk_id: map-result-conflict-marker-folding
kk_derived_from:
  - '3994e51f-d660-44e0-bca2-753952522f57:map:0'
kk_relates_to:
  - map-conflict-marker-scan
  - practice-keep-sources-identifiable-and-the-result-editable
  - practice-follow-the-ui-design-brief-for-the-kde-native-window
kk_depends_on: []
kk_confidence: medium
---
The result pane folds recognized conflict markers through TextBlocks block visibility. Folding leaves the result buffer, serialized bytes, line numbers, choices and application history unchanged. The view shows conflict marks in place of the hidden content.

Show conflict markers, Find and Go to line reveal the raw lines. Cursor movement and selections skip folded content, and an edit that changes hidden content reveals it. Undo, redo and recovered text reapply the fold from the current result.

<!-- kk:related:start -->
# Related

- Related: [map-conflict-marker-scan](../core/map-conflict-marker-scan.md)
- Related: [practice-keep-sources-identifiable-and-the-result-editable](../product/practice-keep-sources-identifiable-and-the-result-editable.md)
- Related: [practice-follow-the-ui-design-brief-for-the-kde-native-window](practice-follow-the-ui-design-brief-for-the-kde-native-window.md)
<!-- kk:related:end -->

<!-- kk:citations:start -->
# Citations

[1] 3994e51f-d660-44e0-bca2-753952522f57:map:0
<!-- kk:citations:end -->
