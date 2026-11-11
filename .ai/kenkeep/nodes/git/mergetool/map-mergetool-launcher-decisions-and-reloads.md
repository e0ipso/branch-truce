---
type: map
title: Mergetool launcher decisions and reloads
description: >-
  Launcher replies control parent staging; reloads reassign waiting paths and
  revalidate copied launch results.
tags:
  - mergetool
  - git
  - transport
  - compatibility
kk_schema_version: 3
kk_id: map-mergetool-launcher-decisions-and-reloads
kk_derived_from: []
kk_relates_to:
  - practice-treat-mergetool-file-state-as-live-evidence
  - map-branch-truce-desktop-implementation-baseline
  - practice-keep-unsupported-and-unfinished-work-visible
  - map-journey-acceptance-suite
kk_depends_on: []
kk_confidence: high
---
Only a `resolved` reply makes the launcher exit 0, so that Git stages the file. An explicit leave from `leaveSession` makes it exit 1 without reporting a missing decision. A session dropped by a hard quit makes it print "The launcher closed before an explicit decision." The `git.mergetool-parent` test checks both cases. A launcher that finds no listener starts the GUI with the hidden `--mergetool-path` option, so the window opens on the path Git launched. `mergetoolWaiting`, `parentStagingPath`, `parentSessionActive` and `canEndParentSession` notify through `mergetoolChanged`. The first two depend on the selected file, so the controller connects `selectedFileChanged` to `mergetoolChanged` and the waiting text follows the selection. The command message and marker blocker, each preview and the resolution state have signals of their own, and a new message does not notify the mergetool state or `handoffChanged`.

A launcher can be waiting when the window reloads its files, for example after an in-window recovery choice. Replacing the files drops the publisher's parent staging, so after every load [SessionLoader](../../../../../src/app/SessionLoader.cpp) calls `MergetoolSession::reassign()` to hand the waiting launcher's path to the reloaded files. Mark resolved then releases the launcher with the exact object and waits for Git to stage, instead of staging the path in the app. The choice is rebound rather than refused because a launcher-started window is the main way users reach it. Gotcha: installed Git moves `MERGED` aside and copies it back when it launches the tool, so `MERGED` gets a new inode and timestamps with the same bytes. A recovery store captured before that launch sees a copy, not the captured file. The path keeps a recovery error until a waiting launcher's `MERGED` copy holds the captured launch bytes and a background recheck of the index and conversion policy passes; the [recovery journal](../../recovery/journal/index.md) lists the conditions. `MergetoolSession` calls `MergetoolSession::acceptCopiedLaunches()` on every attach and SessionLoader calls it after every load, so this works after an in-window choice and in a window the launcher started. `git.mergetool-parent` covers the accepted copy, an outside edit, and a staged path or new `core.autocrlf` both before the choice and during the recheck.

<!-- kk:related:start -->
# Related

- Related: [practice-treat-mergetool-file-state-as-live-evidence](../practice-treat-mergetool-file-state-as-live-evidence.md)
- Related: [map-branch-truce-desktop-implementation-baseline → map-desktop-comparison-and-workflow-integration](../../architecture/desktop-baseline/map-desktop-comparison-and-workflow-integration.md)
- Related: [map-branch-truce-desktop-implementation-baseline → map-desktop-platform-and-implementation-stack](../../architecture/desktop-baseline/map-desktop-platform-and-implementation-stack.md)
- Related: [map-branch-truce-desktop-implementation-baseline → map-editing-and-recovery-resource-envelope](../../architecture/desktop-baseline/map-editing-and-recovery-resource-envelope.md)
- Related: [map-branch-truce-desktop-implementation-baseline → map-installed-desktop-application](../../architecture/desktop-baseline/map-installed-desktop-application.md)
- Related: [practice-keep-unsupported-and-unfinished-work-visible](../../product/practice-keep-unsupported-and-unfinished-work-visible.md)
- Related: [map-journey-acceptance-suite](../../ui/map-journey-acceptance-suite.md)
<!-- kk:related:end -->
