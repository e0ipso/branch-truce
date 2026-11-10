---
type: map
title: Native release package artifact flow
description: >-
  Release CI validates target-native package bytes and publishes those artifacts
  through digest manifests.
tags:
  - packaging
  - platform
  - debian
  - containers
  - compatibility
kk_schema_version: 3
kk_id: map-native-release-package-artifact-flow
kk_derived_from: []
kk_relates_to:
  - map-branch-truce-desktop-implementation-baseline
  - practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins
  - map-installed-artifact-validation-in-base-containers
kk_depends_on: []
kk_confidence: high
---
There are two package paths on purpose. `tools/build-artifact.sh` produces the per-base developer artifacts and dated installed-validation evidence. Release CI uses [container-check.sh](../../../../../packaging/scripts/container-check.sh) once per `kubuntu-deb`, `fedora-rpm` and `arch-pkg` target. It selects the candidate version before compilation, validates the native DEB/RPM/pkg.tar.zst in a clean target and publishes exactly those validated bytes through revision/run/version/digest manifests. There is no active Tuxedo release build. [package-info.sh](../../../../../packaging/scripts/package-info.sh) owns native filenames, versions and architectures. A verified published predecessor avoids synthetic compilation; unavailable predecessors use a real synthetic upgrade, while corrupt downloads fail. The compiler cache is optional, bounded to 2 GB per target/architecture/image-input subtree and mounted only during compilation. Configurations and actual toolchain images have separate namespaces. CTest runs with parallel level two and four exclusive gates, including the only lint invocation. See [docs/build.md](../../../../../docs/build.md#native-ci-and-release-packages) for commands, cache statistics and artifact flow. A result from the developer path is not evidence for release bytes. `check-package.sh` requires each name in its `expected_deps` to appear in `Depends` and ignores extra entries. So a dependency added to `control` goes unchecked until the list names it, and a name the package no longer depends on fails the check. Keep the explicit runtime QML/style/plugin dependencies and linked-library requirements aligned with `packaging/debian/control` and the native checker. `libkirigami6` arrives through `qml6-module-org-kde-kirigami`, which is why the target version report queries it.

<!-- kk:related:start -->
# Related

- Related: [map-branch-truce-desktop-implementation-baseline → map-desktop-comparison-and-workflow-integration](../../architecture/desktop-baseline/map-desktop-comparison-and-workflow-integration.md)
- Related: [map-branch-truce-desktop-implementation-baseline → map-desktop-platform-and-implementation-stack](../../architecture/desktop-baseline/map-desktop-platform-and-implementation-stack.md)
- Related: [map-branch-truce-desktop-implementation-baseline → map-editing-and-recovery-resource-envelope](../../architecture/desktop-baseline/map-editing-and-recovery-resource-envelope.md)
- Related: [map-branch-truce-desktop-implementation-baseline → map-installed-desktop-application](../../architecture/desktop-baseline/map-installed-desktop-application.md)
- Related: [practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins](../practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins.md)
- Related: [map-installed-artifact-validation-in-base-containers](../map-installed-artifact-validation-in-base-containers.md)
<!-- kk:related:end -->
