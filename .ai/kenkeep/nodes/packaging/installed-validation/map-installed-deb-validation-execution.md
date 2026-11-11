---
type: map
title: Installed DEB validation execution
description: >-
  Selected target images run the installed DEB offscreen or on headless Wayland
  without installing anything on the host.
tags:
  - packaging
  - platform
  - validation
  - wayland
  - accessibility
  - compatibility
kk_schema_version: 3
kk_id: map-installed-deb-validation-execution
kk_derived_from: []
kk_relates_to:
  - map-per-base-debian-artifacts-and-container-inspection
  - map-branch-truce-desktop-implementation-baseline
  - map-journey-acceptance-suite
kk_depends_on: []
kk_confidence: high
---
This is the installed DEB validation path and its dated evidence, separate from the three-target release package pipeline. Release packages are Kubuntu DEB, Fedora 44 RPM and Arch pkg.tar.zst, with no Tuxedo compilation. The user reports that the Kubuntu release DEB works on Tuxedo OS; the target table controls which clean base installs the selected release package. See [target platforms](../../../../../docs/target-platforms.md) for that distinction.

[tools/validate-installed-artifact.sh](../../../../../tools/validate-installed-artifact.sh) installs a built `.deb` with `apt install --no-install-recommends` in a fresh target image selected by [packaging/validation-targets.tsv](../../../../../packaging/validation-targets.tsv) and built through [tools/lib/targets.sh](../../../../../tools/lib/targets.sh). Its [installed-checks.sh](../../../../../tools/installed-checks.sh) steps run the installed program with no source or build tree on the system. Nothing is installed on the host. Without `--wayland` it runs only the offscreen steps: install, layout, the CLI, `gio launch` of the desktop entry, and a real `git mergetool` whose GUI is killed. With `--wayland` the container draws on the host's headless sway through a bind-mounted Wayland socket. swaymsg reports the window's `app_id` and size, grim takes screenshots from the host, [tools/atspi-probe.py](../../../../../tools/atspi-probe.py) reads the accessible tree inside the container, and `wtype` types into the result pane. The window opens six times, at `QT_SCALE_FACTOR` 1, 1.5 and 2 in BreezeLight and BreezeDark, and each screenshot is named `<package>-scale<factor>-<scheme>.png`. With the editor-input and resolved-mergetool screenshots, a run writes eight. Run the two bases one after the other, because both runs look for the same window on the compositor.

The package depends on `qt6-wayland`, which ships the `xdg-shell` integration. Without it the app aborts on Wayland with exit 134 and "No shell integration named "xdg-shell" found". Plasma pulls `qt6-wayland` in through `plasma-workspace`, but a minimal or non-Plasma Wayland session does not, so it has to be in `Depends`. The `wayland-minimal` step runs `--smoke-test` on Wayland with only the installed closure and records a failure as FAIL, not a warning. The `session` step then installs only `plasma-integration`, as a Plasma desktop has, before the window steps.

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
