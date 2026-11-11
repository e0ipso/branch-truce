---
type: map
title: Runtime QML and plugin package dependencies
description: >-
  Imported QML modules, styles and plugins need explicit package dependencies
  and inspection coverage.
tags:
  - packaging
  - platform
  - debian
  - containers
  - compatibility
kk_schema_version: 3
kk_id: map-runtime-qml-and-plugin-package-dependencies
kk_derived_from: []
kk_relates_to:
  - map-branch-truce-desktop-implementation-baseline
  - practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins
  - map-installed-artifact-validation-in-base-containers
kk_depends_on: []
kk_confidence: high
---
`dpkg-shlibdeps` cannot see QML imports or plugins loaded at run time, so `packaging/debian/control` lists the imported QML modules, `git`, the desktop styling, `qt6-wayland` and `qt6-svg-plugins` by hand. `plasma-integration` is the only `Recommends`. A new `import` in `src/ui` needs a matching `Depends` entry, or the inspection's `qmldir` check fails. Breeze icons are SVG, so without `qt6-svg-plugins` every `QIcon::fromTheme` lookup in the installed app returns a null icon. The icon step builds [tests/fixtures/AppearanceProbe.cpp](../../../../../tests/fixtures/AppearanceProbe.cpp) in the base's `-build` image, preloads it into the installed binary under `env -i` with no desktop named, and fails when `document-save` is null. Both base Dockerfiles `COPY` `debian/control` into the `-build` stage, so editing `control` invalidates that stage's cache and the next run rebuilds the image.

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
