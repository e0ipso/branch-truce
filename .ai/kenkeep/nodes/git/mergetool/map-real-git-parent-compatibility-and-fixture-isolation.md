---
type: map
title: Real Git parent compatibility and fixture isolation
description: >-
  Real Git parent tests bound the compatibility claim and require short private
  runtime directories.
tags:
  - mergetool
  - git
  - transport
  - compatibility
kk_schema_version: 3
kk_id: map-real-git-parent-compatibility-and-fixture-isolation
kk_derived_from: []
kk_relates_to:
  - practice-treat-mergetool-file-state-as-live-evidence
  - map-branch-truce-desktop-implementation-baseline
  - practice-keep-unsupported-and-unfinished-work-visible
  - map-journey-acceptance-suite
kk_depends_on: []
kk_confidence: high
---
[MergetoolParentTests.cpp](../../../../../tests/git/MergetoolParentTests.cpp) and its fixture [RealParent.h](../../../../../tests/fixtures/mergetool/RealParent.h) make up `git.mergetool-parent`. It runs the installed `/usr/bin/git mergetool` against the launcher, with `HOME`, the XDG directories and `GIT_CONFIG_GLOBAL` isolated. Only Git 2.43.0 on TUXEDO OS 24.04 has been tested. The compatibility declaration in [docs/mergetool-launcher.md](../../../../../docs/mergetool-launcher.md) covers only the passing behaviours listed there, and it makes no claim about Kubuntu 26.04. Git runs the tool as `( eval $merge_tool_cmd )` with the value unquoted. The shell splits words and expands globs before `eval`, so a newline or a run of spaces inside quotes becomes a single space. Keep whitespace runs and glob characters out of the launcher path. If `core.fsmonitor` is set during the wait, Mark resolved still writes `MERGED` but then refuses to release the parent. Leave restores the launch bytes. The user guide [docs/mergetool.md](../../../../../docs/mergetool.md) gives only the configuration these tests write: `mergetool.truce.cmd` with the `MERGED="$MERGED"`-style assignments, because Git does not export those variables, `trustExitCode = true`, and `keepBackup` and `keepTemporaries` set to false. It marks every other configuration as untested. Keep the guide and the fixture's tool section in step.

The GUI, and every controller that opens a supported repository with current unmerged files, listens on the session socket and creates its directory. The app never removes that directory. A test that does either needs a private owner-only `XDG_RUNTIME_DIR`, as [StartupRouteTests.cpp](../../../../../tests/app/StartupRouteTests.cpp), `MergetoolLauncherTests.cpp`, `RealParent.h` and the [journey acceptance suite](../../ui/map-journey-acceptance-suite.md) set, and a `ScopedEnvironment` that sets it must restore it too. Those tests create it with `QTemporaryDir("/tmp/bt-run-XXXXXX")` rather than under `TMPDIR`, so the socket path stays short under a long `TMPDIR`. They also check that `mergetoolSocketDirectory()` returns `<runtime>/branch-truce` and not the `/tmp/branch-truce-<uid>` fallback. Otherwise each run leaves a 0700 `branch-truce` directory in the real runtime directory or `/tmp/branch-truce-<uid>`. `MarkResolvedTests.cpp` and `RecoveryBoundaryFaultTests.cpp` get the same short owner-only runtime directory from `tests/fixtures/ConflictedRepository.h`, verify socket containment, and restore the caller's XDG_RUNTIME_DIR exactly, including an unset value. Their controller is destroyed before the runtime directory is removed. Their regression checks cover a long TMPDIR so the fallback cannot hide a leaked socket directory.

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
