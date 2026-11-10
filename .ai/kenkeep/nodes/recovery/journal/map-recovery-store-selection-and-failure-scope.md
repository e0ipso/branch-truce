---
type: map
title: Recovery store selection and failure scope
description: >-
  Restore choices preserve every store, require explicit selection when
  ambiguous and isolate adopted file failures.
tags:
  - recovery
  - storage
  - editing
  - failures
kk_schema_version: 3
kk_id: map-recovery-store-selection-and-failure-scope
kk_derived_from:
  - docs/recovery-journal.md
  - src/recovery/RecoveryInventory.cpp
  - src/app/SessionModel.h
  - tests/fixtures/performance/evidence.md
  - src/storage/RecoveryStore.cpp
  - src/app/Application.cpp
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
Recovery never guesses between stores and never deletes one. A store whose journal holds only launch captures (`open` records) carries no work. When several inactive stores match the operation, startup sets those aside, so they never make the choice ambiguous. If exactly one remaining store holds app edits, startup adopts it. If two or more do, restoration stays blocked. The current files open in a new store, and the diagnostic lists each session UUID with its journal's last change time. The user picks one with a Restore button in [Main.qml](../../../../../src/ui/Main.qml), backed by the controller's `recoveryChoices` and `chooseRecoverySession`, or by starting the app with `--recovery-session <session-uuid>` from [main.cpp](../../../../../src/app/main.cpp). Both go through the same restore: the chosen store is adopted and every other matching store gets an owner-only `set-aside` marker file, with its journal and payloads left in place. Naming a set-aside session the same way removes its marker and restores it. `chooseRecoverySession` is refused while another window holds a session lock on one of the listed sessions, or while this window's own store already holds app edits, pending input or a running resolution. If the named session is not restored (no match, locked elsewhere, or its marker cannot be removed), no other edited store is adopted in its place. Restoration stays blocked and lists every remaining edited store as a choice, even a single one, and none is set aside. A lone remaining store with only launch captures is still adopted.

Diagnostics from stores that were not adopted remain session details and leave current files editable. An adopted file's disk/index reconciliation failure blocks editing, choices, history and repository mutations only for that file; other files remain editable after another restart. [SessionModel](../../../../../src/app/SessionModel.h) keeps these failures separate from pending input after a failed commit. Any adopted file's recovery failure still prevents session completion, and pending commits retain their shared-store gate. An aborted or older operation's store is retained for inspection. Startup skips a set-aside store unless the user names it. To retire any other store by hand, close all Branch Truce windows and move its complete session directory outside `recovery/v1/`, keeping journal and payloads together. Moving it back makes it available again.

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
<!-- kk:citations:end -->
