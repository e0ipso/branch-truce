---
type: practice
title: Keep unsupported and unfinished work visible
description: >-
  Show all current files, prominent outside-app warnings, and an honest handoff
  that directs users to outstanding work.
tags:
  - product
  - scope
  - progress
  - handoff
kk_schema_version: 3
kk_id: practice-keep-unsupported-and-unfinished-work-visible
kk_derived_from:
  - PRD.md
  - src/app/SessionModel.cpp
  - src/app/ApplicationController.cpp
  - src/ui/Main.qml
  - docs/session-state.md
kk_relates_to: []
kk_depends_on: []
kk_confidence: medium
---
Present all currently conflicted files together in one session with a useful file list and visible progress. Prominently identify unsupported rename-specific, deletion, image, file-type, permission and conversion cases as outside-app work. A file is not unsupported merely because it was renamed earlier; resolving disputes about moves or destinations remains outside the first release. Never count unsupported work as resolved. Keep its notices visible when supported text work is complete.

[ApplicationController](../../../src/app/ApplicationController.cpp) keeps exact byte-path identities separate from escaped display paths. Select files by that identity so supported files open their own immutable sources and preserved launch result. Keep supported, outside-app and unreadable entries in the same session inventory. A load error retains the file entry and its diagnostic. [Main.qml](../../../src/ui/Main.qml) keeps outside-app reasons visible alongside the current inventory. Preserve the distinction between unavailable and empty content defined by [immutable session captures](practice-keep-session-captures-immutable-across-revisions.md).

Before handoff, distinguish saved app drafts, repository results, outstanding decisions, unsaved changes, review needed, failures and outside-app work. A newer draft than an earlier staged result blocks a successful finish and needs an explicit stale-staged warning. Offer a route to each outstanding file. The user may deliberately leave after a warning without a success claim.

`SessionModel::handoff` puts each file in exactly one category: undecided, saved-draft, review-needed, newer-draft, written-unresolved, published, failed or outside-app. Failed also covers uncertain outcomes, unreadable files and recovery problems. Each entry keeps its blockers and its next action. Outside-app and unreadable files have no app draft, so [SessionState.cpp](../../../src/core/SessionState.cpp) gives their entries only the support blocker plus any recorded failure, never Save, Mark resolved, written or staged blockers, and the file rows in [Main.qml](../../../src/ui/Main.qml) show no draft line for them. The controller adds session blockers for a running resolution and for launched paths Git is still waiting on. `clean` is the only success claim, and `finishSession` succeeds only when the handoff is clean. Closing the window with work outstanding opens the handoff dialog in [Main.qml](../../../src/ui/Main.qml), and each route opens its own file, outside-app files included. `leaveSession` is the deliberate leave. It keeps drafts in the recovery store, sends an explicit leave to every waiting launcher and runs no Git command. It refuses to leave while pending input is not yet in recovery or a resolution is running. See [docs/session-state.md](../../../docs/session-state.md).

[SessionModel](../../../src/app/SessionModel.cpp) builds progress from the current, saved, reviewed, and staged revisions. Saved and reviewed count only when they belong to the current revision. A newer draft after an earlier resolved revision stays unresolved. The status then reads "Earlier result staged; current draft not applied", and the earlier staged revision remains recorded.

Difference navigation and undecided navigation stay inside the selected file. An undecided choice with no result range keeps its source byte range. [ApplicationController](../../../src/app/ApplicationController.cpp) leaves the result caret where it is for that unmapped choice. Unfinished navigation can move onto an outside-app file and leaves that file outstanding. Saved or reviewed files can still be revisited. Hiding the file list in [Main.qml](../../../src/ui/Main.qml) keeps the selection, the result cursor, and the unsaved text. Completing every visible decision does not prove the content is correct.

<!-- kk:citations:start -->
# Citations

[1] [PRD.md](../../../PRD.md)
[2] [src/app/SessionModel.cpp](../../../src/app/SessionModel.cpp)
[3] [src/app/ApplicationController.cpp](../../../src/app/ApplicationController.cpp)
[4] [src/ui/Main.qml](../../../src/ui/Main.qml)
[5] [docs/session-state.md](../../../docs/session-state.md)
<!-- kk:citations:end -->
