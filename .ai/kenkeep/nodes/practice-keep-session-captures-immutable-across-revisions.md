---
type: practice
title: Keep session captures immutable across revisions
description: >-
  Retain source bytes, conversion policy, provenance and the launch result
  separately from editable revision history.
tags:
  - session
  - snapshots
  - editing
  - source-identity
kk_schema_version: 3
kk_id: practice-keep-session-captures-immutable-across-revisions
kk_derived_from: []
kk_relates_to:
  - practice-keep-sources-identifiable-and-the-result-editable
  - practice-fence-session-work-and-serialize-worktree-mutations
  - map-conflict-admission-and-offline-startup
kk_depends_on: []
kk_confidence: high
---
Capture a file's inputs once when opening an admitted merge or rebase. [FileSnapshots](../../../src/core/FileSnapshot.h) retains raw Git stage bytes and object/mode identities, converted worktree bytes and normalized UTF-8. It also retains conversion attributes and configuration, source labels, and verified commit provenance. Capture the launch result's exact bytes, encoding, BOM and newline metadata, and observed file identity. Preserve an existing manual result as the initial editable draft.

[FileSession::snapshots()](../../../src/core/SessionState.h) exposes immutable session-owned captures separately from revisions. Editing, undo/redo, checkpoint discards, and staged-result discards preserve these captures. The originals remain available for comparison as the user's draft and decisions evolve.

Represent unavailable sources with absent optionals so they remain distinct from present zero-byte sources. A captured identity records what was observed. Later publication still requires fresh guards.

[ApplicationController.cpp](../../../src/app/ApplicationController.cpp) assembles the captures after conflict admission. [SessionStateTests.cpp](../../../tests/core/SessionStateTests.cpp) and [StartupRouteTests.cpp](../../../tests/app/StartupRouteTests.cpp) cover retention and missing-versus-empty sources.

<!-- kk:related:start -->
# Related

- Related: [practice-keep-sources-identifiable-and-the-result-editable](practice-keep-sources-identifiable-and-the-result-editable.md)
- Related: [practice-fence-session-work-and-serialize-worktree-mutations](practice-fence-session-work-and-serialize-worktree-mutations.md)
- Related: [map-conflict-admission-and-offline-startup](map-conflict-admission-and-offline-startup.md)
<!-- kk:related:end -->
