# Target platforms and artifacts

Branch Truce ships one `.deb` per OS base. Each is built in a container from
that base's own archive and inspected on a clean copy of the same base.
Neither package is assumed to install on the other base. Installation and
runtime compatibility on the real desktops stay pending until both artifacts
are validated on those desktops.

## OS bases

| | Kubuntu 26.04 LTS | Tuxedo OS 24.04 |
| --- | --- | --- |
| Role | Selected KDE/API baseline | Development host and second required target |
| Base image | `ubuntu:26.04@sha256:f144425ff09be612d6d9ad965196e9cdc23dae1f42110a8a11a3e9a8198759f7` | `ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55` |
| Container OS | Ubuntu 26.04.1 LTS, resolute | Ubuntu 24.04.5 LTS, noble |
| Package sources | Ubuntu resolute archive in the image | Only Tuxedo OS sources: [packaging/tuxedo/tuxedo-os.sources](../packaging/tuxedo/tuxedo-os.sources) |
| Definition | [packaging/kubuntu/Dockerfile](../packaging/kubuntu/Dockerfile) | [packaging/tuxedo/Dockerfile](../packaging/tuxedo/Dockerfile) |
| Package version | `0.1.0+kubuntu26.04` | `0.1.0+tuxedo24.04` |
| Artifact | `branch-truce_0.1.0+kubuntu26.04_amd64.deb` | `branch-truce_0.1.0+tuxedo24.04_amd64.deb` |

Kubuntu uses the Ubuntu archive; Kubuntu is Ubuntu with KDE packages from
that archive. The Tuxedo sources are copied from a Tuxedo OS 24.04.4 host and
include Tuxedo's mirror of the Ubuntu archive. The vendor Qt/KDE stack comes
from `txos.tuxedocomputers.com/ubuntu-plasma`. The archive key is
[packaging/tuxedo/tuxedo-archive-keyring.asc](../packaging/tuxedo/tuxedo-archive-keyring.asc),
fingerprint `E5D0C320BBCE8D21CDF60DD5120ED28D54840598`, exported from that
host's `tuxedo-repository` package. Stock noble ships Qt 6.4, which cannot
build the desktop target. The container keeps Ubuntu's `/etc/os-release`,
because Tuxedo's branding packages are not installed. Every installed package
comes from the Tuxedo sources after `apt-get dist-upgrade`. The one exception
is `ca-certificates`, which is fetched from stock noble first because the
Tuxedo repositories use HTTPS.

## Toolchains and Qt/KDE packages

Resolved in the build images on 2026-10-25. Each artifact's `.manifest` keeps
the same list for its build.

| Package | Kubuntu 26.04 | Tuxedo OS 24.04 |
| --- | --- | --- |
| Compiler | GCC 15.2.0 (`g++` 4:15.2.0-5ubuntu1) | GCC 13.3.0 (`g++` 4:13.2.0-7ubuntu1) |
| libc6 | 2.43-2ubuntu2.4 | 2.39-0ubuntu8.9 |
| libstdc++6 | 16-20260322-1ubuntu1 | 14.2.0-4ubuntu2~24.04.1 |
| cmake | 4.2.3-2ubuntu2 | 4.2.3-2ubuntu2~tux1 |
| debhelper | 13.31ubuntu1 | 13.14.1ubuntu5 |
| extra-cmake-modules | 6.24.0-0ubuntu1 | 6.24.0-0ubuntu1~tux1 |
| qt6-base-dev | 6.10.2+dfsg-7 | 6.10.2+dfsg-7~tux1 |
| qt6-declarative-dev | 6.10.2+dfsg-3 | 6.10.2+dfsg-3~tux1 |
| libkirigami-dev | 6.24.0-0ubuntu1 | 6.24.0-0ubuntu1~tux1 |
| libkf6syntaxhighlighting-dev | 6.24.0-0ubuntu1 | 6.24.0-0ubuntu1~tux1 |
| git (runtime) | 1:2.53.0-1ubuntu1.1 | 1:2.43.0-1ubuntu7.4 |

The Tuxedo versions match the development host's installed packages.
`libkf6syntaxhighlighting-dev` was added on 2026-10-26. Stock noble does not
publish it, so the Tuxedo image installs it from `ubuntu-plasma`.

## Dependency floors

