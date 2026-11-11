---
type: map
title: Desktop platform and implementation stack
description: >-
  C++20, Qt Quick and Kirigami define the native Linux desktop stack and its
  platform baseline.
tags:
  - architecture
  - desktop
  - platform
  - cpp
kk_schema_version: 3
kk_id: map-desktop-platform-and-implementation-stack
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
Branch Truce's first desktop implementation targets native Linux and KDE. Kubuntu 26.04 LTS is the named KDE/API baseline, with Qt 6.10.2, KDE Frameworks 6.24.0 and Plasma 6.6. See the [official release notes](https://kubuntu.org/news/kubuntu-26-04-release-notes/). The development host reports Tuxedo OS 24.04.4 on noble, glibc 2.39, GCC 13.3 and Git 2.43.0. Release targets are Kubuntu DEB, Fedora 44 RPM and pinned Arch Linux pkg.tar.zst, each validated in a clean native container. The [per-base artifact node](../../packaging/artifact-paths/index.md) distinguishes those releases from the historical/developer Kubuntu/Tuxedo DEBs. The user reports that the Kubuntu release DEB installs and works on Tuxedo OS. Container evidence and that report do not establish compatibility across real desktop versions.

C++20 is the selected implementation language standard, with a GCC 13.3-compatible compiler floor and Git 2.43 compatibility. Direct Qt/KDE access and simpler builds motivate using C++. The user expects AI maintenance, so their personal language familiarity is not the selection criterion.

The locked stack uses Qt 6, Qt Quick/QML, Qt Quick Controls and Kirigami for the interface; KDE HIG and Breeze for desktop conventions; CMake with KDE Extra CMake Modules where needed for builds; the installed Git CLI behind a constrained C++ adapter using asynchronous QProcess; and Qt file APIs for saving and recovery. Keep the application controller, merge engine, Git adapter, and saving/recovery components loosely coupled. Keep Qt-specific UI objects outside the merge engine. There is no Rust bridge, Python runtime or mandatory libgit2 dependency.

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
