# shellcheck shell=bash
# RPM package manager adapter for packaging/scripts/check-package.sh and
# packaging/scripts/package-identity.sh. Sourcing defines the pkg_* functions
# of the adapter contract in pkg-adapters.md and runs nothing; names that start
# with _pkg_ are private to this file.

_pkg_rpm_fields() { rpm -qp --qf '%{NAME}|%{EPOCHNUM}|%{VERSION}|%{RELEASE}|%{ARCH}|%{LICENSE}\n' "$1"; }
# _pkg_rpm_evr EPOCH VERSION RELEASE: the native version as the identity and the
# installed version both give it, with the epoch only when it is not 0.
_pkg_rpm_evr() {
    if [ "$1" = 0 ]; then printf '%s-%s\n' "$2" "$3"; else printf '%s:%s-%s\n' "$1" "$2" "$3"; fi
}
pkg_identity() {
    local name epoch version release architecture license fields
    fields=$(_pkg_rpm_fields "$1") || return 1
    IFS='|' read -r name epoch version release architecture license <<<"$fields" || return 1
    printf '%s|%s|%s\n' "$name" "$(_pkg_rpm_evr "$epoch" "$version" "$release")" "$architecture"
}
pkg_check_metadata() ( set -o pipefail;
    local license
    license=$(_pkg_rpm_fields "$1" | cut -d'|' -f6) || return 1
    [ "$license" = 'GPL-3.0-or-later AND CC0-1.0' ] \
        || { echo "license is '$license', expected 'GPL-3.0-or-later AND CC0-1.0'" >&2; return 1; }
)
# Build-id links belong to the debuginfo machinery, not to the application.
pkg_files() ( set -o pipefail;
    rpm -qp --qf '[%{FILENAMES} %{FILEMODES:perms}\n]' "$1" \
        | awk '$2 !~ /^d/ && $1 !~ /^\/usr\/lib\/\.build-id\// { print $1 }' | LC_ALL=C sort
)
pkg_extra_files() { echo /usr/share/licenses/branch-truce/LICENSE; }
pkg_has_scripts() {
    local scripts configs
    scripts=$(rpm -qp --scripts --triggers "$1") && configs=$(rpm -qp --configfiles "$1") || return 2
    [ -n "$scripts$configs" ]
}
pkg_depends() ( set -o pipefail; rpm -qpR "$1" | sed 's/ .*//' | LC_ALL=C sort -u; )
# pkg_recipe_depends ROOT: the names the spec's Requires lines give.
pkg_recipe_depends() ( set -o pipefail;
    awk '/^Requires:/ { print $2 }' "$1/packaging/rpm/branch-truce.spec" | sed 's/%{?_isa}/(x86-64)/' | LC_ALL=C sort -u
)
pkg_older() {
    [ "$(OLD="$1" NEW="$2" rpm --eval '%{lua:print(rpm.vercmp(os.getenv("OLD"), os.getenv("NEW")))}')" = -1 ]
}
pkg_target_ready() {
    [ "$(rpm --eval '%{?fedora}')" = 44 ] && [ "$(rpm --eval '%{_arch}')" = x86_64 ] \
        || { echo 'error: use a Fedora 44 x86_64 target' >&2; return 1; }
}
pkg_refresh() { dnf makecache --refresh -y; }
# Only local-file signature checking is excepted for the unsigned candidates;
# repository gpgcheck stays on. Weak dependencies are off, so the installed
# runtime closure is the declared one.
pkg_install() { dnf -y --setopt=localpkg_gpgcheck=False --setopt=install_weak_deps=False install "$1"; }
pkg_remove() { dnf -y remove --no-autoremove branch-truce; }
pkg_installed_version() {
    local packages fields epoch version release
    packages=$(rpm -qa --qf '%{NAME}|%{EPOCHNUM}|%{VERSION}|%{RELEASE}\n') || return 2
    fields=$(awk -F '|' '$1 == "branch-truce" { if (NF != 4) bad=1; fields=$0; found++ }
        END { if (bad || found > 1) exit 2; if (!found) exit 1; print fields }' <<<"$packages") || return $?
    IFS='|' read -r _ epoch version release <<<"$fields"
    [ -n "$epoch" ] && [ -n "$version" ] && [ -n "$release" ] || return 2
    _pkg_rpm_evr "$epoch" "$version" "$release"
}
pkg_verify_installed() { rpm -V branch-truce; }
pkg_runtime_versions() { rpm -q qt6-qtbase qt6-qtdeclarative kf6-kirigami kf6-syntax-highlighting git rpm; }
pkg_qt_root() { echo /usr/lib64/qt6; }
pkg_leftover_suffixes() { echo .rpmsave .rpmnew; }
