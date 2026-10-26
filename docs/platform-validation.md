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
| Container of the Kubuntu 26.04 base, on the host's headless sway | Observed on 2026-10-25. 37 checks passed, 0 failed, no warning. |
| Container of the Tuxedo OS 24.04 base, on the host's headless sway | Observed on 2026-10-25. 37 checks passed, 0 failed, no warning. |

The rows that matter for compatibility are the first two, and both stay
open. The container rows show that each package installs on its own base,
that the installed program runs without the source or build tree, and what
it does on a Wayland compositor with Breeze, three scale factors and AT-SPI.

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

Artifacts, built with `tools/build-artifact.sh` from the commit that added
`qt6-wayland` to `Depends`, with no changed source paths:

| | Kubuntu 26.04 | Tuxedo OS 24.04 |
| --- | --- | --- |
| File | `branch-truce_0.1.0+kubuntu26.04_amd64.deb` | `branch-truce_0.1.0+tuxedo24.04_amd64.deb` |
| SHA-256 | `da990d279d98fd88dd8069c373889f95b1ea9092a8881cb8dd58f38e746ac07e` | `d524f8fe37d7c38da4c55b1121cab9a45f99afe34103ea29f881e77f0d0a0bdf` |
| Size | 505068 bytes | 489554 bytes |
| Container image | `branch-truce-artifact:kubuntu-26.04-target` | `branch-truce-artifact:tuxedo-24.04-target` |
| Container OS | Ubuntu 26.04.1 LTS, resolute | Ubuntu 24.04.5 LTS, noble, with only the Tuxedo OS package sources |

