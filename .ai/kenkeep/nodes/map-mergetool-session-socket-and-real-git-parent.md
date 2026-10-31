---
type: map
title: Mergetool session socket and real Git parent
description: >-
  The session socket sits under XDG_RUNTIME_DIR or /tmp/branch-truce-<uid>; real
  Git 2.43 parent tests bound the compatibility claim.
tags:
  - mergetool
  - git
  - transport
  - compatibility
kk_schema_version: 3
kk_id: map-mergetool-session-socket-and-real-git-parent
kk_derived_from: []
kk_relates_to:
  - practice-treat-mergetool-file-state-as-live-evidence
  - map-branch-truce-desktop-implementation-baseline
  - practice-keep-unsupported-and-unfinished-work-visible
  - map-journey-acceptance-suite
kk_depends_on: []
kk_confidence: high
---
The GUI and `branch-truce --mergetool` find each other through one lookup, `mergetoolSocketDirectory()` in [MergetoolProtocol.cpp](../../../src/mergetool/MergetoolProtocol.cpp). The socket is named `bt-mt-` plus the first 32 hex characters of the session key. It lives in `$XDG_RUNTIME_DIR/branch-truce` when `XDG_RUNTIME_DIR` is absolute and the whole path fits in 106 bytes. Otherwise it lives in `/tmp/branch-truce-<uid>`. The lookup ignores `TMPDIR`, because a long `TMPDIR` pushes the path past the limit. `sun_path` leaves 107 usable bytes and Qt 6 on Linux reserves one more, so `QLocalServer` rejects a 107-byte path with "Name error". `tools/verify-install-prefix.sh` refuses a fixture path that would push the socket out of the fixture's runtime directory.

The socket is restricted to the same user. `prepareMergetoolSocketDirectory` creates the directory with mode 0700 and refuses one that is not a real directory, belongs to another user or grants group or other access. Before it sends the attach frame, the launcher checks the listener's uid with `SO_PEERCRED`. Do not move the socket into the Linux abstract namespace to get around the length limit. That namespace has no filesystem permissions, so the same-user restriction would be lost. When the session cannot listen, [ApplicationController](../../../src/app/ApplicationController.cpp) sets the startup state to `ready-without-mergetool`, and the detail starts with "Mergetool attach is unavailable:". The files stay open. A launcher for that worktree exits 1, and Git restores `MERGED`.

Only a `resolved` reply makes the launcher exit 0, so that Git stages the file. An explicit leave from `leaveSession` makes it exit 1 without reporting a missing decision. A session dropped by a hard quit makes it print "The launcher closed before an explicit decision." The `git.mergetool-parent` test checks both cases. A launcher that finds no listener starts the GUI with the hidden `--mergetool-path` option, so the window opens on the path Git launched. `mergetoolWaiting`, `parentStagingPath`, `parentSessionActive` and `canEndParentSession` notify through `mergetoolChanged`. The first two depend on the selected file, so the controller connects `selectedFileChanged` to `mergetoolChanged` and the waiting text follows the selection. The command message and marker blocker, each preview and the resolution state have signals of their own, and a new message does not notify the mergetool state or `handoffChanged`.

A launcher can be waiting when the window reloads its files, for example after an in-window recovery choice. Replacing the files drops the publisher's parent staging, so after every load [ApplicationController](../../../src/app/ApplicationController.cpp) calls `MergetoolSession::reassign()` to hand the waiting launcher's path to the reloaded files. Mark resolved then releases the launcher with the exact object and waits for Git to stage, instead of staging the path in the app. The choice is rebound rather than refused because a launcher-started window is the main way users reach it. Gotcha: installed Git moves `MERGED` aside and copies it back when it launches the tool, so `MERGED` gets a new inode and timestamps with the same bytes. A recovery store captured before that launch sees a copy, not the captured file. The path keeps a recovery error until a waiting launcher's `MERGED` copy holds the captured launch bytes and a background recheck of the index and conversion policy passes; the [recovery journal](map-recovery-journal-and-acknowledgment-boundary.md) lists the conditions. `MergetoolSession` calls `ApplicationController::acceptCopiedLaunches()` on every attach and the controller calls it after every load, so this works after an in-window choice and in a window the launcher started. `git.mergetool-parent` covers the accepted copy, an outside edit, and a staged path or new `core.autocrlf` both before the choice and during the recheck.

[MergetoolParentTests.cpp](../../../tests/git/MergetoolParentTests.cpp) and its fixture [RealParent.h](../../../tests/fixtures/mergetool/RealParent.h) make up `git.mergetool-parent`. It runs the installed `/usr/bin/git mergetool` against the launcher, with `HOME`, the XDG directories and `GIT_CONFIG_GLOBAL` isolated. Only Git 2.43.0 on TUXEDO OS 24.04 has been tested. The compatibility declaration in [docs/mergetool-launcher.md](../../../docs/mergetool-launcher.md) covers only the passing behaviours listed there, and it makes no claim about Kubuntu 26.04. Git runs the tool as `( eval $merge_tool_cmd )` with the value unquoted. The shell splits words and expands globs before `eval`, so a newline or a run of spaces inside quotes becomes a single space. Keep whitespace runs and glob characters out of the launcher path. If `core.fsmonitor` is set during the wait, Mark resolved still writes `MERGED` but then refuses to release the parent. Leave restores the launch bytes. The user guide [docs/mergetool.md](../../../docs/mergetool.md) gives only the configuration these tests write: `mergetool.truce.cmd` with the `MERGED="$MERGED"`-style assignments, because Git does not export those variables, `trustExitCode = true`, and `keepBackup` and `keepTemporaries` set to false. It marks every other configuration as untested. Keep the guide and the fixture's tool section in step.

The GUI, and every controller that opens a supported repository with current unmerged files, listens on the session socket and creates its directory. The app never removes that directory. A test that does either needs a private owner-only `XDG_RUNTIME_DIR`, as [StartupRouteTests.cpp](../../../tests/app/StartupRouteTests.cpp), `MergetoolLauncherTests.cpp`, `RealParent.h` and the [journey acceptance suite](map-journey-acceptance-suite.md) set, and a `ScopedEnvironment` that sets it must restore it too. Those tests create it with `QTemporaryDir("/tmp/bt-run-XXXXXX")` rather than under `TMPDIR`, so the socket path stays short under a long `TMPDIR`. They also check that `mergetoolSocketDirectory()` returns `<runtime>/branch-truce` and not the `/tmp/branch-truce-<uid>` fallback. Otherwise each run leaves a 0700 `branch-truce` directory in the real runtime directory or `/tmp/branch-truce-<uid>`. `ResolutionPublisherTests.cpp` and `RecoveryBoundaryFaultTests.cpp` open a repository without setting `XDG_RUNTIME_DIR`. The fixture directory comes from `QTemporaryDir` under `TMPDIR`, so a long `TMPDIR` pushes the socket path past 106 bytes, the lookup falls back to `/tmp/branch-truce-<uid>`, and that directory stays behind.

<!-- kk:related:start -->
# Related

- Related: [practice-treat-mergetool-file-state-as-live-evidence](practice-treat-mergetool-file-state-as-live-evidence.md)
- Related: [map-branch-truce-desktop-implementation-baseline](map-branch-truce-desktop-implementation-baseline.md)
- Related: [practice-keep-unsupported-and-unfinished-work-visible](practice-keep-unsupported-and-unfinished-work-visible.md)
- Related: [map-journey-acceptance-suite](map-journey-acceptance-suite.md)
<!-- kk:related:end -->
