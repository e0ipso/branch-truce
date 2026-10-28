# Branch Truce

Branch Truce is an offline Linux desktop app for resolving text conflicts
when a Git merge or rebase stops. It shows both sides next to an editable result,
with the common earlier version on demand, and it can run as Git's merge
tool. It is
written in C++ with Qt Quick and Kirigami for KDE Plasma. It needs no
account and no network.

The current version is 0.1.0. It has not been run on a real Kubuntu 26.04 or
Tuxedo OS desktop. The tested environments and the open gaps are in
[docs/platform-validation.md](docs/platform-validation.md).

## Install

Each OS base gets its own `.deb`, built from the same sources in a container
of that base. [docs/target-platforms.md](docs/target-platforms.md#build-and-inspect)
explains how to build and inspect them. The following install was observed
only in containers of the Kubuntu 26.04 and Tuxedo OS 24.04 bases:

```sh
sudo apt-get install --no-install-recommends ./branch-truce_0.1.0+kubuntu26.04_amd64.deb
```

Use the package built for your base. It installs `/usr/bin/branch-truce`,
the desktop entry `com.canpicasoft.branch-truce.desktop`, its AppStream
metadata and an icon. The program reads nothing from the source or build
tree.

The package depends on `qt6-wayland`, which ships the Wayland shell
integrations, so it also starts on a Wayland session other than Plasma.
Without that package the app aborts at start on Wayland ("No shell
integration named "xdg-shell" found"). See
[docs/platform-validation.md](docs/platform-validation.md#wayland-scaling-and-theme).

Release packages come from a separate path. When a push to `main` has
commits that call for a new version, CI builds both packages with
`packaging/scripts/container-check.sh`, which checks installation, smoke-test
loading, upgrade and removal in containers of each base. semantic-release
then attaches them to the GitHub release as
`branch-truce_<package-version>_amd64-ubuntu26.04.deb` and
`branch-truce_<package-version>_amd64-tuxedo24.04.deb`. The container
validation in [docs/platform-validation.md](docs/platform-validation.md) used packages from
`tools/build-artifact.sh`, not release packages.

To install from a build tree into a prefix of your choice, see
[docs/build.md](docs/build.md#install).

## Launch

Open Branch Truce from the application menu, or from a terminal:

```sh
branch-truce                     # the repository in the current directory
branch-truce /path/to/repository # or a subdirectory of it
branch-truce --version
branch-truce --help
```

The window lists every conflicted file in the merge or rebase that is in
progress, including files it cannot edit, with the reason. Starting it does
not change the worktree or the index.

The menu entry runs `branch-truce %f`. It was started with `gio launch` in
the containers. A Plasma menu or KRunner launch has not been tried.

Two actions finish a file:

- Save keeps a draft inside Branch Truce. The repository file and the index
  stay as they were.
- Mark resolved is your statement that you reviewed the result. Branch Truce
  saves it, writes it to the repository file and stages that one path. If
  writing or staging fails, the file stays outstanding and shows which step
  failed.

Neither action continues the merge or rebase. Run `git merge --continue`,
`git rebase --continue` or `git commit` yourself.

To use it from `git mergetool`, see [docs/mergetool.md](docs/mergetool.md).

## Build and test

Contributor setup, build modes, the install check and the evidence tooling
are in [docs/build.md](docs/build.md). The short version, on a provisioned
machine:

```sh
./tools/prepare-worktree.sh desktop
cmake --build build/desktop --parallel 2
ctest --test-dir build/desktop --output-on-failure

./tools/prepare-worktree.sh sanitize   # core with ASan and UBSan
cmake --build build/sanitize --parallel 2
ctest --test-dir build/sanitize --output-on-failure
```

The Node tooling needs Node 24 or later:

```sh
npm ci
npm run typecheck
npm test
npm run check:tracker-refs
```

Commit messages follow Conventional Commits. `npm ci` installs a commit-msg
hook that runs commitlint, and CI runs the same check. semantic-release
derives the version from them.

## Documentation

- [assets/branding/README.txt](assets/branding/README.txt): logo assets,
  color variants, usage notes and rebuild instructions.
- [docs/mergetool.md](docs/mergetool.md): registering and using Branch Truce
  as Git's merge tool.
- [docs/build.md](docs/build.md): provisioning, building, testing and
  installing.
- [docs/platform-validation.md](docs/platform-validation.md): what the
  installed packages did in containers, and what is untested.
- [docs/target-platforms.md](docs/target-platforms.md): OS bases, packaging
  and artifact inspection.
- [docs/mergetool-launcher.md](docs/mergetool-launcher.md): the launcher
  transport and the behaviours tested against a real `git mergetool`.
- [docs/session-state.md](docs/session-state.md),
  [docs/recovery-journal.md](docs/recovery-journal.md),
  [docs/destination-publication.md](docs/destination-publication.md),
  [docs/editor-integration.md](docs/editor-integration.md),
  [docs/text-conversion.md](docs/text-conversion.md) and
  [docs/resource-limits.md](docs/resource-limits.md): design notes on the
  session, recovery, file writes, the editor, text encodings and size
  limits.
- [PRD.md](PRD.md) and [TECH_SPEC.md](TECH_SPEC.md): what the first
  release must do, and how it is built.