Build floors are the API floors in [CMakeLists.txt](../CMakeLists.txt):
CMake 3.28, ECM 6.0, Qt 6.6, KF6 Kirigami 6.0 and KF6 SyntaxHighlighting 6.0.
They are the
`Build-Depends` of [packaging/debian/control](../packaging/debian/control),
which both bases share. The build images install them with `apt-get build-dep`.

Runtime library floors come from `dpkg-shlibdeps` against each base's own
shlibs and symbols files, so they differ:

| Depends from `${shlibs:Depends}` | Kubuntu 26.04 | Tuxedo OS 24.04 |
| --- | --- | --- |
| libc6 | >= 2.34 | >= 2.34 |
| libgcc-s1 | >= 3.3.1 | >= 3.3.1 |
| libstdc++6 | >= 13 | >= 13 |
| libqt6core6t64 | >= 6.10.2 | >= 6.10.2+dfsg |
| libqt6gui6 | >= 6.6~ | >= 6.10.2+dfsg |
| libqt6network6 | >= 6.6~ | >= 6.10.2+dfsg |
| libqt6qml6 | >= 6.6.0 | >= 6.10.2+dfsg |
| libqt6quick6 | >= 6.6.0 | >= 6.10.2+dfsg |
| libqt6widgets6 | >= 6.6~ | >= 6.10.2+dfsg |
| libkf6syntaxhighlighting6 | >= 6.0.0 | >= 6.24.0 |

Both binaries need `GLIBC_2.34`, `GLIBCXX_3.4.31`, `CXXABI_1.3.9` and
`GCC_3.3.1` at most, and only the `Qt_6` and `Qt_6.10` Qt symbol versions.
Tuxedo's shlibs files give every Qt library its packaged version as the floor,
so its Qt floors are 6.10.2 whatever the binary uses.

### QML bytecode instead of compiled C++

[src/ui/CMakeLists.txt](../src/ui/CMakeLists.txt) passes `--only-bytecode`
to `qmlcachegen`. The executable embeds the QML as bytecode, and the QML
engine runs its bindings and functions. By default `qmlcachegen` also
compiles them to C++. That C++ calls
`QQmlPrivate::AOTCompiledContext::mark`, which carries the
`Qt_6_PRIVATE_API` symbol version, and Qt only keeps private API stable
within one release. On Kubuntu, `dpkg-shlibdeps` turned it into
`qt6-base-private-abi (= 6.10.2)` and `qt6-declarative-private-abi (= 6.10.2)`,
so any Qt update made the package uninstallable until it was rebuilt.
Tuxedo's Qt packages declare no private ABI, so the Tuxedo package would have
installed after a Qt update and could then fail to start.

With bytecode only, neither binary has a `Qt_6_PRIVATE_API` symbol, neither
package depends on a private ABI, and Kubuntu's `libqt6qml6` floor drops from
6.10.2 to 6.6.0. `tools/inspect-artifact.sh` fails a binary that has private
Qt symbols but no exact private ABI pin in `Depends`.

Startup cost was measured on the Tuxedo OS 24.04 development host, Qt
6.10.2, with both binaries built with the package's `dpkg-buildflags`.
`branch-truce --smoke-test` on a disposable merge conflict ran 30 times per
binary, alternating, offscreen with software rendering. It loads the window
and opens the session. The host load average was between 6 and 12.

| Build | Median | Mean | Min | Max |
| --- | --- | --- | --- | --- |
| QML compiled to C++ | 950 ms | 936 ms | 688 ms | 1064 ms |
| QML bytecode only | 939 ms | 931 ms | 790 ms | 1233 ms |

The difference is within the run-to-run spread. Startup was not measured on
Kubuntu, and the cost of running bindings in the QML engine during
interaction was not measured.

