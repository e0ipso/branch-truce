# Debian package manager adapter for packaging/scripts/check-package.sh and
# packaging/scripts/package-identity.sh. Sourcing defines the pkg_* functions
# that every adapter provides (see check-package.sh); it runs nothing.

pkg_identity() {
    local name version architecture
    name=$(dpkg-deb -f "$1" Package) && version=$(dpkg-deb -f "$1" Version) \
        && architecture=$(dpkg-deb -f "$1" Architecture) || return 1
    printf '%s|%s|%s\n' "$name" "$version" "$architecture"
}
# The license text is the payload's copyright file; control has no license field.
pkg_check_metadata() { :; }
pkg_files() { dpkg-deb --fsys-tarfile "$1" | tar -t | sed -n 's|^\./|/|p' | sed '/\/$/d' | LC_ALL=C sort; }
pkg_extra_files() { printf '%s\n' /usr/share/doc/branch-truce/changelog.gz /usr/share/doc/branch-truce/copyright; }
# The control listing is read whole, so grep -q cannot end the pipe early.
pkg_has_scripts() {
    local members
    members=$(dpkg-deb --ctrl-tarfile "$1" | tar -t) || return 2
    grep -E '(^|/)(preinst|postinst|prerm|postrm|config|triggers|conffiles)$' <<<"$members" >/dev/null
}
pkg_depends() {
    dpkg-deb -f "$1" Depends | tr ',|' '\n\n' | sed 's/([^)]*)//; s/:any//; s/[[:space:]]//g; /^$/d' | LC_ALL=C sort -u
}
# pkg_recipe_depends ROOT: the names debian/control lists in Depends.
pkg_recipe_depends() {
    awk '/^Depends:/ { on = 1; sub("^Depends:", "") } on { print } on && $0 !~ /,[ \t]*$/ { on = 0 }' \
        "$1/packaging/debian/control" | sed 's/([^)]*)//g; s/\${[^}]*}//g' | tr ',' '\n' \
        | awk 'NF { print $1 }' | LC_ALL=C sort -u
}
pkg_older() { dpkg --compare-versions "$1" lt "$2"; }
pkg_target_ready() { command -v apt-get dpkg-query >/dev/null || { echo 'error: use a Debian-based target' >&2; return 1; }; }
pkg_refresh() { DEBIAN_FRONTEND=noninteractive apt-get update -q; }
pkg_install() { DEBIAN_FRONTEND=noninteractive apt-get install -y -q "$1"; }
pkg_remove() { DEBIAN_FRONTEND=noninteractive apt-get purge -y -q branch-truce; }
pkg_installed_version() {
    { dpkg-query -W -f '${Status} ${Version}\n' branch-truce 2>/dev/null || true; } | awk '$3 == "installed" { print $4 }'
}
pkg_verify_installed() {
    local out
    out=$(dpkg --verify branch-truce 2>&1) && [ -z "$out" ] || { printf '%s\n' "$out" >&2; return 1; }
}
pkg_runtime_versions() { dpkg-query -W libqt6core6t64 libkirigami6 libkf6syntaxhighlighting6 git; }
pkg_qt_root() { echo /usr/lib/x86_64-linux-gnu/qt6; }
pkg_leftover_suffixes() { echo .dpkg-old .dpkg-dist; }
