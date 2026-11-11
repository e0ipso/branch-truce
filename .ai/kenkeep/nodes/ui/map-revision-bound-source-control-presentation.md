---
type: map
title: Revision-bound source control presentation
description: >-
  Stable source-row models publish complete comparison snapshots and update
  command fields without recreating unaffected controls.
tags:
  - comparison
  - qml
  - editing
  - revisions
kk_schema_version: 3
kk_id: map-revision-bound-source-control-presentation
kk_derived_from:
  - 'eafaf840-7389-45ad-aa53-003771d300c2:map:1'
kk_relates_to:
  - practice-keep-sources-identifiable-and-the-result-editable
  - practice-make-conflict-choices-explicit-and-reversible
  - map-revision-bound-passage-correspondence
kk_depends_on: []
kk_confidence: medium
---
[ComparisonPresentation](../../../../src/app/ComparisonPresentation.h) owns a stable `QAbstractListModel` for each source pane. The [publication path](../../../../src/app/ComparisonPresentation.cpp) installs the full revision snapshot, contribution lookup, sections and both models' rows before completed model updates are emitted. Changed rows with the same source IDs emit `dataChanged`; a different file, row count or source-ID sequence resets the model. Unchanged rows retain their native controls, focus and popups. The `QList` row snapshots remain available for context inspection.

[ChoiceButtons](../../../../src/ui/ChoiceButtons.qml) instantiates range-menu delegates only while the menu is visible. Controls resolve their contribution IDs against the current presentation and revision. Stable identity must retain truthful command availability, mapping and decision updates.

<!-- kk:related:start -->
# Related

- Related: [practice-keep-sources-identifiable-and-the-result-editable](../product/practice-keep-sources-identifiable-and-the-result-editable.md)
- Related: [practice-make-conflict-choices-explicit-and-reversible](../product/practice-make-conflict-choices-explicit-and-reversible.md)
- Related: [map-revision-bound-passage-correspondence](../core/map-revision-bound-passage-correspondence.md)
<!-- kk:related:end -->

<!-- kk:citations:start -->
# Citations

[1] eafaf840-7389-45ad-aa53-003771d300c2:map:1
<!-- kk:citations:end -->
