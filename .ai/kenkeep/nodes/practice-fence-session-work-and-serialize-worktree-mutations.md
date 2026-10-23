---
type: practice
title: Fence session work and serialize worktree mutations
description: >-
  Fence background completions by session and revision; serialize worktree
  mutations through cancellation reconciliation.
tags:
  - architecture
  - concurrency
  - cancellation
  - git
  - desktop
kk_schema_version: 3
kk_id: practice-fence-session-work-and-serialize-worktree-mutations
kk_derived_from: []
kk_relates_to:
  - map-branch-truce-desktop-implementation-baseline
  - practice-keep-failed-work-outstanding
kk_depends_on: []
kk_confidence: high
---
Route expensive app work through [SessionCoordinator](../../../src/app/SessionCoordinator.h). Worker and inspection callbacks consume value snapshots without capturing GUI objects; completion runs on the model's event-loop thread to keep the interface responsive.

Apply a completion only while its session generation, canonical worktree, operation, file identity, and input/result/decision revisions match current work. Session replacement or revision edits fence out earlier results. Use the fenced `Completion` callback for current-model changes; `taskFinished` remains an audit notification for stale or cancelled outcomes.

Use `submitMutation` for app-owned filesystem mutations and `submitGit` for Git staging. Both share a FIFO queue per canonical worktree root, including symlink aliases. A mutating worker calls `TaskControl::beginMutation` immediately before its first side effect and leaves the destination unchanged if admission returns false.

Cancellation does not prove rollback. Keep an admitted mutation's queue slot through inspection, including cancellation or session replacement. Retain its original task stamp and observed outcome in `mutationOutcomes`, and preserve `Uncertain` when inspection cannot establish what happened. The [implementation](../../../src/app/SessionCoordinator.cpp) and [regression coverage](../../../tests/app/SessionCoordinatorTests.cpp) define the fencing, queue, and cancellation contract.

<!-- kk:related:start -->
# Related

- Related: [map-branch-truce-desktop-implementation-baseline](map-branch-truce-desktop-implementation-baseline.md)
- Related: [practice-keep-failed-work-outstanding](practice-keep-failed-work-outstanding.md)
<!-- kk:related:end -->
