---
type: practice
title: Hold a Git call to test racing repository changes
description: >-
  GitGate holds one git subcommand on one path so a write lands mid-inspection;
  keep a race test only if it fails without its guard.
tags:
  - testing
  - git
  - concurrency
  - mergetool
kk_schema_version: 3
kk_id: practice-hold-a-git-call-to-test-racing-repository-changes
kk_derived_from: []
kk_relates_to:
  - map-recovery-journal-and-acknowledgment-boundary
  - map-mergetool-session-socket-and-real-git-parent
  - practice-fence-session-work-and-serialize-worktree-mutations
kk_depends_on: []
kk_confidence: high
---
[GitGate.h](../../../tests/fixtures/mergetool/GitGate.h) puts a `git` wrapper first on the test process's `PATH`. `close("ls-files")` makes it hold any call with that subcommand whose last argument is the gated path. Each held call leaves a `held.*` file with its arguments and waits until the test calls `release`; then the wrapper runs `/usr/bin/git`. The app finds Git through `PATH`, so a test can change the repository at an exact point in a background inspection and get the same interleaving on every run. Child processes started with their own environment keep their own `PATH`. [MergetoolLauncherTests.cpp](../../../tests/app/MergetoolLauncherTests.cpp) and [MergetoolParentTests.cpp](../../../tests/git/MergetoolParentTests.cpp) use it to land an outside write, a staged path or a new `core.autocrlf` while the copied-launch recheck inspects the index.

The wrapper and its `bin` directory must be owner-only. `secureExecutable` in [GitAdapter.cpp](../../../src/git/GitAdapter.cpp) refuses a Git executable that is group or world writable, owned by another user, or under such a directory. Run a `none` variant that holds Git and changes nothing, so the test shows the wrapper alone does not produce the failure it checks for.

Keep a protective test only if it fails when the comparison it guards is removed. A case that still passes with the guard disabled is redundant, because some earlier check catches the change. For example, staging the path or changing `core.autocrlf` before an in-window recovery choice is already caught by the restore, so that case says nothing about the later recheck. A separate case makes the change while GitGate holds the recheck's index inspection.

<!-- kk:related:start -->
# Related

- Related: [map-recovery-journal-and-acknowledgment-boundary](map-recovery-journal-and-acknowledgment-boundary.md)
- Related: [map-mergetool-session-socket-and-real-git-parent](map-mergetool-session-socket-and-real-git-parent.md)
- Related: [practice-fence-session-work-and-serialize-worktree-mutations](practice-fence-session-work-and-serialize-worktree-mutations.md)
<!-- kk:related:end -->
