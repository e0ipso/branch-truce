---
type: map
title: Per-base Debian artifacts and container inspection
description: >-
  Developer per-base DEBs are separate from Kubuntu DEB, Fedora RPM and Arch
  release packages, each validated in a clean native target.
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
The separate developer artifact tooling builds one native debhelper `.deb` per OS base, and [docs/target-platforms.md](../../../docs/target-platforms.md) is the reference for the bases, toolchains, dependency floors and the latest results. Both bases share [packaging/debian/](../../../packaging/debian/). Kubuntu 26.04 builds from `ubuntu:26.04` and Tuxedo OS 24.04 from `ubuntu:24.04`, each pinned by digest in [packaging/kubuntu/Dockerfile](../../../packaging/kubuntu/Dockerfile) and [packaging/tuxedo/Dockerfile](../../../packaging/tuxedo/Dockerfile). The Tuxedo image installs every package from the Tuxedo OS sources and archive key in `packaging/tuxedo/`, except `ca-certificates`, which comes from stock noble first because the Tuxedo repositories use HTTPS. Stock noble ships Qt 6.4, which cannot build the desktop target. Packages from `tools/build-artifact.sh` carry a `+kubuntu26.04` or `+tuxedo24.04` version suffix, so the two artifacts differ in version as well as file name. Do not assume either package installs on the other base.

[tools/build-artifact.sh](../../../tools/build-artifact.sh) builds a `-target` image (the clean base) and a `-build` image (toolchain plus `Build-Depends`) per base, then runs `dpkg-buildpackage -b` on a copy of the sources with `--network none`. The image builds and the inspection fetch from the archives; the package build itself has no network. A fixed build path and `SOURCE_DATE_EPOCH` from the last commit that touched the copied paths make a rebuild from cached images byte-identical. [tools/inspect-artifact.sh](../../../tools/inspect-artifact.sh) installs the `.deb` in a throwaway container from the `-target` image. It checks the metadata and file list, that every linked library resolves through a package named in `Depends`, that each QML import has a `qmldir` from a `Depends` package, the desktop and AppStream validators, `--version` and `--smoke-test`, that the Breeze icon `document-save` resolves in the installed app, and that `apt-get remove` leaves no program files. [tools/artifact-baseline.sh](../../../tools/artifact-baseline.sh) runs inside both containers and records the OS, compiler and Qt/KDE package versions with the evidence. `CONTAINER_ENGINE` picks the engine and defaults to `docker`.

`dpkg-shlibdeps` cannot see QML imports or plugins loaded at run time, so `packaging/debian/control` lists the imported QML modules, `git`, the desktop styling, `qt6-wayland` and `qt6-svg-plugins` by hand. `plasma-integration` is the only `Recommends`. A new `import` in `src/ui` needs a matching `Depends` entry, or the inspection's `qmldir` check fails. Breeze icons are SVG, so without `qt6-svg-plugins` every `QIcon::fromTheme` lookup in the installed app returns a null icon. The icon step builds [tests/fixtures/AppearanceProbe.cpp](../../../tests/fixtures/AppearanceProbe.cpp) in the base's `-build` image, preloads it into the installed binary under `env -i` with no desktop named, and fails when `document-save` is null. Both base Dockerfiles `COPY` `debian/control` into the `-build` stage, so editing `control` invalidates that stage's cache and the next run rebuilds the image.

There are two package paths on purpose. `tools/build-artifact.sh` produces the per-base developer artifacts and dated installed-validation evidence. Release CI uses [container-check.sh](../../../packaging/scripts/container-check.sh) once per `kubuntu-deb`, `fedora-rpm` and `arch-pkg` target. It selects the candidate version before compilation, validates the native DEB/RPM/pkg.tar.zst in a clean target and publishes exactly those validated bytes through revision/run/version/digest manifests. There is no active Tuxedo release build. [package-info.sh](../../../packaging/scripts/package-info.sh) owns native filenames, versions and architectures. A verified published predecessor avoids synthetic compilation; unavailable predecessors use a real synthetic upgrade, while corrupt downloads fail. The compiler cache is optional, bounded to 2 GB per target/architecture/image-input subtree and mounted only during compilation. Configurations and actual toolchain images have separate namespaces. CTest runs with parallel level two and four exclusive gates, including the only lint invocation. See [docs/build.md](../../../docs/build.md#native-ci-and-release-packages) for commands, cache statistics and artifact flow. A result from the developer path is not evidence for release bytes. `check-package.sh` requires each name in its `expected_deps` to appear in `Depends` and ignores extra entries. So a dependency added to `control` goes unchecked until the list names it, and a name the package no longer depends on fails the check. Keep the explicit runtime QML/style/plugin dependencies and linked-library requirements aligned with `packaging/debian/control` and the native checker. `libkirigami6` arrives through `qml6-module-org-kde-kirigami`, which is why the target version report queries it.

The ctest `packaging.artifact-tooling` runs [tests/packaging/ArtifactTooling.sh](../../../tests/packaging/ArtifactTooling.sh) against a stub `CONTAINER_ENGINE`, so it needs no Docker, network or root. It covers the host side, and it sources `inspect-artifact.sh` to test pure helpers. The script returns before its entry point when sourced. `symbol_tags` and `check_private_abi` sit outside `container_main`, so the test runs every private Qt ABI branch on synthetic `objdump` output, including a binary with no version tags. The rest of the `--in-container` half runs only inside the target container, and the test feeds a canned report for it. Container results are not desktop compatibility. Running the installed package in a container of each base is a separate step, described in the [installed-artifact validation node](map-installed-artifact-validation-in-base-containers.md). The user reports that the Kubuntu release DEB installs and works on Tuxedo OS. That report and clean native container checks do not establish compatibility across real desktop versions. The earlier per-base measurements retain their dates in docs/target-platforms.md.

<!-- kk:related:start -->
# Related

- Related: [map-branch-truce-desktop-implementation-baseline](map-branch-truce-desktop-implementation-baseline.md)
- Related: [practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins](practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins.md)
- Related: [map-installed-artifact-validation-in-base-containers](map-installed-artifact-validation-in-base-containers.md)
<!-- kk:related:end -->
