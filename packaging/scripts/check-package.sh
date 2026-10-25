#!/usr/bin/env bash
# Inspects a branch-truce DEB package, then installs, exercises, upgrades and
# removes it.
#
#   check-package.sh --version <version> --package <file> [--previous <file>]
#
# Run as root on a disposable target container with the distribution's
# repositories configured (Kubuntu 26.04 or Tuxedo OS 24.04). Never run on a
# real desktop.

set -euo pipefail

usage() {
    echo "usage: $0 --version <version> --package <file> [--previous <file>]" >&2
    exit 2
}

version=""
package=""
previous=""
while [ $# -gt 0 ]; do
    case "$1" in
        --version) version="${2-}"; shift 2 || usage ;;
        --package) package="${2-}"; shift 2 || usage ;;
        --previous) previous="${2-}"; shift 2 || usage ;;
        *) usage ;;
    esac
done
[ -n "$version" ] && [ -n "$package" ] || usage

native_version="$("$(dirname "${BASH_SOURCE[0]}")/native-version.sh" "$version")" || exit 2

package="$(realpath "$package")"
[ -z "$previous" ] || previous="$(realpath "$previous")"

case "$package" in
    *.deb) ;;
    *) echo "error: $package is not a .deb" >&2; exit 2 ;;
esac

app_id="local.branch_truce.BranchTruce"
binary_file="/usr/bin/branch-truce"
desktop_file="/usr/share/applications/$app_id.desktop"
metainfo_file="/usr/share/metainfo/$app_id.metainfo.xml"
icon_file="/usr/share/icons/hicolor/scalable/apps/$app_id.svg"
license_file="/usr/share/doc/branch-truce/copyright"
expected_arch="amd64"

expected_deps="git hicolor-icon-theme kde-style-breeze kf6-breeze-icon-theme
    libqt6core6t64 libqt6gui6 libqt6network6 libqt6qml6
    libqt6quick6 libqt6widgets6
    qml6-module-org-kde-desktop qml6-module-org-kde-kirigami
    qml6-module-qtqml-workerscript qml6-module-qtquick
    qml6-module-qtquick-controls qml6-module-qtquick-layouts
    qml6-module-qtquick-templates qml6-module-qtquick-window
    qt6-wayland"

expected_files="$binary_file
$desktop_file
$metainfo_file
$icon_file
$license_file"

failures=0
pass() { echo "ok: $*"; }
fail() { echo "FAIL: $*" >&2; failures=$((failures + 1)); }
expect() {
    local ok="$1" bad="$2"
    shift 2
    if "$@"; then pass "$ok"; else fail "$bad"; fi
}
step() { echo; echo "== $*"; }

pkg_field() { dpkg-deb -f "$1" "$2"; }
pkg_files() {
    dpkg-deb --fsys-tarfile "$1" | tar -t | sed -n 's|^\./|/|p' | grep -v '/$' | sort
}
pkg_deps() {
    dpkg-deb -f "$1" Depends | tr ',' '\n' | sed 's/^ *//; s/ .*//'
}
pkg_has_scripts() {
    dpkg-deb --ctrl-tarfile "$1" | tar -t \
        | grep -qE '(^|/)(preinst|postinst|prerm|postrm|config|triggers)$'
}
installed_version() {
    dpkg-query -W -f '${Status} ${Version}\n' branch-truce 2>/dev/null \
        | awk '$3 == "installed" {print $4}'
}
install_file() {
    DEBIAN_FRONTEND=noninteractive apt-get install -y -q "$1"
}
upgrade_file() {
    DEBIAN_FRONTEND=noninteractive apt-get install -y -q "$1"
}
remove_package() {
    DEBIAN_FRONTEND=noninteractive apt-get "${1:-remove}" -y -q branch-truce
}
target_versions() {
    dpkg-query -W libqt6core6t64 libkirigami6 git | sed 's/^/target: /'
}
verify_installed() {
    dpkg --verify branch-truce
}