The container `/etc/os-release` of the Tuxedo base still names Ubuntu,
because Tuxedo's branding packages are not installed; see
[target-platforms.md](target-platforms.md#os-bases).

Runtime packages after `apt-get install --no-install-recommends` of the
`.deb`:

| Package | Kubuntu 26.04 | Tuxedo OS 24.04 |
| --- | --- | --- |
| Packages added by the install | 193 | 184 |
| libc6 | 2.43-2ubuntu2.4 | 2.39-0ubuntu8.9 |
| libqt6core6t64, libqt6gui6 | 6.10.2+dfsg-7 | 6.10.2+dfsg-7~tux1 |
| libqt6qml6, libqt6quick6, qml6-module-qtquick* | 6.10.2+dfsg-3 | 6.10.2+dfsg-3~tux1 |
| qml6-module-org-kde-kirigami, qml6-module-org-kde-desktop | 6.24.0-0ubuntu1 | 6.24.0-0ubuntu1~tux1 |
| kf6-breeze-icon-theme | 6.24.0-0ubuntu1 | 6.24.0-0ubuntu1~tux1 |
| kde-style-breeze | 4:6.6.5-0ubuntu0.1 | 4:6.6.5-0ubuntu0.1~tux1 |
| git | 1:2.53.0-1ubuntu1.1 | 1:2.43.0-1ubuntu7.4 |
| qt6-wayland | 6.10.2-4 | 6.10.2-4~tux1 |

Added after the Wayland check of the closure, because a Plasma desktop has
it: `plasma-workspace` depends on `plasma-integration` in both archives. This
was read from the archive metadata, not from an installed desktop.

| Session package | Kubuntu 26.04 | Tuxedo OS 24.04 |
| --- | --- | --- |
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
With `--wayland` it opens the window six times, at `QT_SCALE_FACTOR` 1, 1.5
and 2 in Breeze Light and Breeze Dark, and saves each as
`<package>-scale<factor>-<scheme>.png`. Each run writes
`<package>.validation` (the report), eight screenshots (those six, editor
input and the resolved mergetool) and `<package>.atspi` (the accessible
tree) to the output directory, and removes its container on exit. The
script header in
[tools/validate-installed-artifact.sh](../tools/validate-installed-artifact.sh)
lists the steps. [tools/atspi-probe.py](../tools/atspi-probe.py) is the AT-SPI
client it runs inside the container.

Each run took three to four minutes on this host when the script opened
four windows. The two 1.5 windows add to that. While it runs, the app's
windows appear on the compositor and take keyboard focus.

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
| Wayland with the package's closure only, `--smoke-test` | Pass | Pass | UNAVAILABLE |
| Wayland with `plasma-integration` added, desktop-entry launches | Pass, `xdg_shell` | Pass, `xdg_shell` | UNAVAILABLE |
| Window `app_id` matches the desktop entry | Pass, `local.branch_truce.BranchTruce` | Pass, `local.branch_truce.BranchTruce` | UNAVAILABLE |
| `QT_SCALE_FACTOR=1`, Breeze Light and Breeze Dark | Pass | Pass | UNAVAILABLE |
| `QT_SCALE_FACTOR=1.5`, Breeze Light and Breeze Dark | Pass, fits 1920x1080 | Pass, fits 1920x1080 | UNAVAILABLE |
| `QT_SCALE_FACTOR=2`, Breeze Light and Breeze Dark | Pass, fits 1920x1080 and scrolls; was clipped before the minimum size was lowered | Pass, fits 1920x1080 and scrolls; was clipped before the minimum size was lowered | UNAVAILABLE |
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
installed package for each. These runs predate the app's own style
selection, described under "Wayland, scaling and theme", so with no platform
theme they loaded the Fusion Qt Quick Controls style.

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
exited 0 on both bases. The package depends on `qt6-wayland`, which ships the
shell integrations. Before it did, the same check aborted with exit status
134: Qt found the `wayland` platform plugin, which `libqt6waylandclient6`
ships, but no shell integration:

```text
qt.qpa.wayland: No shell integration named "xdg-shell" found
qt.qpa.wayland: Loading shell integration failed.
```

A Plasma install brought `qt6-wayland` through `plasma-workspace`, but a
minimal or non-Plasma Wayland session could not start the app. The check now
fails the run if the closure alone cannot start on Wayland. With
`plasma-integration` added, every desktop-entry launch opened an `xdg_shell`
window whose `app_id` matched the desktop entry. sway tiled it to the full
1920x1080 output.

Breeze Light and Breeze Dark both applied. On both bases the screenshots show
Breeze-styled controls in the light and the dark palette; this was checked by
looking at the screenshots, not by tracing which style plugin loaded. The colour
scheme was set before launch; switching it while the app runs was not
tested.

Since then `src/app/main.cpp` picks the Qt Quick Controls style itself,
before it loads the window. If `QT_QUICK_CONTROLS_STYLE` is set, Qt uses that
style. Otherwise the app looks for `org/kde/desktop/qmldir` in the QML import
paths. When it finds one it selects `org.kde.desktop`, with Fusion for any
control that style lacks, and when it does not it selects Fusion. It also
sets `breeze` as the fallback icon theme, because outside Plasma no platform
theme names one. When the platform names no icon theme at all, it names
`hicolor`, as Qt does on xcb and Wayland. The icon notes below say why.

This was checked on 2026-10-26 on the development host (Tuxedo OS,
`qml6-module-org-kde-desktop` 6.24.0) outside Plasma: headless sway, no
`QT_QPA_PLATFORMTHEME`, private XDG directories and a merge with one
conflicted file. `QML_IMPORT_TRACE=1` during `--smoke-test` showed which style
module loaded:

| Condition | Style loaded |
| --- | --- |
| `QT_QUICK_CONTROLS_STYLE` unset | `org.kde.desktop` |
| `QT_QUICK_CONTROLS_STYLE=Material` | Material |
| `org.kde.desktop` left out of the import path through a `qt.conf` | Fusion |

Every run reached the `ready` state. Before the change, the unset case loaded
Fusion.

The KDE style reads its colours through KColorScheme, and the
`org.kde.desktop` plugin links `libKF6ColorScheme.so.6` itself, so the app
does not link `KF6::ColorScheme`. With a `kdeglobals` copied from
`BreezeDark.colors` into the private `XDG_CONFIG_HOME`, a grim capture of the
Wayland window has a dark background, RGB 32,35,38 against 239,240,241
without it.

Setting `KDE_COLOR_SCHEME_PATH` in the environment does nothing, before or
after the change. The capture with it pointing at `BreezeDark.colors` matched
the light capture pixel for pixel. KF6 reads that name only as an application
property, which `KColorSchemeManager` sets for apps that offer their own
scheme picker. Branch Truce has none, so its colours come from `kdeglobals`.

With the `hicolor` theme sway gives Qt, `QIcon::fromTheme("document-save")`
returned a null icon without the fallback and a real one with `breeze`. A
small Qt program making the same calls checked this, not the app itself.
A real Plasma session and the Kubuntu base were not part of this check.

The `offscreen`, `minimal` and `vnc` platforms name no icon theme. When
`XDG_CURRENT_DESKTOP` is also unset, Kirigami's controls plugin (6.24,
`src/controls/kirigamicontrolsplugin.cpp`) sets the theme to
`breeze-internal` and replaces the search path with its own QML directory
and `:/../icons`. The host's Kirigami packages install no `breeze-internal`
theme, and the system icon directories are no longer on the path, so the
`breeze` fallback finds nothing and `QIcon::fromTheme("document-save")` is
null. Kirigami does this only while the theme name is empty, so the app
names `hicolor` before it loads the window. Kirigami then only adds its own
directory to the fallback path, and lookups go through `hicolor` to
`breeze`, as they already did on xcb and Wayland. Two other routes were
rejected. Making `breeze-internal` resolvable would take a resource theme that
inherits Breeze plus a link to KF6BreezeIcons, because the replaced search
path holds no system directory. Setting `XDG_CURRENT_DESKTOP` would claim a
desktop that is not running.

This was checked on 2026-10-26 on the development host with the real binary
and the appearance probe from `tests/fixtures/AppearanceProbe.cpp`, private
XDG directories and no inherited environment:

