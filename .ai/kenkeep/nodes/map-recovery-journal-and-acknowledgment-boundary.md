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
kk_derived_from:
  - docs/recovery-journal.md
  - src/app/RecoveryRestore.cpp
  - src/app/SessionModel.h
kk_relates_to:
  - practice-recover-unfinished-work-without-changing-its-status
  - practice-keep-session-captures-immutable-across-revisions
  - practice-keep-failed-work-outstanding
kk_depends_on: []
kk_confidence: high
---
[SessionModel](../../../src/app/SessionModel.cpp) owns a version 1 recovery store at `QStandardPaths::AppLocalDataLocation/recovery/v1/<session-uuid>/`, independent of the repository and Git index. [SessionRecovery](../../../src/app/SessionRecovery.cpp) serializes immutable sources, launch captures and complete current text-and-decision state, and retains a separate manual Save checkpoint. [RecoveryStore](../../../src/storage/RecoveryStore.cpp) commits and validates content-addressed immutable payloads before appending and synchronizing a sequenced, hash-chained journal frame. See the [journal format](../../../docs/recovery-journal.md).

The model publishes an applied candidate and acknowledges a completed editing transaction only after recovery commits. Undo, redo and discard use the same boundary; composition stays visibly pending until submitted as a command. A write failure leaves the acknowledged revision unchanged and retains pending text and decisions separately for retry. `retryRecovery` uses the same transaction UUID to avoid duplicate appends after an uncertain flush. Automatic recovery does not move the manual Save checkpoint or imply review or Git resolution.

Opening persists captures before editing. If an initial placeholder recovery commit fails, [SessionCoordinator::completeSessionLoad](../../../src/app/SessionCoordinator.cpp) can still replace that load's untouched placeholders with files captured by [ApplicationController](../../../src/app/ApplicationController.cpp). Their text and sources stay available, and retry commits those captures. Pending commits block commands across the shared recovery store until retry succeeds. A recovery failure alone must not discard loaded files or turn them into empty unreadable placeholders; a writable store opens the same captured file as ready and supported.

`RecoveryStore::read` validates complete frames, sequence, hashes and every payload. It ignores an interrupted final line, but complete corruption or missing payloads return errors; a diagnostic prefix does not constitute a successful restore. [RecoveryRestore](../../../src/app/RecoveryRestore.cpp) replays schema-1 domain transitions in journal order, compares complete states with their canonical encoding and validates the captured session against the current worktree and operation. Startup adopts exactly one matching inactive store. Corrupt, unsupported or mismatched records and ambiguous inventories produce diagnostics without a false restoration. Durable discard transactions remain authoritative over earlier retained drafts.

Diagnostics from stores that were not adopted remain session details and leave current files editable. An adopted file's disk/index reconciliation failure blocks editing, choices, history and repository mutations only for that file; other files remain editable after another restart. [SessionModel](../../../src/app/SessionModel.h) keeps these failures separate from pending input after a failed commit. Any adopted file's recovery failure still prevents session completion, and pending commits retain their shared-store gate. After closing all Branch Truce windows, an old store can be retired reversibly by moving its complete session directory outside `recovery/v1/`, keeping journal and payloads together.

Interrupted publication or staging requires inspection against current disk and index evidence without automatic retry or rollback. Matching result bytes alone cannot establish cleanup or resolution. A recovered staging completion requires the exact reviewed written result, canonical staged object, mode and conversion policy, plus a durable completion observation before resolution becomes visible. Missing completion evidence remains uncertain and outstanding. The guarantee covers application termination after acknowledgment, not machine power loss; compaction and final operating limits remain separate work.

<!-- kk:related:start -->
# Related

- Related: [practice-recover-unfinished-work-without-changing-its-status](practice-recover-unfinished-work-without-changing-its-status.md)
- Related: [practice-keep-session-captures-immutable-across-revisions](practice-keep-session-captures-immutable-across-revisions.md)
- Related: [practice-keep-failed-work-outstanding](practice-keep-failed-work-outstanding.md)
<!-- kk:related:end -->
