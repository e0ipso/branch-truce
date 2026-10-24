---
type: map
title: Branch Truce desktop implementation baseline
description: >-
  Locked C++/Qt Quick/Kirigami stack for native Linux/KDE on Kubuntu 26.04 LTS; Tuxedo OS compatibility needs validation.
tags:
  - architecture
  - desktop
  - platform
  - cpp
kk_schema_version: 3
kk_id: map-branch-truce-desktop-implementation-baseline
kk_derived_from:
  - TECH_SPEC.md
  - src/ui/ComparisonView.qml
  - src/ui/Main.qml
  - src/app/ApplicationController.cpp
  - docs/editor-integration.md
  - docs/build.md
  - tests/fixtures/performance/evidence.md
kk_relates_to:
  - practice-design-desktop-apps-around-screens-and-components
  - map-branch-truce-first-release-scope
kk_depends_on: []
kk_confidence: high
---
Branch Truce's first desktop implementation targets native Linux and KDE. Kubuntu 26.04 LTS is the named KDE/API baseline, with Qt 6.10.2, KDE Frameworks 6.24.0 and Plasma 6.6. See the [official release notes](https://kubuntu.org/news/kubuntu-26-04-release-notes/). The development host reports Tuxedo OS 24.04.4 on noble, glibc 2.39, GCC 13.3 and Git 2.43.0. Build separate artifacts against each OS base and its runtime dependencies; package compatibility still requires installation tests.

C++20 is the selected implementation language standard, with a GCC 13.3-compatible compiler floor and Git 2.43 compatibility. Direct Qt/KDE access and simpler builds motivate using C++. The user expects AI maintenance, so their personal language familiarity is not the selection criterion.

The locked stack uses Qt 6, Qt Quick/QML, Qt Quick Controls and Kirigami for the interface; KDE HIG and Breeze for desktop conventions; CMake with KDE Extra CMake Modules where needed for builds; the installed Git CLI behind a constrained C++ adapter using asynchronous QProcess; and Qt file APIs for saving and recovery. Keep the application controller, merge engine, Git adapter, and saving/recovery components loosely coupled. Keep Qt-specific UI objects outside the merge engine. There is no Rust bridge, Python runtime or mandatory libgit2 dependency.

[TECH_SPEC.md](../../../TECH_SPEC.md) is the technical design reference and takes precedence over earlier research recommendations. Save checkpoints app-owned drafts only; Mark resolved publishes and verifies Git resolution. Repository-session mode stages directly; mergetool mode assigns the launched path to parent-owned staging and verifies its acknowledgment. First-release conversion/helper restrictions apply before reading sources as well as before staging. Recovery precedes acknowledgment of completed edits; the [versioned recovery journal](map-recovery-journal-and-acknowledgment-boundary.md) defines the implemented layout and commit boundary. The [correspondence engine](map-revision-bound-passage-correspondence.md) maps source passages to result revisions. [ComparisonView](../../../src/ui/ComparisonView.qml) implements the comparison. It has resizable identified source panes around an editable center, optional earlier context, and command-owned direct edits and basic shortcuts. Search is literal and case-sensitive and covers one pane at a time. Wrap lines and space and tab markers start off. The markers are drawn on the left source, the result, the right source, and the earlier version, because Qt Quick ignores `QTextOption::ShowTabsAndSpaces`. [Main](../../../src/ui/Main.qml) collapses the choice list beside that comparison. Full keyboard operation across files, choices, review, and saving remains unimplemented. Physical desktop verification was not run. Mergetool transport, mergetool lifecycle, and target packaging still need implementation evidence, including the missing mergetool parent handoff in [build notes](../../../docs/build.md).

Representative editing and recovery measurements are in [tests/fixtures/performance/evidence.md](../../../tests/fixtures/performance/evidence.md). The run is an offscreen software Release binary. The proposed envelope for later limit work is 4000 lines, 262144-byte lines, 4194304-byte Git reads, and 8388608-byte recovery payloads. Product defaults are unchanged, and that envelope is still later limit work. An exact 8388608-byte Git blob returns `OutputLimit` with an incomplete capture of 8387891 bytes. A 4194304-byte editor line keeps all 4194304 bytes and is not completed because its content width is 0. Changing the locked stack requires an explicit technical-spec decision.

<!-- kk:related:start -->
# Related

- Related: [practice-design-desktop-apps-around-screens-and-components](practice-design-desktop-apps-around-screens-and-components.md)
- Related: [map-branch-truce-first-release-scope](map-branch-truce-first-release-scope.md)
<!-- kk:related:end -->
