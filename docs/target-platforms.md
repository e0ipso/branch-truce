# Target platforms and artifacts

## Current native release targets

CI builds one package per target from the same selected revision and application
version. There is no separate Tuxedo release build.

| Target | Native artifact | Image definition |
| --- | --- | --- |
| Kubuntu 26.04 LTS, amd64 | `branch-truce_<version>_amd64-ubuntu26.04.deb` | [kubuntu.Dockerfile](../packaging/ci/kubuntu.Dockerfile) |
| Fedora 44, x86_64 | `branch-truce-<native-version>-1.fc44.x86_64.rpm` | [fedora.Dockerfile](../packaging/ci/fedora.Dockerfile) |
| Arch Linux rolling, x86_64 | `branch-truce-<native-version>-1-x86_64.pkg.tar.zst` | [arch.Dockerfile](../packaging/ci/arch.Dockerfile) |

The definitions pin base-image digests. Arch uses the official x86_64 base
manifest and a full `pacman -Syu`; Fedora uses native rpmbuild and Arch uses
unprivileged makepkg. All recipes retain QML bytecode, distro hardening, Git,
QML modules, KDE style/icons, SVG and Wayland runtime dependencies. Debian/RPM
prereleases use tilde ordering; Arch uses its native alphabetic prerelease form.
[package-info.sh](../packaging/scripts/package-info.sh) supplies the exact
filename, native version and architecture. [README installation commands](../README.md#install)
and [build instructions](build.md#native-ci-and-release-packages) describe usage.

Container checks cover package metadata, dependencies, installed version, smoke
loading, upgrades and removal. They do not establish physical desktop, Plasma,
input-method or accessibility compatibility. Use the [platform validation
procedure](platform-validation.md) for those checks.

## Dependency floors

[CMakeLists.txt](../CMakeLists.txt) defines the build API floors: CMake 3.28,
ECM 6.0, Qt 6.6, KF6 Kirigami 6.0 and KF6 SyntaxHighlighting 6.0. The native
recipes declare their distro build and runtime dependencies. Resolve linked
library requirements with the native packaging tools; declare QML imports,
styles, icon engines and Wayland plugins explicitly because dynamic loading
hides them from linked-library inspection.

### QML bytecode instead of compiled C++

[src/ui/CMakeLists.txt](../src/ui/CMakeLists.txt) passes `--only-bytecode` to
qmlcachegen. The executable embeds QML bytecode and loads it without a source
or build tree. Compiling QML bindings to C++ can introduce `Qt_6_PRIVATE_API`
symbols and require an exact Qt private-ABI dependency. Keep bytecode-only
compilation so routine distro Qt updates do not impose that private ABI pin.
The artifact inspector rejects private Qt symbols without an exact dependency.

## Developer per-base Debian tooling

`tools/build-artifact.sh` and `tools/inspect-artifact.sh` build and inspect
separate Kubuntu/Tuxedo developer DEBs. These tools use
[packaging/kubuntu/Dockerfile](../packaging/kubuntu/Dockerfile) and
[packaging/tuxedo/Dockerfile](../packaging/tuxedo/Dockerfile), with shared
[Debian packaging](../packaging/debian/). They use base-specific version
suffixes and do not produce the three native release assets or manifests.
The Tuxedo image obtains its Qt/KDE stack from the vendor repositories.

## Build and inspect developer artifacts


Docker or another engine named by `CONTAINER_ENGINE` is required. Nothing is
installed on the host.

```sh
tools/build-artifact.sh --target kubuntu --output "$out"
tools/inspect-artifact.sh --target kubuntu --package "$out/branch-truce_<version>+kubuntu26.04_amd64.deb" --output "$out"
tools/build-artifact.sh --target tuxedo --output "$out"
tools/inspect-artifact.sh --target tuxedo --package "$out/branch-truce_<version>+tuxedo24.04_amd64.deb" --output "$out"
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

The script copies the checkout's `CMakeLists.txt`, `cmake/`, `src/`,
`packaging/`, `assets/branding/platform/linux/` and `assets/branding/svg/`
(tracked and untracked, not ignored) into a temporary directory and
runs `dpkg-buildpackage -b` there with `--network none` and `parallel=2`. The
build path is always `/work/branch-truce-<version>`, and `SOURCE_DATE_EPOCH`
is the author time of the last commit that changed those paths. The output directory gets the `.deb`, its
`.buildinfo` and a `.manifest`. The manifest records the artifact's SHA-256,
the base image, the build image ID and a SHA-256 of its layer list, a
SHA-256 of the source snapshot, `HEAD`,
the number of changed source paths and resolved toolchain versions.

`inspect-artifact.sh` starts a throwaway container from the `-target` image,
which needs network access for the package lists, and checks:

1. Metadata and contents: the version suffix, `amd64`, exactly the eight
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
5. `desktop-file-validate` and `appstreamcli validate --no-net`. Each
   installed application icon equals the branding pack's file byte for byte,
   each PNG has its directory's pixel size, and the `hicolor` theme's
   `index.theme` declares every icon directory at that size, so a theme lookup
   finds the icon in the installed files alone.
6. The installed `branch-truce --version` and `--smoke-test` on a disposable
   merge conflict, offscreen with software rendering. This loads the compiled
   QML, Qt Quick Controls and Kirigami from the installed closure only.
7. The installed application resolves the Breeze icon `document-save`. The
   inspection builds `tests/fixtures/AppearanceProbe.cpp` in the `-build` image
   of the base, preloads it into `branch-truce --smoke-test` with no desktop
   named and reads the `documentSave` field of its record. A null icon fails,
   which is what a closure without `qt6-svg-plugins` gives.
8. `apt-get remove` leaves none of the program files.
