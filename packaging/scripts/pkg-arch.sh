# Arch Linux (pacman) adapter for packaging/scripts/check-package.sh and
# packaging/scripts/package-identity.sh. Sourcing defines the pkg_* functions
# that every adapter provides (see check-package.sh); it runs nothing.

# pkg_values FILE KEY: every value of KEY in the package's .PKGINFO.
pkg_values() {
    bsdtar -xOf "$1" .PKGINFO | awk -v key="$2" 'index($0, key " = ") == 1 { print substr($0, length(key) + 4) }'
}
# pkg_value FILE KEY: the single value of KEY; duplicates are not hidden.
pkg_value() {
    local value
    value=$(pkg_values "$1" "$2") || return 1
    [ -n "$value" ] && [[ "$value" != *$'\n'* ]] || { echo "error: .PKGINFO has no single $2" >&2; return 1; }
    printf '%s\n' "$value"
}
pkg_identity() {
    local name version architecture
    name=$(pkg_value "$1" pkgname) && version=$(pkg_value "$1" pkgver) \
        && architecture=$(pkg_value "$1" arch) || return 1
    printf '%s|%s|%s\n' "$name" "$version" "$architecture"
}
pkg_check_metadata() {
    local license licenses
    licenses=$(pkg_values "$1" license) || return 1
    for license in GPL-3.0-or-later CC0-1.0; do
        grep -qxF "$license" <<<"$licenses" || { echo "missing license $license" >&2; return 1; }
    done
}
pkg_files() {
    bsdtar -tf "$1" | sed 's|^\./||; /\/$/d' | awk '!/^\.(PKGINFO|BUILDINFO|MTREE)$/ { print "/" $0 }' | LC_ALL=C sort
}
pkg_extra_files() { echo /usr/share/licenses/branch-truce/LICENSE; }
# An install script, or a backup= configuration file that pacman saves as
# .pacsave on removal. The listing is read whole: grep -q could end the pipe
# early, and bsdtar's SIGPIPE status would then read as "no scripts".
pkg_has_scripts() {
    local members backup
    members=$(bsdtar -tf "$1") && backup=$(pkg_values "$1" backup) || return 2
    grep -xE '(\./)?\.INSTALL' <<<"$members" >/dev/null || [ -n "$backup" ]
}
pkg_depends() { pkg_values "$1" depend | sed 's/[<>=].*//' | LC_ALL=C sort -u; }
# pkg_recipe_depends ROOT: the PKGBUILD's depends array, read in a subshell.
pkg_recipe_depends() {
    (
        BRANCH_TRUCE_ARCH_PKGVER=0 BRANCH_TRUCE_VERSION=0.0.0
        # shellcheck disable=SC1091
        source "$1/packaging/arch/PKGBUILD"
        printf '%s\n' "${depends[@]}"
    ) | sed 's/[<>=].*//' | LC_ALL=C sort -u
}
pkg_older() { [ "$(vercmp "$1" "$2")" -lt 0 ]; }
pkg_target_ready() { command -v pacman vercmp >/dev/null || { echo 'error: use an Arch Linux target' >&2; return 1; }; }
# Refresh the whole rolling base; never perform a partial upgrade.
pkg_refresh() { pacman -Syu --noconfirm; }
pkg_install() {
    local dependency
    local -a required=()
    # pacman -U does not fetch missing dependencies from the sync repositories.
    # Resolve them with repository signatures on, then install the local file,
    # which the default LocalFileSigLevel=Optional accepts unsigned.
    while IFS= read -r dependency; do required+=("$dependency"); done < <(pkg_depends "$1")
    pacman -S --needed --noconfirm -- "${required[@]}" && pacman -U --noconfirm -- "$1"
}
pkg_remove() { pacman -Rn --noconfirm branch-truce; }
pkg_installed_version() { { pacman -Q branch-truce 2>/dev/null || true; } | awk '{ print $2 }'; }
pkg_verify_installed() { pacman -Qkk branch-truce; }
pkg_runtime_versions() { pacman -Q git qt6-base qt6-declarative kirigami syntax-highlighting pacman; }
pkg_qt_root() { echo /usr/lib/qt6; }
pkg_leftover_suffixes() { echo .pacsave .pacnew; }
