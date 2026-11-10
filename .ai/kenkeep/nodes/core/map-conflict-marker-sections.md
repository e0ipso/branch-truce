---
type: map
title: Conflict marker sections
description: >-
  Marker sections tie intact Git conflict frames to choices and clean up fully
  decided blocks in the same undo entry.
tags:
  - git
  - editing
  - markers
  - undo
  - recovery
kk_schema_version: 3
kk_id: map-conflict-marker-sections
kk_derived_from:
  - '1d4a909e-497a-4b0c-a66a-f79616a21068:map:0'
kk_relates_to:
  - map-conflict-marker-scan
  - map-revision-bound-passage-correspondence
  - practice-make-conflict-choices-explicit-and-reversible
  - practice-keep-session-captures-immutable-across-revisions
kk_depends_on: []
kk_confidence: medium
---
FileSession::markerSections ties an intact launch conflict frame to the choice sections enclosed by its source regions. A block can enclose several sections when both sides contain the same shared lines around them. A hand edit to marker lines or base text unties the block.

When the last open contribution is decided and every enclosed section has an accepted contribution, the same history entry removes the markers, base text, ignored contributions and duplicate shared lines. Accept alone, ignoring both sides of a section, or ignored content containing manual or pre-launch work keeps the block. Thus an Ignore that completes a marker section can also change result text. Undo restores the text and decisions together.

If deleting rejected text would leave kept sections out of order, the command rebuilds the block in section order. CorrespondenceMap::rebuilt moves kept anchors with their text and leaves ignored anchors empty beside their section. Recovery reproduces the text and decisions.

<!-- kk:related:start -->
# Related

- Related: [map-conflict-marker-scan](map-conflict-marker-scan.md)
- Related: [map-revision-bound-passage-correspondence](map-revision-bound-passage-correspondence.md)
- Related: [practice-make-conflict-choices-explicit-and-reversible](../product/practice-make-conflict-choices-explicit-and-reversible.md)
- Related: [practice-keep-session-captures-immutable-across-revisions](practice-keep-session-captures-immutable-across-revisions.md)
<!-- kk:related:end -->

<!-- kk:citations:start -->
# Citations

[1] 1d4a909e-497a-4b0c-a66a-f79616a21068:map:0
<!-- kk:citations:end -->
