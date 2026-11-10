---
type: map
title: Desktop comparison and workflow integration
description: >-
  The technical specification defines comparison, app drafts, publication and
  mergetool integration.
tags:
  - architecture
  - desktop
  - platform
  - cpp
kk_schema_version: 3
kk_id: map-desktop-comparison-and-workflow-integration
kk_derived_from:
  - TECH_SPEC.md
  - src/ui/ComparisonView.qml
  - src/ui/Main.qml
  - src/app/Application.cpp
  - docs/editor-integration.md
  - docs/build.md
  - tests/fixtures/performance/evidence.md
  - src/core/ResourceLimits.h
  - docs/resource-limits.md
  - docs/mergetool-launcher.md
  - tests/app/ComparisonViewTests.cpp
  - tools/verify-install-prefix.sh
kk_relates_to:
  - practice-design-desktop-apps-around-screens-and-components
  - map-branch-truce-first-release-scope
  - practice-treat-mergetool-file-state-as-live-evidence
  - map-mergetool-session-socket-and-real-git-parent
  - map-per-base-debian-artifacts-and-container-inspection
  - practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins
kk_depends_on: []
kk_confidence: high
---
[TECH_SPEC.md](../../../../../TECH_SPEC.md) is the technical design reference and takes precedence over earlier research recommendations. Save checkpoints app-owned drafts only; Mark resolved publishes and verifies Git resolution. Repository-session mode stages directly; mergetool mode assigns the launched path to parent-owned staging and verifies its acknowledgment. First-release conversion/helper restrictions apply before reading sources as well as before staging. Recovery precedes acknowledgment of completed edits; the [versioned recovery journal](../../recovery/journal/index.md) defines the implemented layout and commit boundary. The [correspondence engine](../../core/map-revision-bound-passage-correspondence.md) maps source passages to result revisions. [ComparisonView](../../../../../src/ui/ComparisonView.qml) implements the comparison. It has resizable identified source panes around an editable center, optional earlier context, and command-owned direct edits and basic shortcuts. Search is literal and case-sensitive and covers one pane at a time. Wrap lines and space and tab markers start off. The markers are drawn on the left source, the result, the right source, and the earlier version, because Qt Quick ignores `QTextOption::ShowTabsAndSpaces`. [Main](../../../../../src/ui/Main.qml) collapses the choice list beside that comparison. Full keyboard operation across files, choices, review, and saving remains unimplemented. Physical desktop verification was not run. `branch-truce --mergetool` is the blocking launcher for Git's mergetool parent. Its transport and lifecycle are in the [mergetool launcher note](../../../../../docs/mergetool-launcher.md). That note declares compatibility only for the passing behaviours a real Git 2.43.0 parent was tested against on the development host, as described in the [session socket and real-parent node](../../git/mergetool/index.md).

<!-- kk:related:start -->
# Related

- Related: [practice-design-desktop-apps-around-screens-and-components](../practice-design-desktop-apps-around-screens-and-components.md)
- Related: [map-branch-truce-first-release-scope](../../product/map-branch-truce-first-release-scope.md)
- Related: [practice-treat-mergetool-file-state-as-live-evidence](../../git/practice-treat-mergetool-file-state-as-live-evidence.md)
- Related: [map-mergetool-session-socket-and-real-git-parent → map-mergetool-launcher-decisions-and-reloads](../../git/mergetool/map-mergetool-launcher-decisions-and-reloads.md)
- Related: [map-mergetool-session-socket-and-real-git-parent → map-mergetool-socket-lookup-and-access](../../git/mergetool/map-mergetool-socket-lookup-and-access.md)
- Related: [map-mergetool-session-socket-and-real-git-parent → map-real-git-parent-compatibility-and-fixture-isolation](../../git/mergetool/map-real-git-parent-compatibility-and-fixture-isolation.md)
- Related: [map-per-base-debian-artifacts-and-container-inspection → map-developer-debian-artifact-builds-and-inspection](../../packaging/artifact-paths/map-developer-debian-artifact-builds-and-inspection.md)
- Related: [map-per-base-debian-artifacts-and-container-inspection → map-native-release-package-artifact-flow](../../packaging/artifact-paths/map-native-release-package-artifact-flow.md)
- Related: [map-per-base-debian-artifacts-and-container-inspection → map-runtime-qml-and-plugin-package-dependencies](../../packaging/artifact-paths/map-runtime-qml-and-plugin-package-dependencies.md)
- Related: [practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins](../../packaging/practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins.md)
<!-- kk:related:end -->

<!-- kk:citations:start -->
# Citations

[1] [TECH_SPEC.md](../../../../../TECH_SPEC.md)
[2] [src/ui/ComparisonView.qml](../../../../../src/ui/ComparisonView.qml)
[3] [src/ui/Main.qml](../../../../../src/ui/Main.qml)
[4] [src/app/Application.cpp](../../../../../src/app/Application.cpp)
[5] [docs/editor-integration.md](../../../../../docs/editor-integration.md)
[6] [docs/build.md](../../../../../docs/build.md)
[7] [tests/fixtures/performance/evidence.md](../../../../../tests/fixtures/performance/evidence.md)
[8] [src/core/ResourceLimits.h](../../../../../src/core/ResourceLimits.h)
[9] [docs/resource-limits.md](../../../../../docs/resource-limits.md)
[10] [docs/mergetool-launcher.md](../../../../../docs/mergetool-launcher.md)
[11] [tests/app/ComparisonViewTests.cpp](../../../../../tests/app/ComparisonViewTests.cpp)
[12] [tools/verify-install-prefix.sh](../../../../../tools/verify-install-prefix.sh)
<!-- kk:citations:end -->
