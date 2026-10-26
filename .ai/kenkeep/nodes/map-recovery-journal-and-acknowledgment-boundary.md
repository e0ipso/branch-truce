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
  - tests/fixtures/performance/evidence.md
  - src/storage/RecoveryStore.cpp
  - src/app/ApplicationController.cpp
  - src/app/main.cpp
  - src/ui/Main.qml
kk_relates_to:
  - practice-recover-unfinished-work-without-changing-its-status
  - practice-keep-session-captures-immutable-across-revisions
  - practice-keep-failed-work-outstanding
  - map-mergetool-session-socket-and-real-git-parent
kk_depends_on: []
kk_confidence: high
---
[SessionModel](../../../src/app/SessionModel.cpp) owns a version 1 recovery store at `QStandardPaths::AppLocalDataLocation/recovery/v1/<session-uuid>/`, independent of the repository and Git index. [SessionRecovery](../../../src/app/SessionRecovery.cpp) serializes immutable sources, launch captures and complete current text-and-decision state, and retains a separate manual Save checkpoint. [RecoveryStore](../../../src/storage/RecoveryStore.cpp) commits and validates content-addressed immutable payloads before appending and synchronizing a sequenced, hash-chained journal frame. See the [journal format](../../../docs/recovery-journal.md).

The model publishes an applied candidate and acknowledges a completed editing transaction only after recovery commits. Undo, redo and discard use the same boundary; composition stays visibly pending until submitted as a command. A write failure leaves the acknowledged revision unchanged and retains pending text and decisions separately for retry. `retryRecovery` uses the same transaction UUID to avoid duplicate appends after an uncertain flush. A failed Save keeps one `manual-save` transaction. A failed Discard keeps one `discard-to-checkpoint` transaction. Retry commits that same transaction id, and other commands stay blocked until then. The retried Save checkpoint leaves written and staged empty. A later restart keeps that checkpoint, and the repository file stays unstaged. Automatic recovery does not move the manual Save checkpoint or imply review or Git resolution. A recovery payload larger than 8388608 bytes is refused. [RecoveryStore](../../../src/storage/RecoveryStore.cpp) fails the commit, names that size, and leaves the acknowledged journal in place. An unread oversized recovery file reports missing or unsafe. Until retry acknowledges the same discard transaction, the editor stays on the restored checkpoint. The message says the restored result is shown until retry finishes it, and the acknowledged draft stays in place until that commit.

Opening persists captures before editing. If an initial placeholder recovery commit fails, [SessionCoordinator::completeSessionLoad](../../../src/app/SessionCoordinator.cpp) can still replace that load's untouched placeholders with files captured by [ApplicationController](../../../src/app/ApplicationController.cpp). Their text and sources stay available, and retry commits those captures. Pending commits block commands across the shared recovery store until retry succeeds. A recovery failure alone must not discard loaded files or turn them into empty unreadable placeholders; a writable store opens the same captured file as ready and supported.

`RecoveryStore::read` validates complete frames, sequence, hashes and every payload. It ignores an interrupted final line, but complete corruption or missing payloads return errors; a diagnostic prefix does not constitute a successful restore. [RecoveryRestore](../../../src/app/RecoveryRestore.cpp) replays schema-1 domain transitions in journal order, compares complete states with their canonical encoding and validates the captured session against the current worktree and operation. Corrupt, unsupported or mismatched records produce diagnostics without a false restoration. Durable discard transactions remain authoritative over earlier retained drafts.

Recovery never guesses between stores and never deletes one. A store whose journal holds only launch captures (`open` records) carries no work. When several inactive stores match the operation, startup sets those aside, so they never make the choice ambiguous. If exactly one remaining store holds app edits, startup adopts it. If two or more do, restoration stays blocked. The current files open in a new store, and the diagnostic lists each session UUID with its journal's last change time. The user picks one with a Restore button in [Main.qml](../../../src/ui/Main.qml), backed by the controller's `recoveryChoices` and `chooseRecoverySession`, or by starting the app with `--recovery-session <session-uuid>` from [main.cpp](../../../src/app/main.cpp). Both go through the same restore: the chosen store is adopted and every other matching store gets an owner-only `set-aside` marker file, with its journal and payloads left in place. Naming a set-aside session the same way removes its marker and restores it. `chooseRecoverySession` is refused while another window holds a session lock on one of the listed sessions, or while this window's own store already holds app edits, pending input or a running resolution. If the named session is not restored (no match, locked elsewhere, or its marker cannot be removed), no other edited store is adopted in its place. Restoration stays blocked and lists every remaining edited store as a choice, even a single one, and none is set aside. A lone remaining store with only launch captures is still adopted.

A store can capture a path before `git mergetool` launches it. Git then replaces `MERGED` with a copy: a new inode and timestamps, same bytes. [RecoveryRestore](../../../src/app/RecoveryRestore.cpp) reports such a file in `copiedLaunches` and still fails it with `copiedLaunchFailure()`. It counts as a copy only when nothing is `written`, the files loaded for the restore already show the copy, and bytes, mode, size, device, index entries and conversion policy all match the launch capture. The error clears only while a launcher waits on that path and the SHA-256 of its `MERGED` copy equals the captured launch bytes (`launchedWithCapturedBytes` in [ApplicationController](../../../src/app/ApplicationController.cpp)). A launcher can attach long after the restore, so `revalidateCopiedLaunch` asks Git for the index entries and conversion policy again on a background task, and `ResolutionPublisher::adoptCopiedLaunch` then makes the copy the launch destination only if the file still has the identity the restore validated. Any change in between, including an outside edit, replaces the error with one saying the file, index or conversion policy changed after recovery validation. Do not clear a copied-launch error from equal bytes alone. See the [journal format](../../../docs/recovery-journal.md).

Diagnostics from stores that were not adopted remain session details and leave current files editable. An adopted file's disk/index reconciliation failure blocks editing, choices, history and repository mutations only for that file; other files remain editable after another restart. [SessionModel](../../../src/app/SessionModel.h) keeps these failures separate from pending input after a failed commit. Any adopted file's recovery failure still prevents session completion, and pending commits retain their shared-store gate. An aborted or older operation's store is retained for inspection. Startup skips a set-aside store unless the user names it. To retire any other store by hand, close all Branch Truce windows and move its complete session directory outside `recovery/v1/`, keeping journal and payloads together. Moving it back makes it available again.

Interrupted publication or staging requires inspection against current disk and index evidence without automatic retry or rollback. Matching result bytes alone cannot establish cleanup or resolution. A recovered staging completion requires the exact reviewed written result, canonical staged object, mode and conversion policy, plus a durable completion observation before resolution becomes visible. Missing completion evidence remains uncertain and outstanding. [RecoveryRestore](../../../src/app/RecoveryRestore.cpp) matches publication and staging completions in journal order by file, kind, and revision. A later success leaves an earlier completed publication completed. An unmatched earlier interruption stays uncertain. The guarantee covers application termination after acknowledgment, not machine power loss; compaction and final operating limits remain separate work.

<!-- kk:related:start -->
# Related

- Related: [practice-recover-unfinished-work-without-changing-its-status](practice-recover-unfinished-work-without-changing-its-status.md)
- Related: [practice-keep-session-captures-immutable-across-revisions](practice-keep-session-captures-immutable-across-revisions.md)
- Related: [practice-keep-failed-work-outstanding](practice-keep-failed-work-outstanding.md)
- Related: [map-mergetool-session-socket-and-real-git-parent](map-mergetool-session-socket-and-real-git-parent.md)
<!-- kk:related:end -->
