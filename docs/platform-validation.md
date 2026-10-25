# Platform validation

This file records what has been observed with the installed packages, and
what has not been tested. No compatibility claim for Kubuntu 26.04 or
Tuxedo OS 24.04 desktops follows from it. Neither artifact has been installed
on a real target desktop. Everything below was observed in a container of each
artifact's own OS base, shown on a headless Wayland compositor. It is evidence
about the package and the program, not about Plasma, KWin or real hardware.

## Status

| Environment | Status |
| --- | --- |
| Kubuntu 26.04 LTS desktop, Plasma session | UNAVAILABLE. No Kubuntu 26.04 machine was available. |
| Tuxedo OS 24.04 desktop, Plasma session | UNAVAILABLE. The only Tuxedo OS host is a headless server with no Plasma session, no display manager and no physical display. |
| Container of the Kubuntu 26.04 base, on the host's headless sway | Observed on 2026-10-25. 36 checks passed, 0 failed, 1 warning. |
| Container of the Tuxedo OS 24.04 base, on the host's headless sway | Observed on 2026-10-25. 36 checks passed, 0 failed, 1 warning. |

The rows that matter for compatibility are the first two, and both stay
open. The container rows show that each package installs on its own base,
that the installed program runs without the source or build tree, and what
it does on a Wayland compositor with Breeze, two scale factors and AT-SPI.

## What was used

Host, shared by both containers:

- Tuxedo OS 24.04 (noble), kernel `7.0.0-111030-tuxedo`, rootless Docker
  29.8.2.
- sway 1.9 (`1.9-1build2`) with the wlroots headless backend: one output,
  `HEADLESS-1`, 1920x1080, output scale 1.0, no input devices and no GPU.
  Mesa fell back to software rendering. grim 1.4.0 took the screenshots
  from the host side.
- The containers connected to the host's Wayland socket
  (`/run/user/1000/wayland-1`) through a bind mount. They used their own
  D-Bus session bus, home and XDG directories, and never touched the host's.

Artifacts, built with `tools/build-artifact.sh` from commit
`46e080149c13fdf806fe390455be343e73882eec` with no changed source paths:

| | Kubuntu 26.04 | Tuxedo OS 24.04 |
| --- | --- | --- |
| File | `branch-truce_0.1.0+kubuntu26.04_amd64.deb` | `branch-truce_0.1.0+tuxedo24.04_amd64.deb` |
| SHA-256 | `4bf41ce4ce83022ef97dd8d91b2275d1c9f76dbedd46ea7bed7216a09ab8889a` | `911931531f6cccf73c2b623c2eaf6670606fac44d40fc8e9b01f9232979e4f61` |
| Size | 503196 bytes | 487276 bytes |
| Container image | `branch-truce-artifact:kubuntu-26.04-target` | `branch-truce-artifact:tuxedo-24.04-target` |
| Container OS | Ubuntu 26.04.1 LTS, resolute | Ubuntu 24.04.5 LTS, noble, with only the Tuxedo OS package sources |