| Platform, `XDG_CURRENT_DESKTOP` | Theme before | `document-save` before | Theme after | `document-save` after |
| --- | --- | --- | --- | --- |
| `offscreen`, unset | `breeze-internal` | null | `hicolor` | resolves |
| `minimal`, unset | `breeze-internal` | null | `hicolor` | resolves |
| `vnc`, unset | `breeze-internal` | null | `hicolor` | resolves |
| `offscreen`, `GNOME` | empty | resolves | `hicolor` | resolves |
| `wayland` (headless sway), unset | `hicolor` | resolves | `hicolor` | resolves |
| `xcb` (Xwayland), unset | `hicolor` | resolves | `hicolor` | resolves |

`ctest -R '^app\.startup-appearance$'` covers the unset `offscreen` case and
fails with the `hicolor` line removed from `src/app/main.cpp`.

A theme the platform names stays: on Wayland the app kept `breeze` with
`QT_QPA_PLATFORMTHEME=kde` and `XDG_CURRENT_DESKTOP=KDE`, and `Adwaita` with
`QT_QPA_PLATFORMTHEME=gtk3`. `document-save` resolved in both.

At `QT_SCALE_FACTOR=2` the text and controls doubled, but the 1920x1080
output is only 960x540 logical pixels. That was smaller than the window's
minimum size at the time, 1000x760. The window stayed at the output size,
the toolbar wrapped onto two rows, the right edge and the bottom row were
cut off, and each text pane showed about one line.

The minimum size is now 900x500 (`src/ui/Main.qml`). When the window is
shorter than its content, the page scrolls and the text panes keep at least
eight lines. The row of Save, Mark resolved and the other file actions wraps
on a narrow window. The 150% and 200% cases were then observed in containers
of both bases on the same sway output, with `QT_SCALE_FACTOR=1.5` and `2`,
Breeze Light and Breeze Dark:

| `QT_SCALE_FACTOR` on 1920x1080 | Logical size | Before (`46e0801`) | After |
| --- | --- | --- | --- |
| 1.5 | 1280x720 | Fits. Panes about 9 lines high. Kubuntu base only. | Fits without scrolling. Panes about 9 lines high, as before. |
| 2 | 960x540 | Right edge and bottom cut off. Panes about 1 line high. Kubuntu base only. | Fits the width. The page shows a scroll bar, and the panes start below the fold. |

The fixture has one conflicted file with short content and no warning, so
the 150% window had room to spare before the change too. A session with
more files, an outside-app warning or a long path is taller. The window-fit
journey in `tests/acceptance/JourneyAcceptanceTests.cpp` covers that case
offscreen. With four files and the warning, it checks at 1280x720 and
960x540 that every control and all three text panes lie inside
the window once the page has scrolled to them. It also checks that each pane
is at least eight lines high and that the mouse wheel scrolls the page. It
passes with the default offscreen style and with `org.kde.desktop`. In the
container the 200% screenshot shows only the top of the page. Scrolling
there was not exercised. The script now opens the 1.5 windows itself, so the
steps in "How to reproduce" write `scale1.5-BreezeLight` and
`scale1.5-BreezeDark` screenshots. The 150% results above were taken before
that, and before the package depended on `qt6-wayland`. The other checks in those runs
gave 36 passed, 0 failed and 1 warning on each base; the warning was the
closure-only Wayland check. Plasma's own output scaling, which KWin
applies rather than `QT_SCALE_FACTOR`, was not available, and 125% was not
measured.

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
The second line contradicted the first, because its visibility in
`src/ui/Main.qml` did not depend on the selected file's state. The label is now
hidden once Git has staged a result for the selected file, and paths Git has
not launched still show it. The acceptance suite checks both cases offscreen
with real `git mergetool`. The container run has not been repeated since.

### Organization name

The app sets an organization domain (`branch-truce.local`) and no organization
name. `KAboutData::setApplicationData` sets the domain only, and KDE
applications that use it keep their data under `~/.local/share/<app>`. The
name would add nothing: the desktop entry, the application ID and the
mergetool intake path do not use it. On Linux Qt does put it into
`QStandardPaths::AppLocalDataLocation`, so setting one moves that directory,
and with it the recovery journals of earlier runs.

`SessionModel::recoveryRoot()` no longer reads `AppLocalDataLocation`. It is
`GenericDataLocation` plus `/Branch Truce/recovery/v1`, which is the path the
app already used with no organization name, so stores written by earlier
builds are found where they are and nothing is moved. A later change that sets
an organization name leaves the root alone. `app.session-model` pins both: the
root, and a store written at the earlier path with an organization name set.

## What would close the real-desktop rows

On each real desktop: install the artifact with `apt` from the file, record
`/etc/os-release`, `plasmashell --version`, `kwin_wayland --version` and the
installed Qt/KDE package versions, then repeat the matrix above in a Plasma
Wayland session. That means launching from the application menu, using
KWin output scales of 100%, 150% and 200%, switching between Breeze Light
and Dark while the app runs, typing composed text through IBus or Fcitx 5,
and having Orca read the conflict view. Record any failure as found.