`dpkg-shlibdeps` cannot see QML imports, so `Depends` also lists the QML
modules [src/ui](../src/ui) imports, with the API floors: `qml6-module-qtquick`,
`qml6-module-qtquick-controls`, `qml6-module-qtquick-layouts`,
`qml6-module-qtquick-shapes` (>= 6.6~) and
`qml6-module-org-kde-kirigami` (>= 6.0~), plus `qml6-module-qtquick-templates`,
`qml6-module-qtquick-window` and `qml6-module-qtqml-workerscript` (>= 6.6~),
which those modules load. The application runs the installed Git, so it also depends on
`git (>= 1:2.43~)`. The desktop styling is in `Depends` too:
`qml6-module-org-kde-desktop`, `kde-style-breeze`, `kf6-breeze-icon-theme` and
`hicolor-icon-theme`. Breeze icons are SVG, so `qt6-svg-plugins (>= 6.6~)`,
Qt's SVG icon engine, is listed too; without it every Breeze icon is null.
`qt6-wayland` ships the Wayland shell
integrations; without it the app aborts on a Wayland session that does not
bring the package some other way. The app selects the `org.kde.desktop`
Qt Quick Controls style, so Kirigami loads the `org.kde.desktop` platform
plugin, and `qml6-module-org-kde-desktop` ships both. The comparison panes
highlight syntax through the KF6 SyntaxHighlighting C++ library, which
`${shlibs:Depends}` adds as `libkf6syntaxhighlighting6`; that package pulls in
`libkf6syntaxhighlighting-data`. No QML syntax-highlighting module is needed.
`plasma-integration` is the only `Recommends`. The package starts on Wayland
without it, and Plasma desktops have it through `plasma-workspace`. This list
is the one the CI release packages use; `packaging/scripts/check-package.sh`
checks it.

## Packaging choice

A native `.deb` built by debhelper's CMake build system, as the technical
spec's initial candidate. Flatpak would need its own design for Git access.

- [packaging/debian/](../packaging/debian/) holds `control`, `rules`,
  `copyright`, `compat` and `source/format`, shared by both bases and by the CI
  release packages of `packaging/scripts/package-release.sh`. `rules` stops
  unless `BRANCH_TRUCE_VERSION` is set, and configures with that version,
  `BUILD_TESTING=OFF` and `BRANCH_TRUCE_WARNINGS_AS_ERRORS=ON`; tests run
  from development builds and in CI. `control` declares `debhelper-compat`,
  so both build scripts drop `compat` from the copied `debian/` directory.
- [tools/build-artifact.sh](../tools/build-artifact.sh) takes the release
  version from `package.json`, converts it with
  `packaging/scripts/native-version.sh`, and passes it to `rules` as
  `BRANCH_TRUCE_VERSION`. It writes `debian/changelog` with the base's suite
  (`resolute` or `noble`) and the `+kubuntu26.04` or `+tuxedo24.04` version
  suffix, so the two packages have different versions as well as different
  file names. The CI release packages use the same `debian/` directory but
  their own version form and file names.
- The build uses each base's default `dpkg-buildflags`, including LTO,
  `_FORTIFY_SOURCE=3` and `hardening=+all`. No debug-symbol package is built.
- `debian/copyright` states all rights reserved, with the metadata under
  CC0-1.0 as the AppStream metainfo declares. The packages built here are
  local validation artifacts.
- The binary keeps the `RUNPATH` `$ORIGIN:$ORIGIN/../lib/x86_64-linux-gnu`
  that the CMake build sets. Under `/usr/bin` it points at system directories
  only.

## Build and inspect

Docker or another engine named by `CONTAINER_ENGINE` is required. Nothing is
installed on the host.

```sh
tools/build-artifact.sh --target kubuntu --output "$out"
tools/inspect-artifact.sh --target kubuntu --package "$out/branch-truce_0.1.0+kubuntu26.04_amd64.deb" --output "$out"
tools/build-artifact.sh --target tuxedo --output "$out"
tools/inspect-artifact.sh --target tuxedo --package "$out/branch-truce_0.1.0+tuxedo24.04_amd64.deb" --output "$out"
```

`build-artifact.sh` builds two images per base from its Dockerfile, with
`packaging/` as the build context:

- `branch-truce-artifact:kubuntu-26.04-target` and
  `branch-truce-artifact:tuxedo-24.04-target`: the clean OS base.
- `branch-truce-artifact:kubuntu-26.04-build` and
  `branch-truce-artifact:tuxedo-24.04-build`: the base plus the toolchain
  and `Build-Depends`.

The images stay for reuse. Image builds fetch from the archives; set
`BRANCH_TRUCE_REFRESH` to a new value to fetch again instead of reusing
cached layers. Remove the images with `docker image rm`.

