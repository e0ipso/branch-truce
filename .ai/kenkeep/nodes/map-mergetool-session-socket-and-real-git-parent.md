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
kk_depends_on: []
kk_confidence: high
---
The GUI and `branch-truce --mergetool` find each other through one lookup, `mergetoolSocketDirectory()` in [MergetoolProtocol.cpp](../../../src/app/MergetoolProtocol.cpp). The socket is named `bt-mt-` plus the first 32 hex characters of the session key. It lives in `$XDG_RUNTIME_DIR/branch-truce` when `XDG_RUNTIME_DIR` is absolute and the whole path fits in 106 bytes. Otherwise it lives in `/tmp/branch-truce-<uid>`. The lookup ignores `TMPDIR`, because a long `TMPDIR` pushes the path past the limit. `sun_path` leaves 107 usable bytes and Qt 6 on Linux reserves one more, so `QLocalServer` rejects a 107-byte path with "Name error". `tools/verify-install-prefix.sh` refuses a fixture path that would push the socket out of the fixture's runtime directory.

The socket is restricted to the same user. `prepareMergetoolSocketDirectory` creates the directory with mode 0700 and refuses one that is not a real directory, belongs to another user or grants group or other access. Before it sends the attach frame, the launcher checks the listener's uid with `SO_PEERCRED`. Do not move the socket into the Linux abstract namespace to get around the length limit. That namespace has no filesystem permissions, so the same-user restriction would be lost. When the session cannot listen, [ApplicationController](../../../src/app/ApplicationController.cpp) sets the startup state to `ready-without-mergetool`, and the detail starts with "Mergetool attach is unavailable:". The files stay open. A launcher for that worktree exits 1, and Git restores `MERGED`.

Only a `resolved` reply makes the launcher exit 0, so that Git stages the file. An explicit leave from `leaveSession` makes it exit 1 without reporting a missing decision. A session dropped by a hard quit makes it print "The launcher closed before an explicit decision." The `git.mergetool-parent` test checks both cases.

[MergetoolParentTests.cpp](../../../tests/git/MergetoolParentTests.cpp) and its fixture [RealParent.h](../../../tests/fixtures/mergetool/RealParent.h) make up `git.mergetool-parent`. It runs the installed `/usr/bin/git mergetool` against the launcher, with `HOME`, the XDG directories and `GIT_CONFIG_GLOBAL` isolated. Only Git 2.43.0 on TUXEDO OS 24.04 has been tested. The compatibility declaration in [docs/mergetool-launcher.md](../../../docs/mergetool-launcher.md) covers only the passing behaviours listed there, and it makes no claim about Kubuntu 26.04. Git runs the tool as `( eval $merge_tool_cmd )` with the value unquoted. The shell splits words and expands globs before `eval`, so a newline or a run of spaces inside quotes becomes a single space. Keep whitespace runs and glob characters out of the launcher path. If `core.fsmonitor` is set during the wait, Mark resolved still writes `MERGED` but then refuses to release the parent. Leave restores the launch bytes.

<!-- kk:related:start -->
# Related

- Related: [practice-treat-mergetool-file-state-as-live-evidence](practice-treat-mergetool-file-state-as-live-evidence.md)
- Related: [map-branch-truce-desktop-implementation-baseline](map-branch-truce-desktop-implementation-baseline.md)
- Related: [practice-keep-unsupported-and-unfinished-work-visible](practice-keep-unsupported-and-unfinished-work-visible.md)
<!-- kk:related:end -->
