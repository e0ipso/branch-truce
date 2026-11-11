---
type: map
title: Installed validation tooling regression suite
description: >-
  Stubbed container and compositor tooling checks reports, failure counts and
  optional-tool skips.
tags:
  - packaging
  - platform
  - validation
  - wayland
  - accessibility
  - compatibility
kk_schema_version: 3
kk_id: map-installed-validation-tooling-regression-suite
kk_derived_from: []
kk_relates_to:
  - map-per-base-debian-artifacts-and-container-inspection
  - map-branch-truce-desktop-implementation-baseline
  - map-journey-acceptance-suite
kk_depends_on: []
kk_confidence: high
---
The ctest `packaging.installed-validation-tooling` runs [tests/packaging/InstalledValidationTooling.sh](../../../../../tests/packaging/InstalledValidationTooling.sh) against stub container engine, swaymsg and grim commands and a fake Atspi module from [AtspiProbeFixture.py](../../../../../tests/packaging/AtspiProbeFixture.py). It needs no Docker, display or D-Bus, and it exits 77, reported as skipped, when `jq` or `python3` is missing. It does not cover the container steps themselves. The host side keeps the exit status of its `run_steps | tee` pipeline instead of letting `set -e` and `pipefail` abort it. After a failed install the report still ends with its `== Result` section and failure count, and the script prints the report path. The ctest asserts both.

<!-- kk:related:start -->
# Related

- Related: [map-per-base-debian-artifacts-and-container-inspection → map-developer-debian-artifact-builds-and-inspection](../artifact-paths/map-developer-debian-artifact-builds-and-inspection.md)
- Related: [map-per-base-debian-artifacts-and-container-inspection → map-native-release-package-artifact-flow](../artifact-paths/map-native-release-package-artifact-flow.md)
- Related: [map-per-base-debian-artifacts-and-container-inspection → map-runtime-qml-and-plugin-package-dependencies](../artifact-paths/map-runtime-qml-and-plugin-package-dependencies.md)
- Related: [map-branch-truce-desktop-implementation-baseline → map-desktop-comparison-and-workflow-integration](../../architecture/desktop-baseline/map-desktop-comparison-and-workflow-integration.md)
- Related: [map-branch-truce-desktop-implementation-baseline → map-desktop-platform-and-implementation-stack](../../architecture/desktop-baseline/map-desktop-platform-and-implementation-stack.md)
- Related: [map-branch-truce-desktop-implementation-baseline → map-editing-and-recovery-resource-envelope](../../architecture/desktop-baseline/map-editing-and-recovery-resource-envelope.md)
- Related: [map-branch-truce-desktop-implementation-baseline → map-installed-desktop-application](../../architecture/desktop-baseline/map-installed-desktop-application.md)
- Related: [map-journey-acceptance-suite](../../ui/map-journey-acceptance-suite.md)
<!-- kk:related:end -->
