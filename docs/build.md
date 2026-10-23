# Build Branch Truce

The build separates `branch_truce_core`, `branch_truce_controller`,
`branch_truce_git`, `branch_truce_storage`, `branch_truce_ui` and the
`branch-truce` executable. Git and storage currently reserve compilation
boundaries; they perform no operations. The window is a startup scaffold.
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
are required. Desktop configuration checks ECM, Qt Core/Gui/Widgets/Qml/Quick/
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
```

Preparation enables `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion`
and `-Werror` for project C++ targets. Direct CMake users can set
`BRANCH_TRUCE_WARNINGS_AS_ERRORS`, `BRANCH_TRUCE_SANITIZERS`,
`BRANCH_TRUCE_BUILD_DESKTOP` and `BUILD_TESTING` explicitly. Sanitizers enable
AddressSanitizer and UndefinedBehaviorSanitizer with frame pointers. The default
sanitizer preparation tests the core without Qt; desktop sanitizers can be
configured separately with both desktop and sanitizer options ON.

CTest has a core build canary with one version assertion, plus a desktop startup
canary that loads the embedded QML and Kirigami with offscreen software rendering.
These establish build wiring, not conflict-resolution behavior. The executable
also accepts `--smoke-test` for the same startup check. GUI tests must retain a
30-second timeout. Test output does not report an assertion count for startup.

Build directories are disposable. The default root `build/` directory is ignored
by Git. An optional second absolute argument to `prepare-worktree.sh` places
generated files outside the checkout.

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
