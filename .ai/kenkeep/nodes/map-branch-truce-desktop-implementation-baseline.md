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
Branch Truce's first desktop implementation targets native Linux and KDE. Kubuntu 26.04 LTS is the named KDE/API baseline, with Qt 6.10.2, KDE Frameworks 6.24.0 and Plasma 6.6. See the [official release notes](https://kubuntu.org/news/kubuntu-26-04-release-notes/). The development host reports Tuxedo OS 24.04.4 on noble, glibc 2.39, GCC 13.3 and Git 2.43.0. Each OS base gets its own `.deb`, built and inspected in a container of that base, as the [per-base artifact node](map-per-base-debian-artifacts-and-container-inspection.md) describes. Compatibility on the real desktops still requires installation tests.

C++20 is the selected implementation language standard, with a GCC 13.3-compatible compiler floor and Git 2.43 compatibility. Direct Qt/KDE access and simpler builds motivate using C++. The user expects AI maintenance, so their personal language familiarity is not the selection criterion.

The locked stack uses Qt 6, Qt Quick/QML, Qt Quick Controls and Kirigami for the interface; KDE HIG and Breeze for desktop conventions; CMake with KDE Extra CMake Modules where needed for builds; the installed Git CLI behind a constrained C++ adapter using asynchronous QProcess; and Qt file APIs for saving and recovery. Keep the application controller, merge engine, Git adapter, and saving/recovery components loosely coupled. Keep Qt-specific UI objects outside the merge engine. There is no Rust bridge, Python runtime or mandatory libgit2 dependency.

[TECH_SPEC.md](../../../TECH_SPEC.md) is the technical design reference and takes precedence over earlier research recommendations. Save checkpoints app-owned drafts only; Mark resolved publishes and verifies Git resolution. Repository-session mode stages directly; mergetool mode assigns the launched path to parent-owned staging and verifies its acknowledgment. First-release conversion/helper restrictions apply before reading sources as well as before staging. Recovery precedes acknowledgment of completed edits; the [versioned recovery journal](map-recovery-journal-and-acknowledgment-boundary.md) defines the implemented layout and commit boundary. The [correspondence engine](map-revision-bound-passage-correspondence.md) maps source passages to result revisions. [ComparisonView](../../../src/ui/ComparisonView.qml) implements the comparison. It has resizable identified source panes around an editable center, optional earlier context, and command-owned direct edits and basic shortcuts. Search is literal and case-sensitive and covers one pane at a time. Wrap lines and space and tab markers start off. The markers are drawn on the left source, the result, the right source, and the earlier version, because Qt Quick ignores `QTextOption::ShowTabsAndSpaces`. [Main](../../../src/ui/Main.qml) collapses the choice list beside that comparison. Full keyboard operation across files, choices, review, and saving remains unimplemented. Physical desktop verification was not run. `branch-truce --mergetool` is the blocking launcher for Git's mergetool parent. Its transport and lifecycle are in the [mergetool launcher note](../../../docs/mergetool-launcher.md). That note declares compatibility only for the passing behaviours a real Git 2.43.0 parent was tested against on the development host, as described in the [session socket and real-parent node](map-mergetool-session-socket-and-real-git-parent.md).

A desktop build installs four files under the application ID `local.branch_truce.BranchTruce`: `bin/branch-truce`, a desktop entry, AppStream metainfo and a hicolor scalable SVG icon, as listed in [docs/build.md](../../../docs/build.md). `BRANCH_TRUCE_APP_ID` in [CMakeLists.txt](../../../CMakeLists.txt) sets the ID, and [packaging/](../../../packaging/) holds the files named after it. The ID comes from the placeholder domain `branch-truce.local` and stays provisional until the user settles the ID, homepage, developer and license. The QML module is embedded in the executable as bytecode only, for the reason in the [QML bytecode practice](practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins.md), so the installed program needs no QML directory and reads nothing from the source or build tree. [tools/verify-install-prefix.sh](../../../tools/verify-install-prefix.sh) builds and installs a disposable prefix and removes the copied sources and build tree before it launches anything. It is registered as the serial ctest `install.prefix`.

Representative editing and recovery measurements are in [tests/fixtures/performance/evidence.md](../../../tests/fixtures/performance/evidence.md). The run is an offscreen software Release binary. [ResourceLimits.h](../../../src/core/ResourceLimits.h) enforces that envelope: 4000 lines, 262144-byte lines, 4194304-byte Git reads, and 8388608-byte recovery payloads. See [docs/resource-limits.md](../../../docs/resource-limits.md). Oversized work fails explicitly and the previous buffer stays. Each Git phase starts a fresh 4194304-byte budget. A rejected insertion at the 4000-line limit leaves the editor usable. The draft, journal, and editor stay in place, the limit message is shown, a queued event still runs, and replacing the first character still applies. Qt reports Backspace and Delete with a control character in `text()`, so `ApplicationController::eventFilter` excludes those keys from its insertion prediction. Both keys shorten a 262144-byte line or an 8388608-byte buffer, and an insertion past either limit is still rejected. [ComparisonViewTests.cpp](../../../tests/app/ComparisonViewTests.cpp) covers both limits. Changing the locked stack requires an explicit technical-spec decision.

<!-- kk:related:start -->
# Related

- Related: [practice-design-desktop-apps-around-screens-and-components](practice-design-desktop-apps-around-screens-and-components.md)
- Related: [map-branch-truce-first-release-scope](map-branch-truce-first-release-scope.md)
- Related: [practice-treat-mergetool-file-state-as-live-evidence](practice-treat-mergetool-file-state-as-live-evidence.md)
- Related: [map-mergetool-session-socket-and-real-git-parent](map-mergetool-session-socket-and-real-git-parent.md)
- Related: [map-per-base-debian-artifacts-and-container-inspection](map-per-base-debian-artifacts-and-container-inspection.md)
- Related: [practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins](practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins.md)
<!-- kk:related:end -->

<!-- kk:citations:start -->
# Citations

[1] [TECH_SPEC.md](../../../TECH_SPEC.md)
[2] [src/ui/ComparisonView.qml](../../../src/ui/ComparisonView.qml)
[3] [src/ui/Main.qml](../../../src/ui/Main.qml)
[4] [src/app/ApplicationController.cpp](../../../src/app/ApplicationController.cpp)
[5] [docs/editor-integration.md](../../../docs/editor-integration.md)
[6] [docs/build.md](../../../docs/build.md)
[7] [tests/fixtures/performance/evidence.md](../../../tests/fixtures/performance/evidence.md)
[8] [src/core/ResourceLimits.h](../../../src/core/ResourceLimits.h)
[9] [docs/resource-limits.md](../../../docs/resource-limits.md)
[10] [docs/mergetool-launcher.md](../../../docs/mergetool-launcher.md)
[11] [tests/app/ComparisonViewTests.cpp](../../../tests/app/ComparisonViewTests.cpp)
[12] [tools/verify-install-prefix.sh](../../../tools/verify-install-prefix.sh)
<!-- kk:citations:end -->
