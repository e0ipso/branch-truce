# RPM package manager adapter for packaging/scripts/check-package.sh and
# packaging/scripts/package-identity.sh. Sourcing defines the pkg_* functions
# that every adapter provides (see check-package.sh); it runs nothing.

pkg_rpm_fields() { rpm -qp --qf '%{NAME}|%{EPOCHNUM}|%{VERSION}|%{RELEASE}|%{ARCH}|%{LICENSE}\n' "$1"; }
# pkg_rpm_evr EPOCH VERSION RELEASE: the native version as the identity and the
# installed version both give it, with the epoch only when it is not 0.
pkg_rpm_evr() {
    if [ "$1" = 0 ]; then printf '%s-%s\n' "$2" "$3"; else printf '%s:%s-%s\n' "$1" "$2" "$3"; fi
}
pkg_identity() {
    local name epoch version release architecture license
    IFS='|' read -r name epoch version release architecture license < <(pkg_rpm_fields "$1") || return 1
    printf '%s|%s|%s\n' "$name" "$(pkg_rpm_evr "$epoch" "$version" "$release")" "$architecture"
}
pkg_check_metadata() {
    local license
    license=$(pkg_rpm_fields "$1" | cut -d'|' -f6) || return 1
    [ "$license" = 'GPL-3.0-or-later AND CC0-1.0' ] \
        || { echo "license is '$license', expected 'GPL-3.0-or-later AND CC0-1.0'" >&2; return 1; }
}
# Build-id links belong to the debuginfo machinery, not to the application.
pkg_files() {
    rpm -qp --qf '[%{FILENAMES} %{FILEMODES:perms}\n]' "$1" \
        | awk '$2 !~ /^d/ && $1 !~ /^\/usr\/lib\/\.build-id\// { print $1 }' | LC_ALL=C sort
}
pkg_extra_files() { echo /usr/share/licenses/branch-truce/LICENSE; }
pkg_has_scripts() {
    local scripts configs
    scripts=$(rpm -qp --scripts --triggers "$1") && configs=$(rpm -qp --configfiles "$1") || return 2
    [ -n "$scripts$configs" ]
}
pkg_depends() { rpm -qpR "$1" | sed 's/ .*//' | LC_ALL=C sort -u; }
# pkg_recipe_depends ROOT: the names the spec's Requires lines give.
pkg_recipe_depends() {
    awk '/^Requires:/ { print $2 }' "$1/packaging/rpm/branch-truce.spec" | sed 's/%{?_isa}/(x86-64)/' | LC_ALL=C sort -u
}
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
    local epoch version release
    rpm -q branch-truce >/dev/null 2>&1 || return 0
    IFS='|' read -r epoch version release < <(rpm -q --qf '%{EPOCHNUM}|%{VERSION}|%{RELEASE}\n' branch-truce) || return 1
    pkg_rpm_evr "$epoch" "$version" "$release"
}
pkg_verify_installed() { rpm -V branch-truce; }
pkg_runtime_versions() { rpm -q qt6-qtbase qt6-qtdeclarative kf6-kirigami kf6-syntax-highlighting git rpm; }
pkg_qt_root() { echo /usr/lib64/qt6; }
pkg_leftover_suffixes() { echo .rpmsave .rpmnew; }
