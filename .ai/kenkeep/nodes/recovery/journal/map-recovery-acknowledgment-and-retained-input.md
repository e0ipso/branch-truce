---
type: map
title: Recovery acknowledgment and retained input
description: >-
  One bounded serial worker computes and persists commands; failures retain
  exact pending transactions for retry.
tags:
  - recovery
  - storage
  - editing
  - failures
kk_schema_version: 3
kk_id: map-recovery-acknowledgment-and-retained-input
kk_derived_from:
  - docs/recovery-journal.md
  - src/recovery/RecoveryInventory.cpp
  - src/app/SessionModel.h
  - tests/fixtures/performance/evidence.md
  - src/storage/RecoveryStore.cpp
  - src/app/Application.cpp
  - src/app/main.cpp
  - src/ui/Main.qml
  - 'dcc586bc-c490-4186-893e-e9ac032ec77c:map:1'
kk_relates_to:
  - practice-recover-unfinished-work-without-changing-its-status
  - practice-keep-session-captures-immutable-across-revisions
  - practice-keep-failed-work-outstanding
  - map-mergetool-session-socket-and-real-git-parent
kk_depends_on: []
kk_confidence: high
---
`SessionStore` routes commands and typing through one bounded `RecoveryJournal` worker FIFO. Command computation, encoding and persistence run on the worker; owner-thread publication follows successful durable commit. Ordinary typing is visible before acknowledgment and retains its burst separately until committed. Count and conservative retained-byte admission apply before copying candidates. Refused typing stays pending for retry. Shutdown drains admitted records without an artificial deadline; a blocked filesystem can delay shutdown.

The model publishes an applied candidate and acknowledges a completed editing transaction only after recovery commits. Undo, redo and discard use the same boundary; composition stays visibly pending until submitted as a command. A write failure leaves the acknowledged revision unchanged and retains pending text and decisions separately for retry. `retryRecovery` uses the same transaction UUID to avoid duplicate appends after an uncertain flush. A failed Save keeps one `manual-save` transaction. A failed Discard keeps one `discard-to-checkpoint` transaction. Retry commits that same transaction id, and other commands stay blocked until then. Retry preserves publication state; saving alone does not write or stage repository content. Automatic recovery does not move the manual Save checkpoint or imply review or Git resolution. A recovery payload larger than 8388608 bytes is refused. [RecoveryStore](../../../../../src/storage/RecoveryStore.cpp) fails the commit, names that size, and leaves the acknowledged journal in place. An unread oversized recovery file reports missing or unsafe. Until retry acknowledges the same discard transaction, the editor stays on the restored checkpoint. The message says the restored result is shown until retry finishes it, and the acknowledged draft stays in place until that commit.

Opening persists captures before editing. If an initial placeholder recovery commit fails, [SessionCoordinator::completeRecoveryLoad](../../../../../src/session/SessionCoordinator.cpp) can still replace that load's untouched placeholders with files captured by [SessionLoader](../../../../../src/app/SessionLoader.cpp). Their text and sources stay available, and retry commits those captures. Failed pending commits block dependent commands across the shared recovery store until retry succeeds. A recovery failure alone must not discard loaded files or turn them into empty unreadable placeholders; a writable store opens the same captured file as ready and supported.

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
[9] dcc586bc-c490-4186-893e-e9ac032ec77c:map:1
<!-- kk:citations:end -->
