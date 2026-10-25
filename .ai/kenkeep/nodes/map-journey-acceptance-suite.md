---
type: map
title: Journey acceptance suite
description: >-
  ctest acceptance.journeys drives the real Main scene offscreen over real Git
  merges and rebases with private XDG dirs and an offline git guard.
tags:
  - testing
  - acceptance
  - qml
  - offscreen
  - git
kk_schema_version: 3
kk_id: map-journey-acceptance-suite
kk_derived_from: []
kk_relates_to:
  - practice-wait-for-layout-before-clicking-offscreen-dialog-items
  - map-branch-truce-first-release-scope
  - practice-make-conflict-choices-explicit-and-reversible
  - map-mergetool-session-socket-and-real-git-parent
kk_depends_on: []
kk_confidence: high
---
[JourneyAcceptanceTests.cpp](../../../tests/acceptance/JourneyAcceptanceTests.cpp) checks the observable acceptance examples in `PRD.md` through the real application. It is the ctest `acceptance.journeys` with the label `acceptance`, so `ctest -L acceptance` runs it. Each journey stops a real merge or rebase on conflicts in a disposable repository, opens it with `ApplicationController::openRepository`, loads the real `BranchTruce` `Main` scene and drives it with mouse clicks and key events. It then compares the scene with the repository and the Git index. ctest sets `QT_QPA_PLATFORM=offscreen` and `QT_QUICK_BACKEND=software`. Run directly, the binary needs both and exits 2 on any other platform plugin.

Before Qt starts, the process points `HOME`, `XDG_CONFIG_HOME`, `XDG_CACHE_HOME`, `XDG_DATA_HOME` and an owner-only `XDG_RUNTIME_DIR` into one temporary directory, so the recovery store and the mergetool socket never touch the real ones. A `git` wrapper first on `PATH` logs the app's arguments and refuses `fetch`, `pull`, `push`, `clone`, `ls-remote`, `remote`, `submodule` and `credential`. The fixtures call `/usr/bin/git` directly, so the log holds only the app's commands. Every journey ends with `checkOffline`, which also rejects visible text about accounts, sign-in, subscriptions, payment or license keys. The guard is a wrapper and proxy variables, with no network namespace, so a socket opened directly by Qt or a library would go unnoticed.

When the result is Git's raw conflict output, Accept and Resolve using one side change only decisions, because both sides' lines are already in the text. The remaining conflict markers then block Mark resolved. A journey that checks text changes from choices has to start from a result the user set before launch, such as the common version.

The window-fit journey resizes the window to 1280x720 and 960x540, the logical sizes of 1920x1080 at 150% and 200%. Its `checkFits` scrolls each control and text pane into view and checks that it lies inside the window, and the journey requires each pane to be at least eight lines high.

Uncovered PRD examples and environment-dependent gaps are listed in [tests/acceptance/README.md](../../../tests/acceptance/README.md). The write-failure journey relies on a read-only directory and fails when the suite runs as root.

<!-- kk:related:start -->
# Related

- Related: [practice-wait-for-layout-before-clicking-offscreen-dialog-items](practice-wait-for-layout-before-clicking-offscreen-dialog-items.md)
- Related: [map-branch-truce-first-release-scope](map-branch-truce-first-release-scope.md)
- Related: [practice-make-conflict-choices-explicit-and-reversible](practice-make-conflict-choices-explicit-and-reversible.md)
- Related: [map-mergetool-session-socket-and-real-git-parent](map-mergetool-session-socket-and-real-git-parent.md)
<!-- kk:related:end -->
