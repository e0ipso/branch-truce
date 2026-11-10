---
type: map
title: Mergetool socket lookup and access
description: >-
  GUI and launcher share a short owner-only socket directory with a locked
  random fallback.
tags:
  - mergetool
  - git
  - transport
  - compatibility
kk_schema_version: 3
kk_id: map-mergetool-socket-lookup-and-access
kk_derived_from: []
kk_relates_to:
  - practice-treat-mergetool-file-state-as-live-evidence
  - map-branch-truce-desktop-implementation-baseline
  - practice-keep-unsupported-and-unfinished-work-visible
  - map-journey-acceptance-suite
kk_depends_on: []
kk_confidence: high
---
The GUI and `branch-truce --mergetool` find each other through one lookup, `mergetoolSocketDirectory()` in [MergetoolSocket.cpp](../../../../../src/mergetool/MergetoolSocket.cpp). The socket is named `bt-mt-` plus the first 32 hex characters of the session key. It lives in `$XDG_RUNTIME_DIR/branch-truce` when `XDG_RUNTIME_DIR` is absolute and the whole path fits in 106 bytes. Otherwise it lives in a `/tmp/branch-truce-<uid>-XXXXXX` directory made by `mkdtemp`, which a link at `$XDG_CACHE_HOME/branch-truce/mergetool-runtime` names under a lock, so another user cannot create it first. The lookup ignores `TMPDIR`, because a long `TMPDIR` pushes the path past the limit. `sun_path` leaves 107 usable bytes and Qt 6 on Linux reserves one more, so `QLocalServer` rejects a 107-byte path with "Name error". `tools/verify-install-prefix.sh` refuses a fixture path that would push the socket out of the fixture's runtime directory.

The socket is restricted to the same user. `prepareMergetoolSocketDirectory` creates the directory with mode 0700 and refuses one that is not a real directory, belongs to another user or grants group or other access. Before it sends the attach frame, the launcher checks the listener's uid with `SO_PEERCRED`. Do not move the socket into the Linux abstract namespace to get around the length limit. That namespace has no filesystem permissions, so the same-user restriction would be lost. When the session cannot listen, [Application](../../../../../src/app/Application.cpp) sets the startup state to `ready-without-mergetool`, and the detail starts with "Mergetool attach is unavailable:". The files stay open. A launcher for that worktree exits 1, and Git restores `MERGED`.

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
