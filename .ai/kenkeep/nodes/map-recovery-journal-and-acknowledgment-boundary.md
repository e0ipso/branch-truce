---
type: map
title: Recovery journal and acknowledgment boundary
description: >-
  Persist complete text-and-decision transactions before acknowledgment; retain
  failed input and loaded captures for retry.
tags:
  - recovery
  - storage
  - editing
  - failures
kk_schema_version: 3
kk_id: map-recovery-journal-and-acknowledgment-boundary
kk_derived_from: []
kk_relates_to:
  - practice-recover-unfinished-work-without-changing-its-status
  - practice-keep-session-captures-immutable-across-revisions
  - practice-keep-failed-work-outstanding
kk_depends_on: []
kk_confidence: high
---
[SessionModel](../../../src/app/SessionModel.cpp) owns a version 1 recovery store at `QStandardPaths::AppLocalDataLocation/recovery/v1/<session-uuid>/`, independent of the repository and Git index. [SessionRecovery](../../../src/app/SessionRecovery.cpp) serializes immutable sources, launch captures and complete current text-and-decision state, and retains a separate manual Save checkpoint. [RecoveryStore](../../../src/storage/RecoveryStore.cpp) commits and validates content-addressed immutable payloads before appending and synchronizing a sequenced, hash-chained journal frame. See the [journal format](../../../docs/recovery-journal.md).

The model publishes an applied candidate and acknowledges a completed editing transaction only after recovery commits. Undo, redo and discard use the same boundary; composition stays visibly pending until submitted as a command. A write failure leaves the acknowledged revision unchanged and retains pending text and decisions separately for retry. `retryRecovery` uses the same transaction UUID to avoid duplicate appends after an uncertain flush. Automatic recovery does not move the manual Save checkpoint or imply review or Git resolution.

Opening persists captures before editing. If an initial placeholder recovery commit fails, [SessionCoordinator::completeSessionLoad](../../../src/app/SessionCoordinator.cpp) can still replace that load's untouched placeholders with files captured by [ApplicationController](../../../src/app/ApplicationController.cpp). Their text and sources stay available, and retry commits those captures. Editing remains blocked while recovery is pending. A recovery failure alone must not discard loaded files or turn them into empty unreadable placeholders; a writable store opens the same captured file as ready and supported.

`RecoveryStore::read` validates complete frames, sequence, hashes and every payload. It ignores an interrupted final line, but complete corruption or missing payloads return errors; a diagnostic prefix does not constitute a successful restore. A restore caller must still validate the state schema and current worktree, operation, disk and index identities. The guarantee covers application termination after acknowledgment, not machine power loss. Restoration UI, compaction and final operating limits remain separate work.

<!-- kk:related:start -->
# Related

- Related: [practice-recover-unfinished-work-without-changing-its-status](practice-recover-unfinished-work-without-changing-its-status.md)
- Related: [practice-keep-session-captures-immutable-across-revisions](practice-keep-session-captures-immutable-across-revisions.md)
- Related: [practice-keep-failed-work-outstanding](practice-keep-failed-work-outstanding.md)
<!-- kk:related:end -->