inspect() {
    local file="$1" want="$2" got dep
    step "inspect $(basename "$file")"
    got="$(pkg_field "$file" Package)"
    expect "name branch-truce" "name is $got" [ "$got" = branch-truce ]
    got="$(pkg_field "$file" Version)"
    expect "version $want" "version is $got, expected $want" [ "$got" = "$want" ]
    got="$(pkg_field "$file" Architecture)"
    expect "architecture $expected_arch" "architecture is $got" [ "$got" = "$expected_arch" ]

    local files deps
    files="$(pkg_files "$file")"
    while IFS= read -r path; do
        expect "contains $path" "missing $path" grep -qxF "$path" <<<"$files"
    done <<<"$expected_files"
    if grep -v '^/usr/' <<<"$files" | grep -q .; then
        fail "files outside /usr: $(grep -v '^/usr/' <<<"$files" | tr '\n' ' ')"
    else
        pass "every file is under /usr"
    fi

    deps="$(pkg_deps "$file")"
    for dep in $expected_deps; do
        expect "depends on $dep" "does not depend on $dep" grep -qxF "$dep" <<<"$deps"
    done

    if pkg_has_scripts "$file"; then
        fail "has maintainer scripts; check what they do to user data"
    else
        pass "no maintainer scripts"
    fi
}

check_installed() {
    local want="$1" semver_want="$2" got ldd_out ldd_status=0 out
    got="$(installed_version)"
    expect "installed version $want" "installed version is '$got', expected $want" \
        [ "$got" = "$want" ]

    for path in $expected_files; do
        expect "installed $path" "missing installed $path" [ -f "$path" ]
    done

    ldd_out="$(ldd "$binary_file" 2>&1)" || ldd_status=$?
    if [ "$ldd_status" -ne 0 ]; then
        fail "ldd failed on $binary_file (exit $ldd_status): $(tr '\n' ' ' <<<"$ldd_out")"
    elif grep -q 'not found' <<<"$ldd_out"; then
        fail "binary libraries missing: $(grep 'not found' <<<"$ldd_out" | tr '\n' ' ')"
    else
        pass "binary libraries resolve"
    fi

    if out="$(verify_installed 2>&1)" && [ -z "$out" ]; then
        pass "package verify is clean"
    else
        fail "package verify: $out"
    fi

    local cli_ver
    cli_ver="$(env -i PATH=/usr/bin:/bin LC_ALL=C.UTF-8 QT_QPA_PLATFORM=offscreen \
        "$binary_file" --version | awk '{print $NF}')"
    expect "binary --version reports $semver_want" \
        "binary --version reported '$cli_ver', expected '$semver_want'" \
        [ "$cli_ver" = "$semver_want" ]
}

check_loads() {
    local work repo home smoke record
    work="$(mktemp -d /tmp/bt-pkg-smoke.XXXXXX)"
    repo="$work/repo"
    home="$work/home"
    mkdir -p "$home/data" "$home/config" "$home/cache" "$home/runtime" "$repo"
    chmod 700 "$home/runtime"
    (
        cd "$repo"
        git init --quiet --initial-branch=main
        git config user.name 'Package Smoke'
        git config user.email 'smoke@example.invalid'
        git config core.autocrlf false
        printf 'base\n' > tracked.txt
        git add tracked.txt && git commit --quiet -m base
        git checkout --quiet -b side
        printf 'side\n' > tracked.txt && git commit --quiet -am side
        git checkout --quiet main
        printf 'main\n' > tracked.txt && git commit --quiet -am main
        git merge --no-edit side >/dev/null 2>&1 || true
    )
    smoke="$(env -i PATH=/usr/bin:/bin HOME="$home" LC_ALL=C.UTF-8 \
        XDG_DATA_HOME="$home/data" XDG_CONFIG_HOME="$home/config" \
        XDG_CACHE_HOME="$home/cache" XDG_RUNTIME_DIR="$home/runtime" \
        GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null GIT_TERMINAL_PROMPT=0 \
        QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
        "$binary_file" --smoke-test "$repo")" || {
        rm -rf "$work"
        fail "installed branch-truce --smoke-test failed"
        return
    }
    rm -rf "$work"
    record="$(printf '%s\n' "$smoke" | sed -n 's/^BRANCH_TRUCE_SESSION=//p')"
    if [[ "$record" == *'"state":"ready"'* && "$record" == *'"operation":"Merge"'* \
        && "$record" == *'"path":"tracked.txt"'* ]]; then
        pass "installed branch-truce --smoke-test loaded QML/Kirigami and opened the merge session"
    else
        fail "smoke-test record unexpected: $smoke"
    fi
}

