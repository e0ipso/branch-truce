# Build Branch Truce

The build separates `branch_truce_core`, `branch_truce_controller`,
`branch_truce_git`, `branch_truce_storage`, `branch_truce_ui` and the
`branch-truce` executable. Git provides constrained asynchronous inspection and
conversion; storage provides lossless text codecs. See the
[supported conversion envelope](text-conversion.md). The window opens the launched
repository's complete current conflict inventory, including outside-app work and
load errors with persistent reasons. Selecting a file by its exact byte-derived ID
opens read-only captured sources around the editable result; the common
earlier version is available on demand. The session warning keeps outside-app work
visible while supported files are selected. Using the app, including Save and
Mark resolved, is described in the [README](../README.md#launch).
The core uses C++20 and the standard library without Qt or platform I/O.

## Machine provisioning

Run on Tuxedo OS 24.04 with its vendor Qt/KDE repositories, or Kubuntu 26.04:

```sh
./tools/provision-debian.sh --dry-run
sudo ./tools/provision-debian.sh --install
./tools/provision-debian.sh --check
```

The script installs only missing packages. Repeated runs with all packages
installed exit successfully without calling apt or changing the system.
`--dry-run` uses apt simulation when packages are missing. Provisioning requires
network access; building and running the installed application do not.
Do not call provisioning from worktree preparation. Agents prohibited from
system changes must report missing packages to the environment owner.

CMake >= 3.28, a GCC 13.3-compatible C++20 compiler, Make and Git >= 2.43
are required. Desktop configuration checks ECM, Qt Core/Gui/Widgets/Network/Qml/Quick/
QuickControls2, KF6 Kirigami and KF6 SyntaxHighlighting. Qt Widgets supplies the
QApplication bootstrap for desktop controls; the interface is Qt Quick/Kirigami.
The current API floor is Qt 6.6 and KF6 6.0, below the selected reference stack
Qt 6.10.2/KF6 6.24.
Stock Ubuntu noble's Qt 6.4 cannot build the desktop target. Do not substitute
stock packages for Tuxedo's vendor packages.

## Configure, build and test

From any checkout, run these commands. Each preparation checks shared tools
and configures that checkout; it never installs packages.

```sh
./tools/prepare-worktree.sh desktop
cmake --build build/desktop --parallel 2
ctest --test-dir build/desktop --output-on-failure
cmake --build build/desktop --target branch_truce_ui_qmllint

./tools/prepare-worktree.sh core
cmake --build build/core --parallel 2
ctest --test-dir build/core --output-on-failure

./tools/prepare-worktree.sh sanitize
cmake --build build/sanitize --parallel 2
ctest --test-dir build/sanitize --output-on-failure

/usr/bin/env -i PATH=/usr/bin:/bin LC_ALL=C /bin/bash tools/check-shell-syntax.sh
```

The QML lint target fails on any qmllint warning, because
`src/ui/.qmllint.ini` sets `MaxWarnings=0`. The desktop CTest run includes it
as `ui.qmllint`.

CTest entries that start the real binary or load Qt Quick outside the unit harness run through
`tests/with-private-xdg.sh`. It gives them a private `HOME` and XDG directories, removes
them afterwards, and fails the entry if the run created anything under
`<data home>/Branch Truce/recovery` of the invoking user, so a test run never adds
sessions to the recovery list of the developer's installed app. The data home is
`XDG_DATA_HOME` when it is absolute, otherwise `$HOME/.local/share`. An entry that
reports that failure while the app is open elsewhere may be seeing a session the
developer started; rerun it with the app closed. `lifecycle.private-xdg-isolation`
checks the wrapper against a stand-in data directory.

Preparation enables `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion`
and `-Werror` for project C++ targets. Direct CMake users can set
`BRANCH_TRUCE_WARNINGS_AS_ERRORS`, `BRANCH_TRUCE_SANITIZERS`,
`BRANCH_TRUCE_BUILD_DESKTOP` and `BUILD_TESTING` explicitly. Sanitizers enable
AddressSanitizer and UndefinedBehaviorSanitizer with frame pointers. The default
sanitizer preparation tests the core without Qt, and CI runs it as
`./tools/prepare-worktree.sh sanitize` followed by the build and CTest above.
Desktop sanitizers can be configured separately with both desktop and
sanitizer options ON; no preparation mode or CI job runs that configuration.

`BRANCH_TRUCE_SYNTAX_HIGHLIGHTING` defaults to ON. A desktop configuration then
requires KF6SyntaxHighlighting 6.0 or later (`libkf6syntaxhighlighting-dev`) and
stops with an error naming that package when it is missing. Set the option OFF to
build the desktop app without syntax highlighting; the panes then show plain
text. Each pane still gets a plain `PaneHighlighter` for decorations, and the
`app.syntax-highlighting` test checks that it stays attached.

CTest includes the core build canary, revisioned session tests, correspondence
command fixtures and CLI guards. Desktop builds also test the Qt session model,
the asynchronous Git adapter in temporary repositories, and the accepted editor
fixture. The session, correspondence, model and Git tests report case and
assertion counts; the editor reports assertions.
The Git test includes the real 30-second operation deadline and has a 90-second
suite timeout. Its scripted executables live outside the temporary repositories.

The desktop startup canary loads the embedded QML and Kirigami with offscreen
software rendering, then waits for repository discovery and file loading. The
executable accepts `--smoke-test` to print a `BRANCH_TRUCE_SESSION=` JSON record and
exit after startup settles, including an unsupported or invalid selection. Its
exit code checks window loading; the record describes session opening separately.
GUI tests retain a 30-second timeout. Startup does not report an assertion count.

Launch directly into a repository's current merge or rebase with
`build/desktop/src/app/branch-truce /path/to/repository`. A subdirectory selects
its containing worktree; omitting the directory uses the current directory.
Discovery verifies the operation and source identities locally. Unsupported
operations and files remain visible with an explanation, and unreadable files
never appear as empty supported results. Startup does not change the worktree or
index. The session uses no account or network and covers the conflicts present at
launch. Continue and commit in the usual Git workflow. `branch-truce --mergetool`
is the blocking parent launcher. [mergetool.md](mergetool.md) gives the tested
Git configuration, and [the mergetool launcher note](mergetool-launcher.md)
records its transport, lifecycle and the behaviours tested with a real parent.

Build directories are disposable. The default root `build/` directory is ignored
by Git. An optional second absolute argument to `prepare-worktree.sh` places
generated files outside the checkout.

For evidence collection, `./tools/verification-context.sh source` prints the path
of an environment file under `/tmp/branch-truce-verification.*`. Source that
file in the calling shell before executing or validating a check. Capture again
before each validation. Use `desktop`, `core` or `sanitize` instead of `source`
after configuring that build to include its generated test membership and
available binaries. Retain the adjacent manifests with the check receipts.
CTest output logs are excluded from artifact identity.

The stamps record tool contents and versions, installed package versions,
selected environment variables with set/unset state, platform, committed HEAD
and selected build artifacts. Package versions do not establish the contents
of compiler headers, QML plugins or user configuration. These observations alone
cannot justify a complete build/runtime dependency declaration. The normalized
shell syntax command above has a separate stamp covering env, Bash and their
linked libraries. That stamp is empty when exported loader overrides are present,
so the evidence helper must treat its declaration as incomplete in that case.

## Preview

After a desktop build, `tools/preview-app.sh` takes a light and a dark
screenshot of the window on a real conflict. It runs on the current sway
session, such as the headless one agents use here:

```sh
tools/preview-app.sh                              # build/desktop/src/app/branch-truce
PREVIEW_FILE=tracked.txt tools/preview-app.sh     # the three comparison panes on a text file
PREVIEW_FILE=notes.md tools/preview-app.sh        # the same on a Markdown file
PREVIEW_SIZE=1280x800 tools/preview-app.sh        # at a 1280x800 logical screen size
tools/preview-app.sh /path/to/branch-truce        # another build
```

For each of BreezeLight and BreezeDark, the script builds a repository stopped
on a merge with three conflicting files: `tracked.txt` (text), `notes.md`
(Markdown) and `image.bin` (binary, listed as outside the app). It starts the
app fullscreen with a private home and XDG directories and no D-Bus session.
Once the session's recovery journal appears it waits `PREVIEW_SETTLE` seconds
(default 1), saves the window with grim and stops the app. stdout gets one PNG
path per scheme. The PNGs go to `PREVIEW_OUTPUT`, or to a new
`/tmp/bt-preview-shots.*` directory that you remove when done. The fixture
directory is deleted on exit. The script writes nothing to the checkout or
`$HOME` and stops only the app it started.

Without `PREVIEW_FILE` the app selects its first file, `image.bin`, so the
captures show the outside-app notice instead of the comparison. `PREVIEW_FILE`
names one of the three fixture files and passes it to the app's hidden
`--mergetool-path` option. With `tracked.txt` or `notes.md` the captures show
that file in the left source, editable result and right source panes. Any other
value stops the script before it launches the app, because the app would fall
back to its first file.

Each launch gets `PREVIEW_TIMEOUT` seconds (default 180) to show its window and
load its files. On an idle host that takes under 15 seconds, but the app renders
in software on the headless session and a host busy with builds can stretch it
past a minute. On timeout the script prints the end of the app's log.

`PREVIEW_SIZE=WIDTHxHEIGHT` captures at that logical screen size instead of the
size of the focused output. The script adds a temporary headless output with
`swaymsg create_output` and finds its name by comparing the output list before
and after, because sway numbers new outputs with a counter. It sets that
output's mode and scale 1, opens the app there and checks that the fullscreen
window has the requested size. On every exit, including a failure or an
interrupt, it unplugs the output and gives focus back to the output that had
it. It never changes another output's mode, so `HEADLESS-1`, which VNC shows,
keeps its size. The PNG names end in the size, such as
`branch-truce-BreezeLight-1280x800.png`.
Agents sharing the headless session run the script under
`flock /tmp/branch-truce-preview.lock`, so two runs never add outputs at once.

`tools/merge-fixture.sh` builds the fixture. The installed-artifact validator
uses the same function inside its container for its single `tracked.txt`
conflict.

## Install

A desktop build installs four files under `CMAKE_INSTALL_PREFIX`, placed by
ECM's KDE install directories:

- `bin/branch-truce`, the GUI. With `--mergetool` it is the blocking launcher.
- `share/applications/local.branch_truce.BranchTruce.desktop`
- `share/metainfo/local.branch_truce.BranchTruce.metainfo.xml`
- `share/icons/hicolor/scalable/apps/local.branch_truce.BranchTruce.svg`

The QML module is compiled to QML bytecode inside the executable, not to C++
([target-platforms.md](target-platforms.md#qml-bytecode-instead-of-compiled-c)
explains why), so no QML directory is installed and the installed program
reads nothing from the source or build tree. Qt, Kirigami and the KDE style
come from system packages. The
application ID is the reverse-DNS form of the organization domain
`branch-truce.local`. The GUI sets it as its desktop file name, which is how
Plasma matches a window to the entry and icon. For a prefix outside the system
data directories, add `<prefix>/share` to `XDG_DATA_DIRS` so menus and icon
lookup find the files. The per-OS `.deb` packages, their build containers and
their inspection are described in [target-platforms.md](target-platforms.md).

```sh
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DCMAKE_INSTALL_PREFIX="$prefix"
cmake --build "$build" --parallel 2
cmake --install "$build"
```

To check an install without touching the system or your home, run:

```sh
./tools/verify-install-prefix.sh --mergetool-tests build/desktop/tests/branch_truce_mergetool_tests
```

The script copies the checkout's tracked and untracked non-ignored files into
a fixture under `${TMPDIR:-/tmp}`, builds and installs a Release prefix, and
deletes the copied sources and build tree before launching anything. It then:

1. Compares the deployed file list with the four files above and runs
   `desktop-file-validate` and `appstreamcli validate --no-net`.
2. Runs the installed `branch-truce --version` and
   `branch-truce --smoke-test <repo>` on a disposable merge conflict.
3. Starts the desktop entry with `gio launch`, attaches the installed
   `branch-truce --mergetool` to that GUI, and checks that the launcher blocks
   and leaves the file unmerged until the GUI closes.
4. Runs `git mergetool` with the installed launcher, which starts the installed
   GUI itself, and checks the same blocking and release.
5. With `--mergetool-tests`, runs the mergetool protocol suite against the
   installed executable. This covers the resolved handoff, where the launcher
   exits 0 and Git stages the file.

Launches run with `env -i`, an isolated `HOME` and XDG directories inside the
fixture, and offscreen software rendering. The script prints every install and
launch command, and removes the fixture and any process it started on exit.
The session socket sits in the fixture's `XDG_RUNTIME_DIR`, under
`branch-truce/`. A runtime path too long for a Unix socket would move it to
`/tmp/branch-truce-<uid>`, outside the fixture, so the script refuses such a
fixture path.

The metainfo has no homepage or developer yet. `appstreamcli` reports
`url-homepage-missing` as a warning, which the script downgrades to info so
the finding stays visible. The ID's `local` TLD is reported as info.

On 2026-10-25 every step above passed on the development host: four files
deployed, `RUNPATH` `$ORIGIN:$ORIGIN/../lib/x86_64-linux-gnu`, both validators
passed, the smoke test opened the merge session, the desktop-launched and the
launcher-started GUIs each held their launcher until they closed (exit 1,
file unmerged), and the protocol suite passed 7/7 against the installed
executable. A Plasma menu launch, Wayland and a physical display were not
exercised.

## Coordinator setup hook

After this scaffold lands, run from the newly prepared checkout:

```sh
./tools/prepare-worktree.sh desktop
```

After committing the scaffold, verify clean preparation with:

```sh
./tools/verify-clean-worktree.sh
```

This creates a disposable detached worktree of HEAD, builds and runs only the
core/startup canaries in desktop, core and sanitizer configurations, checks QML,
confirms the checkout stayed clean after each default preparation and after the
checks, and removes the fixture. Build output uses the default `build/<mode>`
directories inside the fixture checkout, including the setup hook's
`./tools/prepare-worktree.sh desktop` command. Failure logs must be retained by
the caller.

The desktop preparation above is the per-worktree setup command to register with Sift. It configures;
checks remain separate `cmake --build` and `ctest` commands above. Hook registration
belongs to the coordinator and is deliberately deferred.

## Platform evidence

Development verification runs on Tuxedo OS 24.04.4 with GCC 13.3.0, CMake 4.2.3,
Git 2.43.0, Qt 6.10.2 and ECM/Kirigami 6.24.0. No system packages were changed.
The provisioning check and repeated install-mode reruns report no changes on this
prepared host. Desktop CTest passed 2/2 canaries; independent core and sanitized
core CTest each passed 1/1. QML lint completed without diagnostics. The same
checks passed in a disposable detached worktree, which stayed clean and was
removed afterward. These are scaffold checks only.
The install-prefix check under [Install](#install) passed on this host.
Separate Kubuntu 26.04 and Tuxedo OS 24.04 packages are built and inspected
in containers of each base, as [target-platforms.md](target-platforms.md)
records. Installing them on the real desktops, Wayland, theme/scaling and
accessibility checks remain required before platform compatibility claims.
Build separate artifacts on each OS base. A user-owned prefix via
`CMAKE_PREFIX_PATH` can supply compatible Qt/KDE packages, but must keep their ABI
and runtime modules consistent; no such alternate toolchain was needed here.
