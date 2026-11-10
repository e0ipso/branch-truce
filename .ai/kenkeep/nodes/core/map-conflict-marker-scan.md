---
type: map
title: Conflict marker scan
description: >-
  FileSession derives marker counts from complete blocks and stray lines while
  preserving marker-like source text.
tags:
  - git
  - editing
  - markers
  - recovery
kk_schema_version: 3
kk_id: map-conflict-marker-scan
kk_derived_from:
  - '56ab09b6-0c56-46ab-b8f0-695ab329cc97:map:0'
kk_relates_to:
  - map-revision-bound-passage-correspondence
  - map-conflict-marker-sections
  - practice-keep-failed-work-outstanding
kk_depends_on: []
kk_confidence: medium
---
`scanMarkers` in `src/core/ConflictMarkers.cpp` is the conflict-marker recognizer. It returns complete blocks, optional diff3 base markers and stray marker lines, with line and byte ranges. Each complete block and each stray line counts once. `FileSession` derives `unresolvedMarkers` from this scan for each revision and during recovery, rather than trusting a caller-supplied count.

Captured source correspondence identifies marker-like file content, so ordinary source lines such as a Markdown heading underline do not block Mark resolved. Diff3 base text needs the earlier-version context to distinguish its content from Git's separator. Without source evidence the scan remains conservative. The scan supplies both the resolution blocker and the result view; marker-section association separately decides whether choices can safely remove a block.

<!-- kk:related:start -->
# Related

- Related: [map-revision-bound-passage-correspondence](map-revision-bound-passage-correspondence.md)
- Related: [map-conflict-marker-sections](map-conflict-marker-sections.md)
- Related: [practice-keep-failed-work-outstanding](../product/practice-keep-failed-work-outstanding.md)
<!-- kk:related:end -->

<!-- kk:citations:start -->
# Citations

[1] 56ab09b6-0c56-46ab-b8f0-695ab329cc97:map:0
<!-- kk:citations:end -->
