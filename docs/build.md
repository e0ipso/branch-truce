# Build Branch Truce

The build separates `branch_truce_core`, `branch_truce_controller`,
`branch_truce_git`, `branch_truce_storage`, `branch_truce_ui` and the
`branch-truce` executable. Git provides constrained asynchronous inspection and
conversion; storage provides lossless text codecs. See the
[supported conversion envelope](text-conversion.md). The window opens the launched
repository's complete current conflict inventory, including outside-app work and
load errors with persistent reasons. Selecting a file by its exact byte-derived ID
opens read-only captured sources around the preserved result preview; the common
earlier version is available on demand. The session warning keeps outside-app work
visible while supported files are selected. Editing and resolution controls remain
separate implementation work.
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
QuickControls2 and KF6 Kirigami. Qt Widgets supplies the QApplication bootstrap
for desktop controls; the interface is Qt Quick/Kirigami. The current API floor
is Qt 6.6 and KF6 6.0, below the selected reference stack Qt 6.10.2/KF6 6.24.
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

Preparation enables `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion`
and `-Werror` for project C++ targets. Direct CMake users can set
`BRANCH_TRUCE_WARNINGS_AS_ERRORS`, `BRANCH_TRUCE_SANITIZERS`,
`BRANCH_TRUCE_BUILD_DESKTOP` and `BUILD_TESTING` explicitly. Sanitizers enable
AddressSanitizer and UndefinedBehaviorSanitizer with frame pointers. The default
sanitizer preparation tests the core without Qt; desktop sanitizers can be
configured separately with both desktop and sanitizer options ON.

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
is the blocking parent launcher. Its transport and lifecycle are recorded in
[the mergetool launcher note](mergetool-launcher.md). That note does not declare
mergetool compatibility.

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
Native Kubuntu 26.04 builds, packaging/installation, Wayland, theme/scaling and
accessibility checks remain required before platform compatibility claims.
Build separate artifacts on each OS base. A user-owned prefix via
`CMAKE_PREFIX_PATH` can supply compatible Qt/KDE packages, but must keep their ABI
and runtime modules consistent; no such alternate toolchain was needed here.
