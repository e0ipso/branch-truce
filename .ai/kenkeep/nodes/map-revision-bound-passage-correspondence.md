---
type: map
title: Revision-bound passage correspondence
description: >-
  Map immutable source ranges to one result revision; reject uncertain targets
  and keep alignment memory linear.
tags:
  - comparison
  - editing
  - revisions
  - failures
kk_schema_version: 3
kk_id: map-revision-bound-passage-correspondence
kk_derived_from: []
kk_relates_to:
  - practice-keep-session-captures-immutable-across-revisions
  - practice-make-conflict-choices-explicit-and-reversible
  - practice-keep-failed-work-outstanding
kk_depends_on: []
kk_confidence: high
---
[ExactLineCorrespondence](../../../src/core/Correspondence.h) deterministically compares exact UTF-8 lines, including newline bytes. It accepts equal mappings only where every optimal alignment agrees, so repeated passages do not justify redirecting a choice to another occurrence. Identical buffers retain individually addressable positions. See the [correspondence contract](../../../src/core/Correspondence.md).

[ResultAnchor](../../../src/core/Correspondence.h) binds an immutable source contribution to its session, exact file path, source-object identity and one [RevisionId](../../../src/core/RevisionId.h). [FileSession::replaceMapped](../../../src/core/SessionState.cpp) validates the registered anchor and current revision before changing text or decisions; stale, ambiguous and invalidated targets leave both unchanged. `editResult` transforms valid anchors across exact byte edits. Replacing an overlapping manual edit requires confirmation for that preview, and unrelated manual content stays intact. Undo and redo restore mapping, text and decisions together with a fresh revision, so old previews remain stale.

The [alignment implementation](../../../src/core/Correspondence.cpp) releases Hirschberg length rows before recursion, using O(source lines + result lines) space and O(source lines * result lines) time. It allocates no source-lines by result-lines matrix. Representative latency measurements and final resource limits remain open.

An initial correspondence exception marks only that `FileSession` unreadable with a path-specific diagnostic. It retains captured sources and the launch result while other files finish loading. Correspondence allocation failures during later edits occur before publication of a new revision.

<!-- kk:related:start -->
# Related

- Related: [practice-keep-session-captures-immutable-across-revisions](practice-keep-session-captures-immutable-across-revisions.md)
- Related: [practice-make-conflict-choices-explicit-and-reversible](practice-make-conflict-choices-explicit-and-reversible.md)
- Related: [practice-keep-failed-work-outstanding](practice-keep-failed-work-outstanding.md)
<!-- kk:related:end -->
