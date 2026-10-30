---
type: map
title: Installed-artifact validation in base containers
description: >-
  validate-installed-artifact.sh runs each installed .deb in a container of its
  base on headless sway; real-desktop rows stay UNAVAILABLE.
tags:
  - packaging
  - platform
  - validation
  - wayland
  - accessibility
  - compatibility
kk_schema_version: 3
kk_id: map-installed-artifact-validation-in-base-containers
kk_derived_from: []
kk_relates_to:
  - map-per-base-debian-artifacts-and-container-inspection
  - map-branch-truce-desktop-implementation-baseline
  - map-journey-acceptance-suite
kk_depends_on: []
kk_confidence: high
---
This is the separate developer-artifact validation path and its dated evidence, not the current three-target release pipeline. Release packages are Kubuntu DEB, Fedora 44 RPM and Arch pkg.tar.zst, with no Tuxedo compilation. The user reports that the Kubuntu release DEB works on Tuxedo OS; the historical per-base runs below use different artifacts. See [target platforms](../../../docs/target-platforms.md) for that distinction.

[tools/validate-installed-artifact.sh](../../../tools/validate-installed-artifact.sh) installs a built `.deb` with `apt install --no-install-recommends` in a fresh container from the `branch-truce-artifact:<base>-target` image that `tools/build-artifact.sh` leaves behind. It then runs the installed program with no source or build tree on the system. Nothing is installed on the host. Without `--wayland` it runs only the offscreen steps: install, layout, the CLI, `gio launch` of the desktop entry, and a real `git mergetool` whose GUI is killed. With `--wayland` the container draws on the host's headless sway through a bind-mounted Wayland socket. swaymsg reports the window's `app_id` and size, grim takes screenshots from the host, [tools/atspi-probe.py](../../../tools/atspi-probe.py) reads the accessible tree inside the container, and `wtype` types into the result pane. The window opens six times, at `QT_SCALE_FACTOR` 1, 1.5 and 2 in BreezeLight and BreezeDark, and each screenshot is named `<package>-scale<factor>-<scheme>.png`. With the editor-input and resolved-mergetool screenshots, a run writes eight. Run the two bases one after the other, because both runs look for the same window on the compositor.

[docs/platform-validation.md](../../../docs/platform-validation.md) records the results. Word each one as observed in a container of that base. Variables such as `XDG_CURRENT_DESKTOP=KDE` make Qt behave as in a Plasma session, but the container has no KWin, Plasma shell, portals, input method, screen reader or physical display. Rows that need them stay UNAVAILABLE or Not run: both real desktops, Plasma menu launch, KWin output scaling, IME, Orca and real hardware. Nothing in that file is a compatibility claim. For those historical measurements, the only Tuxedo OS host was a headless server and no Kubuntu 26.04 machine was available. The later user-reported installation does not change those dated rows.

The package depends on `qt6-wayland`, which ships the `xdg-shell` integration. Without it the app aborts on Wayland with exit 134 and "No shell integration named "xdg-shell" found". Plasma pulls `qt6-wayland` in through `plasma-workspace`, but a minimal or non-Plasma Wayland session does not, so it has to be in `Depends`. The `wayland-minimal` step runs `--smoke-test` on Wayland with only the installed closure and records a failure as FAIL, not a warning. The `session` step then installs only `plasma-integration`, as a Plasma desktop has, before the window steps.

The minimum window size in [Main.qml](../../../src/ui/Main.qml) is 900x500 logical pixels, so the window fits 1920x1080 at 200% (960x540). When the window is shorter than its content the page scrolls, each text pane keeps at least eight lines, and the row of file actions wraps. The container runs measured 150% and 200% with `QT_SCALE_FACTOR` 1.5 and 2. 125% and KWin's own output scaling were not measured. The window-fit journey in the [acceptance suite](map-journey-acceptance-suite.md) checks 1280x720 and 960x540 offscreen.

The ctest `packaging.installed-validation-tooling` runs [tests/packaging/InstalledValidationTooling.sh](../../../tests/packaging/InstalledValidationTooling.sh) against stub container engine, swaymsg and grim commands and a fake Atspi module from [AtspiProbeFixture.py](../../../tests/packaging/AtspiProbeFixture.py). It needs no Docker, display or D-Bus, and it exits 77, reported as skipped, when `jq` or `python3` is missing. It does not cover the container steps themselves. The host side keeps the exit status of its `run_steps | tee` pipeline instead of letting `set -e` and `pipefail` abort it. After a failed install the report still ends with its `== Result` section and failure count, and the script prints the report path. The ctest asserts both.

<!-- kk:related:start -->
# Related

- Related: [map-per-base-debian-artifacts-and-container-inspection](map-per-base-debian-artifacts-and-container-inspection.md)
- Related: [map-branch-truce-desktop-implementation-baseline](map-branch-truce-desktop-implementation-baseline.md)
- Related: [map-journey-acceptance-suite](map-journey-acceptance-suite.md)
<!-- kk:related:end -->
