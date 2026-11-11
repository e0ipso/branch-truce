---
type: map
title: Installed desktop application
description: >-
  The installed application embeds QML bytecode and includes desktop metadata
  and application icons.
tags:
  - architecture
  - desktop
  - platform
  - cpp
kk_schema_version: 3
kk_id: map-installed-desktop-application
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
A desktop build installs the binary, desktop entry, AppStream metainfo and hicolor SVG/PNG icons under the application ID `com.canpicasoft.branch-truce`, as listed in [docs/build.md](../../../../../docs/build.md). `BRANCH_TRUCE_APP_ID` in [CMakeLists.txt](../../../../../CMakeLists.txt) sets the ID, and [packaging/](../../../../../packaging) holds the files named after it. The QML module is embedded in the executable as bytecode only, for the reason in the [QML bytecode practice](../../packaging/practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins.md), so the installed program needs no QML directory and reads nothing from the source or build tree. [tools/verify-install-prefix.sh](../../../../../tools/verify-install-prefix.sh) builds and installs a disposable prefix and removes the copied sources and build tree before it launches anything. It is registered as the serial ctest `install.prefix`.

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
