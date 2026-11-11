---
type: map
title: Native package build entry point
description: >-
  One format-driven container entry point runs desktop tests, package
  construction and clean-target inspection.
tags:
  - packaging
  - platform
  - debian
  - containers
  - compatibility
kk_schema_version: 3
kk_id: map-developer-debian-artifact-builds-and-inspection
kk_derived_from:
  - 'dcc586bc-c490-4186-893e-e9ac032ec77c:map:4'
kk_relates_to:
  - map-branch-truce-desktop-implementation-baseline
  - practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins
  - map-installed-artifact-validation-in-base-containers
kk_depends_on: []
kk_confidence: high
---
`packaging/scripts/container-check.sh` is the host entry point for `kubuntu-deb`, `fedora-rpm`, `arch-pkg` or `all`. Format metadata comes from `packaging/formats.tsv` and `package-info.sh`; per-format adapters supply native package commands. `packaging/ci/*.Dockerfile` define target, build and test images. There is no separate Tuxedo release artifact. `docs/target-platforms.md` distinguishes current release targets from historical per-base measurements.

Each format runs image preparation, desktop tests, candidate/predecessor packaging and clean-target inspection, retaining explicit stage failures. The runtime fixture contains package-checking inputs and branding, without application sources, build trees or compiler caches. `check-package.sh` and native adapters validate metadata, dependency/integrity, installed version and smoke loading, upgrade and removal. `packaging/ci/baseline.sh` records resolved OS, toolchain and package-tool versions. `CONTAINER_ENGINE` selects the engine.

`tests/packaging/NativeIntegration.sh`, `PackageChecker.sh` and `PackageAdapters.sh` exercise host contracts with controlled collaborators. Clean container checks do not establish physical desktop compatibility; installed UI validation has its own evidence contract in `docs/platform-validation.md`.

<!-- kk:related:start -->
# Related

- Related: [map-branch-truce-desktop-implementation-baseline → map-desktop-comparison-and-workflow-integration](../../architecture/desktop-baseline/map-desktop-comparison-and-workflow-integration.md)
- Related: [map-branch-truce-desktop-implementation-baseline → map-desktop-platform-and-implementation-stack](../../architecture/desktop-baseline/map-desktop-platform-and-implementation-stack.md)
- Related: [map-branch-truce-desktop-implementation-baseline → map-editing-and-recovery-resource-envelope](../../architecture/desktop-baseline/map-editing-and-recovery-resource-envelope.md)
- Related: [map-branch-truce-desktop-implementation-baseline → map-installed-desktop-application](../../architecture/desktop-baseline/map-installed-desktop-application.md)
- Related: [practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins](../practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins.md)
- Related: [map-installed-artifact-validation-in-base-containers → map-installed-deb-validation-execution](../installed-validation/map-installed-deb-validation-execution.md)
- Related: [map-installed-artifact-validation-in-base-containers → map-installed-validation-evidence-and-desktop-limits](../installed-validation/map-installed-validation-evidence-and-desktop-limits.md)
- Related: [map-installed-artifact-validation-in-base-containers → map-installed-validation-tooling-regression-suite](../installed-validation/map-installed-validation-tooling-regression-suite.md)
<!-- kk:related:end -->

<!-- kk:citations:start -->
# Citations

[1] dcc586bc-c490-4186-893e-e9ac032ec77c:map:4
<!-- kk:citations:end -->
