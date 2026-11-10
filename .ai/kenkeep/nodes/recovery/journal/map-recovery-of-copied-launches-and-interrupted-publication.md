---
type: map
title: Recovery of copied launches and interrupted publication
description: >-
  Copied launch destinations and interrupted mutations require current evidence
  and durable observations.
tags:
  - recovery
  - storage
  - editing
  - failures
kk_schema_version: 3
kk_id: map-recovery-of-copied-launches-and-interrupted-publication
kk_derived_from:
  - docs/recovery-journal.md
  - src/recovery/RecoveryInventory.cpp
  - src/app/SessionModel.h
  - tests/fixtures/performance/evidence.md
  - src/storage/RecoveryStore.cpp
  - src/app/Application.cpp
  - src/app/main.cpp
  - src/ui/Main.qml
  - 'dcc586bc-c490-4186-893e-e9ac032ec77c:map:2'
kk_relates_to:
  - practice-recover-unfinished-work-without-changing-its-status
  - practice-keep-session-captures-immutable-across-revisions
  - practice-keep-failed-work-outstanding
  - map-mergetool-session-socket-and-real-git-parent
kk_depends_on: []
kk_confidence: high
---
A store can capture a path before `git mergetool` launches it. Git then replaces `MERGED` with a copy: a new inode and timestamps, same bytes. [RecoveryRestore](../../../../../src/recovery/RecoveryInventory.cpp) reports such a file in `copiedLaunches` and still fails it with `copiedLaunchFailure()`. It counts as a copy only when nothing is `written`, the files loaded for the restore already show the copy, and bytes, mode, size, device, index entries and conversion policy all match the launch capture. The error clears only while a launcher waits on that path and the SHA-256 of its `MERGED` copy equals the captured launch bytes (`launchedWithCapturedBytes` in `MergetoolSession`, composed by [Application](../../../../../src/app/Application.cpp)). A launcher can attach long after the restore, so `revalidateCopiedLaunch` asks Git for the index entries and conversion policy again on a background task, and `ResolutionPublisher::adoptCopiedLaunch` then makes the copy the launch destination only if the file still has the identity the restore validated. Any change in between, including an outside edit, replaces the error with one saying the file, index or conversion policy changed after recovery validation. Do not clear a copied-launch error from equal bytes alone. See the [journal format](../../../../../docs/recovery-journal.md).

Interrupted publication or staging requires inspection against current disk and index evidence without automatic retry or rollback. Matching result bytes alone cannot establish cleanup or resolution. A recovered staging completion requires the exact reviewed written result, canonical staged object, mode and conversion policy, plus a durable completion observation before resolution becomes visible. Missing completion evidence remains uncertain and outstanding. [RecoveryRestore](../../../../../src/recovery/RecoveryInventory.cpp) matches publication and staging completions in journal order by file, kind, and revision. A later success leaves an earlier completed publication completed. An unmatched earlier interruption stays uncertain. Acknowledgment covers termination and machine power loss on a filesystem that honors file and directory fsync. Bounded continuation preserves the retained domain checkpoint and outstanding mutation targets; admission failure retains pending input.

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
[9] dcc586bc-c490-4186-893e-e9ac032ec77c:map:2
<!-- kk:citations:end -->
