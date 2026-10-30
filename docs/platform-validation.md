# Platform validation

Use clean native target containers for package checks and a physical desktop
for interactive behavior. Keep run logs, screenshots, package digests and
machine-specific results with CI artifacts or the issue being investigated.

## Native package checks

Run the release pipeline described in [build.md](build.md#native-ci-and-release-packages)
for Kubuntu, Fedora and Arch. Verify native metadata, dependency resolution,
installed version, smoke loading, integrity, upgrade and removal. Debian also
checks purge. Installed-runtime targets receive package files and the minimal
validation fixture, without application sources, build trees or compiler caches.

## Developer Debian validation

Build and inspect a developer artifact using [target-platforms.md](target-platforms.md#build-and-inspect-developer-artifacts).
For its installed-program checks, run:

```sh
tools/validate-installed-artifact.sh --target kubuntu \
    --package /absolute/path/to/package.deb --output /absolute/path/to/results
```

Use `--target tuxedo` for a Tuxedo developer artifact. Add `--wayland` from a
wlroots session such as sway to exercise the GUI through a bind-mounted
Wayland socket. The script uses isolated HOME, XDG and D-Bus state, opens the
window at scale factors 1, 1.5 and 2 in Breeze Light and Dark, probes AT-SPI,
and checks editor input and mergetool handoff. Run validators sequentially
because they identify the same application window on the compositor.

[validate-installed-artifact.sh](../tools/validate-installed-artifact.sh)
describes its required tools and output files.
[atspi-probe.py](../tools/atspi-probe.py) reads the accessible tree.

## Physical desktop checks

Install the target's native package and verify:

- Application-menu launch, window identity, icons and installed resources.
- Real Git merge and rebase handoff, including resolution and deliberate leave.
- Plasma Wayland window layout at 100%, 150% and 200% output scaling.
- Breeze Light and Dark, including changes while the application runs.
- Selection, clipboard, scrolling, keyboard navigation and composed input
  through IBus or Fcitx 5.
- Screen-reader access to sources, editable results, decisions and progress.

Record OS, Plasma, KWin and Qt/KDE versions alongside each external report.
A headless compositor and environment variables that imitate a KDE session
cannot establish these physical-desktop behaviors. Keep untested behavior
explicit in compatibility claims.
