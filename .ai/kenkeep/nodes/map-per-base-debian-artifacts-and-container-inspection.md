---
type: map
title: Per-base Debian artifacts and container inspection
description: >-
  Each OS base builds its own .deb from shared packaging/debian in a pinned
  container and is inspected on a clean copy of that base.
tags:
  - packaging
  - platform
  - debian
  - containers
  - compatibility
kk_schema_version: 3
kk_id: map-per-base-debian-artifacts-and-container-inspection
kk_derived_from: []
kk_relates_to:
  - map-branch-truce-desktop-implementation-baseline
  - practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins
  - map-installed-artifact-validation-in-base-containers
kk_depends_on: []
kk_confidence: high
---
Branch Truce ships one native debhelper `.deb` per OS base, and [docs/target-platforms.md](../../../docs/target-platforms.md) is the reference for the bases, toolchains, dependency floors and the latest results. Both bases share [packaging/debian/](../../../packaging/debian/). Kubuntu 26.04 builds from `ubuntu:26.04` and Tuxedo OS 24.04 from `ubuntu:24.04`, each pinned by digest in [packaging/kubuntu/Dockerfile](../../../packaging/kubuntu/Dockerfile) and [packaging/tuxedo/Dockerfile](../../../packaging/tuxedo/Dockerfile). The Tuxedo image installs every package from the Tuxedo OS sources and archive key in `packaging/tuxedo/`, except `ca-certificates`, which comes from stock noble first because the Tuxedo repositories use HTTPS. Stock noble ships Qt 6.4, which cannot build the desktop target. Packages from `tools/build-artifact.sh` carry a `+kubuntu26.04` or `+tuxedo24.04` version suffix, so the two artifacts differ in version as well as file name. Do not assume either package installs on the other base.

[tools/build-artifact.sh](../../../tools/build-artifact.sh) builds a `-target` image (the clean base) and a `-build` image (toolchain plus `Build-Depends`) per base, then runs `dpkg-buildpackage -b` on a copy of the sources with `--network none`. The image builds and the inspection fetch from the archives; the package build itself has no network. A fixed build path and `SOURCE_DATE_EPOCH` from the last commit that touched the copied paths make a rebuild from cached images byte-identical. [tools/inspect-artifact.sh](../../../tools/inspect-artifact.sh) installs the `.deb` in a throwaway container from the `-target` image. It checks the metadata and file list, that every linked library resolves through a package named in `Depends`, that each QML import has a `qmldir` from a `Depends` package, the desktop and AppStream validators, `--version` and `--smoke-test`, that the Breeze icon `document-save` resolves in the installed app, and that `apt-get remove` leaves no program files. [tools/artifact-baseline.sh](../../../tools/artifact-baseline.sh) runs inside both containers and records the OS, compiler and Qt/KDE package versions with the evidence. `CONTAINER_ENGINE` picks the engine and defaults to `docker`.

`dpkg-shlibdeps` cannot see QML imports or plugins loaded at run time, so `packaging/debian/control` lists the imported QML modules, `git`, the desktop styling, `qt6-wayland` and `qt6-svg-plugins` by hand. There is no `Recommends`. A new `import` in `src/ui` needs a matching `Depends` entry, or the inspection's `qmldir` check fails. Breeze icons are SVG, so without `qt6-svg-plugins` every `QIcon::fromTheme` lookup in the installed app returns a null icon. The icon step builds [tests/fixtures/AppearanceProbe.cpp](../../../tests/fixtures/AppearanceProbe.cpp) in the base's `-build` image, preloads it into the installed binary under `env -i` with no desktop named, and fails when `document-save` is null. Both base Dockerfiles `COPY` `debian/control` into the `-build` stage, so editing `control` invalidates that stage's cache and the next run rebuilds the image.

There are two package paths on purpose. `tools/build-artifact.sh` builds the packages that the container inspection and the installed validation use. CI builds the release packages with [packaging/scripts/container-check.sh](../../../packaging/scripts/container-check.sh), which runs [package-release.sh](../../../packaging/scripts/package-release.sh) and then [check-package.sh](../../../packaging/scripts/check-package.sh) in a target container of each base. `package-release.sh` uses the same `packaging/debian/`, but both bases get the same version with no base suffix. Only the file name differs: `_amd64-ubuntu26.04.deb` or `_amd64-tuxedo24.04.deb`. A result from one path is not evidence for the other, and the [README](../../../README.md) keeps the two apart. `check-package.sh` requires each name in its `expected_deps` to appear in `Depends` and ignores extra entries. So a dependency added to `control` goes unchecked until the list names it, and a name the package no longer depends on fails the check. The binary does not link `libkirigami6` or `libqt6quickcontrols2-6`, so `dpkg-shlibdeps` leaves them out and the list must not name them. `libkirigami6` still arrives through `qml6-module-org-kde-kirigami`, which is why the target version report still queries it.

The ctest `packaging.artifact-tooling` runs [tests/packaging/ArtifactTooling.sh](../../../tests/packaging/ArtifactTooling.sh) against a stub `CONTAINER_ENGINE`, so it needs no Docker, network or root. It covers the host side, and it sources `inspect-artifact.sh` to test pure helpers. The script returns before its entry point when sourced. `symbol_tags` and `check_private_abi` sit outside `container_main`, so the test runs every private Qt ABI branch on synthetic `objdump` output, including a binary with no version tags. The rest of the `--in-container` half runs only inside the target container, and the test feeds a canned report for it. Container results are not desktop compatibility. Running the installed package in a container of each base is a separate step, described in the [installed-artifact validation node](map-installed-artifact-validation-in-base-containers.md). Installation and runtime on real Kubuntu and Tuxedo OS desktops are still untested.

<!-- kk:related:start -->
# Related

- Related: [map-branch-truce-desktop-implementation-baseline](map-branch-truce-desktop-implementation-baseline.md)
- Related: [practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins](practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins.md)
- Related: [map-installed-artifact-validation-in-base-containers](map-installed-artifact-validation-in-base-containers.md)
<!-- kk:related:end -->