The script copies the checkout's `CMakeLists.txt`, `cmake/`, `src/` and
`packaging/` (tracked and untracked, not ignored) into a temporary directory and
runs `dpkg-buildpackage -b` there with `--network none` and `parallel=2`. The
build path is always `/work/branch-truce-<version>`, and `SOURCE_DATE_EPOCH`
is the author time of the last commit that changed those paths. The output directory gets the `.deb`, its
`.buildinfo` and a `.manifest`. The manifest records the artifact's SHA-256,
the base image, the build image ID and a SHA-256 of its layer list, a
SHA-256 of the source snapshot, `HEAD`,
the number of changed source paths and the toolchain list above.

`inspect-artifact.sh` starts a throwaway container from the `-target` image,
which needs network access for the package lists, and checks:

1. Metadata and contents: the version suffix, `amd64`, exactly the four
   installed files from [build.md](build.md#install) plus the Debian
   `copyright` and `changelog.gz`, and no maintainer scripts.
2. `apt-get install --no-install-recommends` of the `.deb`. The packages it
   adds are the runtime closure and are printed with their versions.
3. Every linked library resolves. Each `NEEDED` library's package is named in
   `Depends`. The `RUNPATH` and the highest required symbol versions are
   printed. A `Qt_6_PRIVATE_API` symbol fails the check unless `Depends`
   pins a Qt private ABI to an exact version.
4. Each QML module imported by `src/ui` has a `qmldir` from a package named in
   `Depends`.
5. `desktop-file-validate` and `appstreamcli validate --no-net`.
6. The installed `branch-truce --version` and `--smoke-test` on a disposable
   merge conflict, offscreen with software rendering. This loads the compiled
   QML, Qt Quick Controls and Kirigami from the installed closure only.
7. The installed application resolves the Breeze icon `document-save`. The
   inspection builds `tests/fixtures/AppearanceProbe.cpp` in the `-build` image
   of the base, preloads it into `branch-truce --smoke-test` with no desktop
   named and reads the `documentSave` field of its record. A null icon fails,
   which is what a closure without `qt6-svg-plugins` gives.
8. `apt-get remove` leaves none of the program files.

## Results on 2026-10-25

Both artifacts were built from the same committed sources, with the QML
compiled to bytecode only, and passed every inspection step. These builds
predate the shared `debian/` directory above: their `control` had the desktop
styling and `qt6-wayland` under `Recommends`, and their `rules` took no
version. A second build
of each, from cached images, was byte-identical to the first.

| | Kubuntu 26.04 | Tuxedo OS 24.04 |
| --- | --- | --- |
| `.deb` SHA-256 | `df24668acccf40eea8726e96f9d2eed6e4ba899eeee20ba6afc6992c23805b71` | `1cbe296f49501a1aefa82d0fdfc08717ea9bb696d20bba00e6fc50e9cb5e8826` |
| Size | 502176 bytes | 486220 bytes |
| Build warnings | none | none |
| Packages added by install | 155 | 146 |
| Linked libraries resolved | 83 | 88 |
| Git in the closure | 2.53.0 | 2.43.0 |
| Smoke test | merge session ready, `tracked.txt` open | merge session ready, `tracked.txt` open |

On both bases the install pulled the four declared QML modules and, through
their own dependencies, `qml6-module-qtqml`, `-qtqml-models`,
`-qtqml-workerscript`, `-qt-labs-qmlmodels`, `-qtquick-shapes`,
`-qtquick-templates` and `-qtquick-window`, all at the base's Qt 6.10.2.
Kirigami came in at 6.24.0 with its private libraries. The `.inspection`
report kept beside each artifact lists the full closure with versions.
`appstreamcli` reported three infos, `cid-maybe-not-rdns`,
`url-homepage-missing` and `developer-info-missing`, which stay open until the
application identity and metadata are confirmed before publishing.

## Compatibility status

Pending. These results come from containers of each base, not from Kubuntu
or Tuxedo OS desktops. The smoke test runs offscreen, without Plasma,
Wayland, the KDE style or a display, and the containers' runtime closure is
what `--no-install-recommends` installs on a minimal base. Each artifact
still has to be installed on its real desktop, with the runtime matrix
recorded, before any compatibility claim.

[platform-validation.md](platform-validation.md) records the installed-package
runs of `tools/validate-installed-artifact.sh`: Wayland, scale factors,
Breeze Light and Dark, editor input, AT-SPI and a real `git mergetool`
handoff, all in containers of each base on a headless compositor. Its
real-desktop rows are unavailable.
