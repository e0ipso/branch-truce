# Package manager adapters

`pkg-<family>.sh` holds the calls to one package manager: `pkg-deb.sh` (dpkg and
apt), `pkg-rpm.sh` (rpm and dnf) and `pkg-arch.sh` (bsdtar and pacman). The
family is the `family` column of [`../formats.tsv`](../formats.tsv).
[`check-package.sh`](check-package.sh) and
[`package-identity.sh`](package-identity.sh) source the adapter of the format
they check. Sourcing an adapter defines the functions below and nothing else:
no shell options, no global variables, no commands. Names that start with
`_pkg_` are private to one adapter.

Every adapter defines every function, and the functions behave the same in
every family. [`tests/packaging/PackageAdapters.sh`](../../tests/packaging/PackageAdapters.sh)
runs one contract suite against all three. A new family needs a new adapter,
a `package-<family>.sh` builder, its recipe, a `formats.tsv` row, a
`minimum-versions.tsv` column and its version functions in `../ci/baseline.sh`.

Unless a function says otherwise, it prints its answer on stdout, prints
diagnostics on stderr and returns 0 on success and non-zero on failure.

## Reading a package file

These read the package's own metadata and run anywhere the family's tools are
installed, including the release job.

| Function | Answer |
| --- | --- |
| `pkg_identity FILE` | `name\|native version\|architecture`. The native version has the epoch, as `E:`, only when it is not 0. |
| `pkg_check_metadata FILE` | Status 0 when the format's own metadata, such as its licenses, is the expected one; otherwise one line on stderr. |
| `pkg_files FILE` | The payload files, absolute paths, sorted, without directories. |
| `pkg_extra_files` | The payload files the format adds beyond `installed-files.txt`, such as the license or the Debian changelog. |
| `pkg_has_scripts FILE` | Status 0 when the package has maintainer scripts, triggers or configuration files that the package manager keeps on removal; 1 when it has none; 2 when the package cannot be read. |
| `pkg_depends FILE` | The declared dependency names, without versions, sorted and unique. |
| `pkg_recipe_depends ROOT` | The dependency names the recipe under `ROOT/packaging` declares, in the same form. |
| `pkg_older OLD NEW` | Status 0 when native version `OLD` sorts strictly before `NEW` in the family's own order. |

## Changing the target

These run as root in a disposable target container of the format's OS and
change it. `check-package.sh` calls them only after every read check passed.

| Function | Answer |
| --- | --- |
| `pkg_target_ready` | Status 0 when the container is the format's target; otherwise one line on stderr. |
| `pkg_refresh` | Brings the package lists up to date, so `pkg_install` resolves against the current repositories. On Arch, a rolling release that does not support partial upgrades, it also upgrades the installed packages. |
| `pkg_install FILE` | Installs or upgrades from a local file, with the dependencies from the target's repositories. |
| `pkg_remove` | Removes branch-truce completely, configuration included. |
| `pkg_installed_version` | The installed native version in the form `pkg_identity` gives, or nothing, with status 0, when branch-truce is not installed. |
| `pkg_verify_installed` | The package manager's integrity check of the installed files. |
| `pkg_runtime_versions` | Prints the installed Qt, KDE Frameworks and Git packages, for the report. |
| `pkg_qt_root` | The Qt 6 directory that holds `qml/` and `plugins/`. |
| `pkg_leftover_suffixes` | The suffixes of the files the package manager saves on removal, such as `.dpkg-old`. |