The container `/etc/os-release` of the Tuxedo base still names Ubuntu,
because Tuxedo's branding packages are not installed; see
[target-platforms.md](target-platforms.md#os-bases).

Runtime packages after `apt-get install --no-install-recommends` of the
`.deb`:

| Package | Kubuntu 26.04 | Tuxedo OS 24.04 |
| --- | --- | --- |
| Packages added by the install | 189 | 180 |
| libc6 | 2.43-2ubuntu2.4 | 2.39-0ubuntu8.9 |
| libqt6core6t64, libqt6gui6 | 6.10.2+dfsg-7 | 6.10.2+dfsg-7~tux1 |
| libqt6qml6, libqt6quick6, qml6-module-qtquick* | 6.10.2+dfsg-3 | 6.10.2+dfsg-3~tux1 |
| qml6-module-org-kde-kirigami, qml6-module-org-kde-desktop | 6.24.0-0ubuntu1 | 6.24.0-0ubuntu1~tux1 |
| kf6-breeze-icon-theme | 6.24.0-0ubuntu1 | 6.24.0-0ubuntu1~tux1 |
| kde-style-breeze | 4:6.6.5-0ubuntu0.1 | 4:6.6.5-0ubuntu0.1~tux1 |
| git | 1:2.53.0-1ubuntu1.1 | 1:2.43.0-1ubuntu7.4 |

Added afterwards, because a Plasma desktop has them: `plasma-workspace`
depends on `qt6-wayland` and `plasma-integration` in both archives. This was
read from the archive metadata, not from an installed desktop.

| Session package | Kubuntu 26.04 | Tuxedo OS 24.04 |
| --- | --- | --- |
| qt6-wayland | 6.10.2-4 | 6.10.2-4~tux1 |
| plasma-integration | 6.6.5-0ubuntu0.1 | 6.6.5-0ubuntu0.1~tux1 |

Validation tools, not part of the runtime closure: `libglib2.0-bin` (for
`gio launch`), `dbus-daemon`, `at-spi2-core` (2.60.4 on Kubuntu, 2.52.0 on
Tuxedo), `gir1.2-atspi-2.0`, `python3-gi` and `wtype` 0.4.

The GUI ran with `QT_QPA_PLATFORM=wayland`, `XDG_SESSION_TYPE=wayland`,
`XDG_CURRENT_DESKTOP=KDE`, `KDE_SESSION_VERSION=6`, `KDE_FULL_SESSION=true`
and `QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1`. The colour scheme came from a
`kdeglobals` made from `/usr/share/color-schemes/BreezeLight.colors` or
`BreezeDark.colors`. These variables imitate a Plasma session for Qt; they
do not provide KWin, the Plasma shell, portals or a real seat.

## How to reproduce

Build both artifacts as described in
[target-platforms.md](target-platforms.md#build-and-inspect), then, from a
shell inside a wlroots session such as sway:

```sh
tools/validate-installed-artifact.sh --target kubuntu --wayland \
    --package "$out/branch-truce_0.1.0+kubuntu26.04_amd64.deb" --output "$out/kubuntu"
tools/validate-installed-artifact.sh --target tuxedo --wayland \
    --package "$out/branch-truce_0.1.0+tuxedo24.04_amd64.deb" --output "$out/tuxedo"
```

Run them one after the other: both look for the same window on the
compositor. Without `--wayland` the script runs only the offscreen steps.
Each run writes `<package>.validation` (the report), seven screenshots and
`<package>.atspi` (the accessible tree) to the output directory, and removes
its container on exit. The script header in
[tools/validate-installed-artifact.sh](../tools/validate-installed-artifact.sh)
lists the steps. [tools/atspi-probe.py](../tools/atspi-probe.py) is the AT-SPI
client it runs inside the container.

Each run took three to four minutes on this host. While it runs, the
app's windows appear on the compositor and take keyboard focus.

## Results

"Pass" means the check ran in the container and its expected outcome was
observed. The real-desktop column has no observations.

| Check | Kubuntu container | Tuxedo container | Real desktops |
| --- | --- | --- | --- |
| Clean install with `--no-install-recommends` | Pass | Pass | UNAVAILABLE |
| No CMake file, application QML source or build path on the system | Pass | Pass | UNAVAILABLE |
| `--version`, `--help`, `--smoke-test` from `/usr/bin` | Pass | Pass | UNAVAILABLE |
| Resource lookup: application QML from the binary, every loaded `qmldir` and plugin owned by a package | Pass, 23 `qmldir` and 30 plugins | Pass, 23 `qmldir` and 30 plugins | UNAVAILABLE |
| `gio launch` of the installed desktop entry, offscreen | Pass | Pass | UNAVAILABLE |
| Plasma menu or KRunner launch | Not run | Not run | UNAVAILABLE |
| Real `git mergetool`, launcher starts the GUI, GUI killed: `MERGED` restored, path unmerged | Pass, Git 2.53.0 | Pass, Git 2.43.0 | UNAVAILABLE |
| Real `git mergetool` on Wayland: typed result, Mark resolved, Confirm resolution, Git stages the reviewed bytes, session reports 1 resolved | Pass | Pass | UNAVAILABLE |
| Wayland with the package's closure only | Warning: aborts | Warning: aborts | UNAVAILABLE |
| Wayland with `qt6-wayland` and `plasma-integration` | Pass, `xdg_shell` | Pass, `xdg_shell` | UNAVAILABLE |
| Window `app_id` matches the desktop entry | Pass, `local.branch_truce.BranchTruce` | Pass, `local.branch_truce.BranchTruce` | UNAVAILABLE |
| `QT_SCALE_FACTOR=1`, Breeze Light and Breeze Dark | Pass | Pass | UNAVAILABLE |
| `QT_SCALE_FACTOR=2`, Breeze Light and Breeze Dark | Renders; clipped on a 1920x1080 output | Renders; clipped on a 1920x1080 output | UNAVAILABLE |
| Plasma output scaling (KWin fractional or integer scale) | Not run | Not run | UNAVAILABLE |
| Editor input through the Wayland virtual keyboard, accented letters | Pass | Pass | UNAVAILABLE |
| Composed input through an input method (IBus, Fcitx 5, dead keys) | Not run | Not run | UNAVAILABLE |
| Accessibility metadata over AT-SPI | Pass | Pass | UNAVAILABLE |
| Orca or another screen reader | Not run | Not run | UNAVAILABLE |
| Physical display, GPU rendering, pointer and touch input | Not run | Not run | UNAVAILABLE |

### Install, layout and resources

The `.deb` installed on its own base with no extra package sources. After
the install, `find / -xdev` found no `CMakeCache.txt`, `CMakeLists.txt`,
`Main.qml`, `ComparisonView.qml`, `/work` or `branch-truce-*` build
directory. With `QML_IMPORT_TRACE=1` and `QT_DEBUG_PLUGINS=1`, the smoke
test loaded the `BranchTruce` module from the binary's `qrc:/qt/qml/`
resources. It loaded 23 `qmldir` files and 30 plugins from the file system,
all under `/usr/lib/x86_64-linux-gnu/qt6`, and `dpkg-query -S` found an
installed package for each. The offscreen runs have no platform theme, so
they load the Fusion Qt Quick Controls style.

The desktop entry's `Exec=branch-truce %f`, started by `gio launch` with the
repository as its argument, ran `/usr/bin/branch-truce /tmp/bt-repo`, which
opened the session and created its mergetool socket. A Plasma menu, KRunner
or a file manager launch was not available.

### Mergetool handoff with real Git

The Git configuration is the one in
[mergetool-launcher.md](mergetool-launcher.md#tested-with-a-real-parent),
with the launcher at `/usr/bin/branch-truce`. On both bases,
`git mergetool --tool truce` ran the installed launcher, which started the
installed GUI with `--mergetool-path` and attached `tracked.txt`.

- When the GUI was killed with SIGTERM, `git mergetool` exited 1, `MERGED`
  had its launch bytes again and the index still had three stages.
- On Wayland, the result was replaced with `merged é ü` and a newline,
  then Mark resolved and Confirm resolution were pressed through AT-SPI.
  `git mergetool` exited 0. The index held blob
  `23a7d88d743520a735939b41fd8f0ea5ee330b56` at stage 0, which is
  `git hash-object` of the typed text, and the session showed
  "1 files in this session · 1 resolved".

### Wayland, scaling and theme

With only the package's closure, `branch-truce --smoke-test` on Wayland
aborted with exit status 134. Qt found the `wayland` platform plugin, which
`libqt6waylandclient6` ships, but no shell integration:

```text
qt.qpa.wayland: No shell integration named "xdg-shell" found
qt.qpa.wayland: Loading shell integration failed.
```

The shell integrations are in `qt6-wayland`, which the package does not
depend on. A Plasma install brings it through `plasma-workspace`, so this
should not affect the target desktops, but a minimal or non-Plasma Wayland
session cannot start the app. With `qt6-wayland` and `plasma-integration`
installed, every desktop-entry launch opened an `xdg_shell` window whose
`app_id` matched the desktop entry. sway tiled it to the full 1920x1080
output.

Breeze Light and Breeze Dark both applied. On both bases the screenshots show
Breeze-styled controls in the light and the dark palette; this was checked by
looking at the screenshots, not by tracing which style plugin loaded. The colour
scheme was set before launch; switching it while the app runs was not
tested.

At `QT_SCALE_FACTOR=2` the text and controls doubled, but the 1920x1080
output is only 960x540 logical pixels. That is smaller than the window's
minimum size of 1000x760 (`src/ui/Main.qml`). The window stayed at the
output size, the toolbar wrapped onto two rows, the right edge and the
bottom row were cut off, and each text pane showed about one line. By the
same arithmetic, any scale above about 1.42 on a 1080-pixel-high screen
leaves less than 760 logical pixels of height. That part was calculated,
not observed. Plasma's own output scaling, which KWin applies rather than
`QT_SCALE_FACTOR`, was not available.

### Editor input

The result editor received focus through AT-SPI `grabFocus`. `wtype` then
sent Ctrl+A, `merged é ü` and Return through the Wayland virtual keyboard
protocol, and AT-SPI read back `merged é ü` followed by a paragraph
separator. Undo became available, so the app took the keys as edits. `wtype`
uploads a new keymap on every run, and without a delay the app missed the
first characters; the script waits 400 ms after the upload. That is a
property of the test tool, not of the app.

This is keyboard input with accented keysyms, not composed input. No input
method framework ran and sway had no input method, so preedit text, dead
keys and the app's `inputMethodComposing` guard on Save and Mark resolved
were not exercised.

### Accessibility metadata

Over AT-SPI, on the container's own bus, the app exposed 113 objects on
both bases. The named controls the script checks were present: the Mark
resolved and Save buttons, the Editable result and Passage search text
fields, the Search scope combo box, the Wrap lines check box and the
`tracked.txt` list item. Every button had a name. Every text field that was
showing had a name, and the source panes reported the read-only state.
Buttons offered the Press and SetFocus actions, and Press worked. Other
observations:

- The editable result implements the AT-SPI Text interface, but not
  EditableText. Line breaks come back as U+2029 paragraph separators.
- Read-only text panes report both the editable and the read-only state.
- The role name of a button is `push-button` with AT-SPI 2.52 (Tuxedo) and
  `button` with AT-SPI 2.60 (Kubuntu).

No screen reader ran, so it is unknown what Orca announces, in what order,
or whether focus changes are spoken.

### Other observations

After the resolved handoff, the selected file showed "Written and resolved."
and, under it, "Waiting for Git to launch this path. It is not resolved."
The second line contradicts the first. Its visibility in `src/ui/Main.qml`
does not depend on the selected file's state.

## What would close the real-desktop rows

On each real desktop: install the artifact with `apt` from the file, record
`/etc/os-release`, `plasmashell --version`, `kwin_wayland --version` and the
installed Qt/KDE package versions, then repeat the matrix above in a Plasma
Wayland session. That means launching from the application menu, using
KWin output scales of 100%, 150% and 200%, switching between Breeze Light
and Dark while the app runs, typing composed text through IBus or Fcitx 5,
and having Orca read the conflict view. Record any failure as found.