check_removed() {
    expect "branch-truce is not installed" "branch-truce is still installed" [ -z "$(installed_version)" ]
    for path in "$binary_file" "$desktop_file" "$metainfo_file" "$icon_file" "$license_file"; do
        expect "$path is gone" "$path is left behind" [ ! -e "$path" ]
    done
    if (cd "$fixture_home" && sha256sum --quiet -c "$fixture_sums"); then
        pass "user settings and recovery files are unchanged"
    else
        fail "user settings or recovery files changed"
    fi
}

fixture_home="$(mktemp -d /home/branch-truce-check.XXXXXX)"
fixture_sums="$(mktemp)"
trap 'rm -rf "$fixture_home" "$fixture_sums"' EXIT
mkdir -p "$fixture_home/.config" "$fixture_home/.local/share/branch-truce/recovery"
printf '[General]\nwrapLines=true\n' > "$fixture_home/.config/branch-trucerc"
head -c 256 /dev/urandom > "$fixture_home/.local/share/branch-truce/recovery/session.journal"
(cd "$fixture_home" && find . -type f -exec sha256sum {} +) > "$fixture_sums"

[ -z "$(installed_version)" ] || { echo "error: branch-truce is already installed; use a clean system" >&2; exit 1; }

inspect "$package" "$native_version"

check_previous() {
    local field got
    step "check the previous package $(basename "$previous")"
    case "$previous" in
        *.deb) ;;
        *) echo "error: the previous package $previous is not a .deb" >&2; exit 1 ;;
    esac
    for field in Package Architecture Version; do
        got="$(pkg_field "$previous" "$field" 2>&1)" && [ -n "$got" ] \
            || { echo "error: cannot read the $field of $previous: $got" >&2; exit 1; }
        case "$field" in
            Package) [ "$got" = branch-truce ] \
                || { echo "error: $previous is the package $got, not branch-truce" >&2; exit 1; } ;;
            Architecture) [ "$got" = "$expected_arch" ] \
                || { echo "error: $previous is for $got, not $expected_arch" >&2; exit 1; } ;;
            Version) previous_version="$got" ;;
        esac
    done
    dpkg --compare-versions "$previous_version" lt "$native_version" \
        || { echo "error: previous version $previous_version is not older than $native_version" >&2; exit 1; }
    pass "previous package is branch-truce $previous_version for $expected_arch, older than $native_version"
}
previous_version=""
[ -z "$previous" ] || check_previous

step "clean install of $native_version"
install_file "$package"
target_versions
check_installed "$native_version" "$version"
check_loads

step "remove $native_version"
remove_package
check_removed

if [ -n "$previous" ]; then
    step "install $previous_version, then upgrade to $native_version"
    install_file "$previous"
    got="$(installed_version)"
    expect "installed version $previous_version" \
        "installed version is '$got', expected $previous_version" \
        [ "$got" = "$previous_version" ]
    upgrade_file "$package"
    check_installed "$native_version" "$version"
    check_loads

    step "remove $native_version after the upgrade (purge)"
    remove_package purge
    check_removed
fi

echo
if [ "$failures" -gt 0 ]; then
    echo "$failures check(s) failed for $(basename "$package")" >&2
    exit 1
fi
echo "all checks passed for $(basename "$package")"
