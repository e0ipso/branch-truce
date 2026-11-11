---
type: map
title: Recovery journal format and validation
description: >-
  Version 3 frames and complete domain checkpoints preserve retained history and
  reject invalid recovery.
tags:
  - recovery
  - storage
  - editing
  - failures
kk_schema_version: 3
kk_id: map-recovery-journal-format-and-validation
kk_derived_from:
  - docs/recovery-journal.md
  - src/recovery/RecoveryInventory.cpp
  - src/app/SessionModel.h
  - tests/fixtures/performance/evidence.md
  - src/storage/RecoveryStore.cpp
  - src/app/Application.cpp
  - src/app/main.cpp
  - src/ui/Main.qml
  - 'dcc586bc-c490-4186-893e-e9ac032ec77c:map:0'
kk_relates_to:
  - practice-recover-unfinished-work-without-changing-its-status
  - practice-keep-session-captures-immutable-across-revisions
  - practice-keep-failed-work-outstanding
  - map-mergetool-session-socket-and-real-git-parent
kk_depends_on: []
kk_confidence: high
---
`SessionStore` owns a `RecoveryJournal` at `<GenericDataLocation>/Branch Truce/recovery/v1/<session-uuid>/`, independent of the repository and Git index. `RecoveryRecord` encodes immutable captures and schema-3 domain transactions; `RecoveryStore` owns version-3 sequenced hash-chained frames and content-addressed payloads. Hand-edited anchors and ranges are explicit fields. `RecoveryReplay` validates canonical domain transitions; `RecoveryInventory` validates candidates against the current worktree and operation. A diagnostic prefix is never a successful restore.

Checkpoint schema 2 captures complete retained history, cursor, publication references, outcomes, discard fence, review Save eligibility and unresolved mutation targets. `core/SessionCheckpoint.h` validates all supported drafts before rebuilding correspondence, including no-map drafts. A saved revision older than its review cannot authorize publication. Pending transactions retain the configured correspondence algorithm for continuation, self-validation and replay; mismatched identities are refused.

Continuation synchronizes a replacement head before reclaiming unreachable payloads and preserves exact uncertain retry identities. Readers and writers enforce the same storage and expanded-state limits. A live checkpoint that exceeds admission fails visibly and retains pending input. See `docs/recovery-journal.md` for concrete limits and the fsync acknowledgment contract.

<!-- kk:related:start -->
# Related

- Related: [practice-recover-unfinished-work-without-changing-its-status](../../product/practice-recover-unfinished-work-without-changing-its-status.md)
- Related: [practice-keep-session-captures-immutable-across-revisions](../../core/practice-keep-session-captures-immutable-across-revisions.md)
- Related: [practice-keep-failed-work-outstanding](../../product/practice-keep-failed-work-outstanding.md)
- Related: [map-mergetool-session-socket-and-real-git-parent → map-mergetool-launcher-decisions-and-reloads](../../git/mergetool/map-mergetool-launcher-decisions-and-reloads.md)
- Related: [map-mergetool-session-socket-and-real-git-parent → map-mergetool-socket-lookup-and-access](../../git/mergetool/map-mergetool-socket-lookup-and-access.md)
- Related: [map-mergetool-session-socket-and-real-git-parent → map-real-git-parent-compatibility-and-fixture-isolation](../../git/mergetool/map-real-git-parent-compatibility-and-fixture-isolation.md)
<!-- kk:related:end -->

<!-- kk:citations:start -->
# Citations

[1] [docs/recovery-journal.md](../../../../../docs/recovery-journal.md)
[2] [src/recovery/RecoveryInventory.cpp](../../../../../src/recovery/RecoveryInventory.cpp)
[3] [src/app/SessionModel.h](../../../../../src/app/SessionModel.h)
[4] [tests/fixtures/performance/evidence.md](../../../../../tests/fixtures/performance/evidence.md)
[5] [src/storage/RecoveryStore.cpp](../../../../../src/storage/RecoveryStore.cpp)
[6] [src/app/Application.cpp](../../../../../src/app/Application.cpp)
[7] [src/app/main.cpp](../../../../../src/app/main.cpp)
[8] [src/ui/Main.qml](../../../../../src/ui/Main.qml)
[9] dcc586bc-c490-4186-893e-e9ac032ec77c:map:0
<!-- kk:citations:end -->
