---
type: map
title: Journey acceptance suite
description: >-
  CTest acceptance groups drive Main offscreen over real Git merges and rebases
  with private XDG directories and an offline Git guard.
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
[JourneyAcceptanceTests.cpp](../../../../tests/acceptance/JourneyAcceptanceTests.cpp) checks the observable acceptance examples in `PRD.md` through the real application. The CTest entries `acceptance.journeys-review`, `acceptance.journeys-handoff` and `acceptance.journeys-window` share the label `acceptance`, so `ctest -L acceptance` runs all three. Each journey stops a real merge or rebase on conflicts in a disposable repository, opens it with `SessionLoader::openRepository`, loads the real `BranchTruce` `Main` scene and drives it with mouse clicks and key events. It then compares the scene with the repository and the Git index. CTest sets `QT_QPA_PLATFORM=offscreen` and `QT_QUICK_BACKEND=software`; the window group also selects `org.kde.desktop`. Run directly, the binary needs both and exits 2 on any other platform plugin.

Before Qt starts, the process points `HOME`, `XDG_CONFIG_HOME`, `XDG_CACHE_HOME`, `XDG_DATA_HOME` and an owner-only `XDG_RUNTIME_DIR` into one temporary directory, so the recovery store and the mergetool socket never touch the real ones. A `git` wrapper first on `PATH` logs the app's arguments and refuses `fetch`, `pull`, `push`, `clone`, `ls-remote`, `remote`, `submodule` and `credential`. The fixtures call `/usr/bin/git` directly, so the log holds only the app's commands. Every journey ends with `checkOffline`, which also rejects visible text about accounts, sign-in, subscriptions, payment or license keys. The guard is a wrapper and proxy variables, with no network namespace, so a socket opened directly by Qt or a library would go unnoticed.

When the result is Git's raw conflict output, Accept and Resolve using one side change only decisions, because both sides' lines are already in the text. The remaining conflict markers then block Mark resolved. A journey that checks text changes from choices has to start from a result the user set before launch, such as the common version.

The window-fit journey retains strict 1280x720 eight-row pane height and control/menu reachability requirements, and 1280x800 sidebar no-wrap rows and handoff whole-word text requirements. The sidebar's [FileRow.qml](../../../../src/ui/FileRow.qml) elides one-line path and status text; [HandoffRow.qml](../../../../src/ui/HandoffRow.qml) keeps its prose wrapped between words. Source choices overlay the inward pane edges on hover or keyboard/menu focus, reserving neither a row nor a column. Actual laid-out geometry, overflow menus and page scrolling must be exercised; grid-unit formulas do not establish a measured fit. Only strict 960x540 fit has the owner's temporary waiver. First-undecided navigation at that size and both 1280 obligations remain required. Fit evidence must match the current controls, native fonts and layout inputs.

Controls have stable `objectName` locators; per-range actions include `choiceAccept-<id>` and menus use `choiceMore-<owner id>`. Precise line and order-aware actions remain menu entries. Fully decided ranges remove their chrome and retain an editor-anchored menu. `clickChoice` uses real hover targets or `sourceLineMenu` to open the source editor menu; precise commands remain reachable without a persistent menu button. Public right-click, hold and keyboard routing require current runtime evidence when changed. [tests/acceptance/README.md](../../../../tests/acceptance/README.md) records the locator contract and remaining obligations. `namedButton` in [tests/fixtures/QuickScene.h](../../../../tests/fixtures/QuickScene.h) falls back to accessible names for unnamed native controls. Assert state through properties such as `checked`; a conflict count and an undecided-decision count are independent.

Uncovered PRD examples and environment-dependent gaps are listed in [tests/acceptance/README.md](../../../../tests/acceptance/README.md). The write-failure journey relies on a read-only directory and fails when the suite runs as root.

<!-- kk:related:start -->
# Related

- Related: [practice-wait-for-layout-before-clicking-offscreen-dialog-items](practice-wait-for-layout-before-clicking-offscreen-dialog-items.md)
- Related: [map-branch-truce-first-release-scope](../product/map-branch-truce-first-release-scope.md)
- Related: [practice-make-conflict-choices-explicit-and-reversible](../product/practice-make-conflict-choices-explicit-and-reversible.md)
- Related: [map-mergetool-session-socket-and-real-git-parent → map-mergetool-launcher-decisions-and-reloads](../git/mergetool/map-mergetool-launcher-decisions-and-reloads.md)
- Related: [map-mergetool-session-socket-and-real-git-parent → map-mergetool-socket-lookup-and-access](../git/mergetool/map-mergetool-socket-lookup-and-access.md)
- Related: [map-mergetool-session-socket-and-real-git-parent → map-real-git-parent-compatibility-and-fixture-isolation](../git/mergetool/map-real-git-parent-compatibility-and-fixture-isolation.md)
<!-- kk:related:end -->
