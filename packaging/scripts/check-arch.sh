#!/usr/bin/env bash
# Run as root ONLY in a disposable Arch target container, with no source/build
# mount. Supply this script, package-arch.sh, branding exports and packages only.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/package-arch.sh"

die() { echo "error: $*" >&2; exit 1; }
ok() { echo "ok: $*"; }
pkg_values() {
    bsdtar -xOf "$1" .PKGINFO | awk -v key="$2" 'index($0, key " = ") == 1 {print substr($0, length(key) + 4)}'
}
pkg_value() {
    local value
    value="$(pkg_values "$1" "$2")" || return 1
    [ -n "$value" ] && [[ "$value" != *$'\n'* ]] || return 1
    printf '%s\n' "$value"
}
inspect_identity() {
    local file="$1" kind="$2" got
    [[ "$file" = *.pkg.tar.zst ]] || die "$kind format must be .pkg.tar.zst"
    got="$(pkg_value "$file" pkgname)" && [ "$got" = branch-truce ] || die "$kind name is not branch-truce"
    got="$(pkg_value "$file" arch)" && [ "$got" = x86_64 ] || die "$kind architecture is not x86_64"
    pkg_value "$file" pkgver >/dev/null || die "$kind version is missing or ambiguous"
}

check_arch_main() {
    local version='' package='' previous='' native field got files deps dep path rel
    local work runtime_home repo smoke output
    while [ "$#" -gt 0 ]; do
        [ "$#" -ge 2 ] || die 'missing option value'
        case "$1" in
            --version) version="$2" ;;
            --package) package="$2" ;;
            --previous) previous="$2" ;;
            *) die "unknown option $1" ;;
        esac
        shift 2
    done
    [ -n "$version" ] && [ -n "$package" ] || die "usage: $0 --version <application-semver> --package <file> [--previous <file>]"
    native="$(arch_native_version "$version")" || return 2
    [ "$(id -u)" -eq 0 ] || die 'checker requires root in a disposable target container'
    package="$(realpath "$package")"
    [ -z "$previous" ] || previous="$(realpath "$previous")"
    if pacman -Q branch-truce >/dev/null 2>&1; then die 'branch-truce is already installed; use a clean target'; fi

    inspect_identity "$package" candidate
    got="$(pkg_value "$package" pkgver)"
    [ "$got" = "$native-1" ] || die "candidate version $got does not match $native-1"
    local previous_version=''
    if [ -n "$previous" ]; then
        inspect_identity "$previous" previous
        previous_version="$(pkg_value "$previous" pkgver)"
        [ "$(vercmp "$previous_version" "$native-1")" -lt 0 ] || die "previous version $previous_version is not older than $native-1"
        ok "previous $previous_version is older than $native-1"
    fi

    local app_id=com.canpicasoft.branch-truce
    local binary=/usr/bin/branch-truce
    local desktop="/usr/share/applications/$app_id.desktop"
    local metainfo="/usr/share/metainfo/$app_id.metainfo.xml"
    local license=/usr/share/licenses/branch-truce/copyright
    local icon_root=/usr/share/icons/hicolor
    local branding
    branding="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/assets/branding/platform/linux/hicolor"
    local -a icons=("scalable/apps/$app_id.svg" "16x16/apps/$app_id.png" \
        "22x22/apps/$app_id.png" "24x24/apps/$app_id.png" "32x32/apps/$app_id.png")
    local -a expected=("$binary" "$desktop" "$metainfo" "$license")
    for rel in "${icons[@]}"; do expected+=("$icon_root/$rel"); done
    files="$(bsdtar -tf "$package" | sed 's|^\./||' | sed '/\/$/d' | sort)"
    for path in "${expected[@]}"; do
        grep -qxF "${path#/}" <<<"$files" || die "package missing $path"
    done
    # Exact payload prevents accidental config files, hooks, sources or debug packages.
    local expected_files
    expected_files="$(printf '%s\n' "${expected[@]#/}" .PKGINFO .BUILDINFO .MTREE | sort)"
    [ "$files" = "$expected_files" ] || die "unexpected package payload: $files"
    for field in GPL-3.0-or-later CC0-1.0; do
        pkg_values "$package" license | grep -qxF "$field" || die "missing license $field"
    done
    deps="$(pkg_values "$package" depend)"
    for dep in gcc-libs glibc git qt6-base qt6-declarative kirigami syntax-highlighting \
        qqc2-desktop-style breeze breeze-icons hicolor-icon-theme qt6-svg qt6-wayland; do
        sed 's/[<>=].*//' <<<"$deps" | grep -qxF "$dep" || die "missing runtime dependency $dep"
    done
    ok "candidate metadata, architecture, exact payload, licenses and runtime dependencies"

    work="$(mktemp -d /tmp/branch-truce-arch-check.XXXXXX)"
    # Capture the path while the function's local variable is still in scope.
    # shellcheck disable=SC2064
    trap "$(printf 'rm -rf -- %q' "$work")" EXIT
    runtime_home="$work/home"; repo="$work/repo"
    mkdir -p "$runtime_home/.config" "$runtime_home/.local/share/branch-truce/recovery" "$runtime_home/.cache" "$runtime_home/runtime" "$repo"
    chmod 700 "$runtime_home/runtime"
    printf '[General]\nwrapLines=true\n' > "$runtime_home/.config/branch-trucerc"
    head -c 256 /dev/urandom > "$runtime_home/.local/share/branch-truce/recovery/session.journal"
    (cd "$runtime_home" && sha256sum .config/branch-trucerc .local/share/branch-truce/recovery/session.journal) > "$work/user-files.sha256"
    prepare_conflict() (
        cd "$repo"
        export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null
        git init --quiet --initial-branch=main
        git config user.name 'Package Smoke'; git config user.email smoke@example.invalid
        git config core.autocrlf false
        printf 'base\n' > tracked.txt
        git add tracked.txt; git commit --quiet -m base
        git checkout --quiet -b side
        printf 'side\n' > tracked.txt; git commit --quiet -am side
        git checkout --quiet main
        printf 'main\n' > tracked.txt; git commit --quiet -am main
        if git merge --no-edit side > "$work/merge.log" 2>&1; then die 'fixture did not conflict'; fi
        [ "$(git diff --name-only --diff-filter=U)" = tracked.txt ] || die 'fixture has no unmerged tracked.txt'
    )

    local -a clean_env=(env -i PATH=/usr/bin:/bin LC_ALL=C.UTF-8 HOME="$runtime_home" \
        XDG_DATA_HOME="$runtime_home/.local/share" XDG_CONFIG_HOME="$runtime_home/.config" \
        XDG_CACHE_HOME="$runtime_home/.cache" XDG_RUNTIME_DIR="$runtime_home/runtime" \
        GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null GIT_TERMINAL_PROMPT=0 \
        QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software)

    install_file() {
        local file="$1" dependency
        local -a required=()
        # Pacman -U does not fetch missing dependencies from sync repositories.
        # Resolve them with repository signatures enabled before the local install.
        while IFS= read -r dependency; do required+=("${dependency%%[<>=]*}"); done < <(pkg_values "$file" depend)
        pacman -S --needed --noconfirm -- "${required[@]}"
        # Default LocalFileSigLevel=Optional accepts a locally unsigned package.
        pacman -U --noconfirm -- "$file"
    }
    check_installed() {
        [ "$(pacman -Q branch-truce)" = "branch-truce $native-1" ] || die 'installed package version mismatch'
        for path in "${expected[@]}"; do [ -f "$path" ] || die "installed file missing $path"; done
        for rel in "${icons[@]}"; do cmp -s "$branding/$rel" "$icon_root/$rel" || die "icon bytes differ for $rel"; done
        cmp -s <(bsdtar -xOf "$package" "${license#/}") "$license" || die 'installed license bytes differ'
        # Check every package file against pacman's recorded mtree attributes.
        pacman -Qkk branch-truce > "$work/integrity.log" 2>&1 || { cat "$work/integrity.log"; die 'installed integrity failure'; }
        cat "$work/integrity.log"
        local -a declared_deps=()
        mapfile -t declared_deps < <(pkg_values "$package" depend)
        pacman -T "${declared_deps[@]}" > "$work/missing-deps" || die "unresolved dependencies: $(cat "$work/missing-deps")"
        ldd "$binary" > "$work/ldd.log"
        if grep -q 'not found' "$work/ldd.log"; then cat "$work/ldd.log"; die 'unresolved shared libraries'; fi
        objdump -T "$binary" > "$work/symbols"
        if grep -q Qt_6_PRIVATE_API "$work/symbols"; then die 'private Qt ABI dependency'; fi
        readelf -hW "$binary" | grep -q 'Type:.*DYN' || die 'binary is not PIE'
        readelf -lW "$binary" | grep -q GNU_RELRO || die 'binary lacks RELRO'
        readelf -dW "$binary" | grep -q BIND_NOW || die 'binary lacks immediate binding'
        readelf -lW "$binary" | grep 'GNU_STACK' | grep -q 'RW ' || die 'executable or missing stack policy'
        for path in QtQml/Models QtQml/WorkerScript QtQuick QtQuick/Controls QtQuick/Layouts \
            QtQuick/Shapes QtQuick/Templates QtQuick/Window org/kde/kirigami org/kde/desktop; do
            [ -f "/usr/lib/qt6/qml/$path/qmldir" ] || die "missing QML module $path"
        done
        [ -f /usr/lib/qt6/plugins/iconengines/libqsvgicon.so ] || die 'missing SVG icon engine'
        [ -f /usr/lib/qt6/plugins/wayland-shell-integration/libxdg-shell.so ] || die 'missing Wayland xdg-shell integration'
        desktop-file-validate "$desktop"
        appstreamcli validate --no-net "$metainfo"
        output="$("${clean_env[@]}" "$binary" --version)"
        [ "$output" = "Branch Truce $version" ] || die "installed --version reports '$output'"
        smoke="$("${clean_env[@]}" "$binary" --smoke-test "$repo")"
        [[ "$smoke" = *'"state":"ready"'* && "$smoke" = *'"operation":"Merge"'* \
            && "$smoke" = *'"path":"tracked.txt"'* ]] || die "unexpected smoke-test record: $smoke"
        ok "installed $native-1: icon bytes, integrity, dependencies, hardening, QML/plugins, --version and conflict --smoke-test"
    }
    remove_and_check() {
        pacman -Rn --noconfirm branch-truce
        if pacman -Q branch-truce >/dev/null 2>&1; then die 'package remains installed'; fi
        for path in "${expected[@]}"; do
            [ ! -e "$path" ] && [ ! -e "$path.pacsave" ] && [ ! -e "$path.pacnew" ] || die "removal left $path"
        done
        [ ! -d /usr/share/licenses/branch-truce ] || die 'license directory remains'
        (cd "$runtime_home" && sha256sum --quiet -c "$work/user-files.sha256") || die 'removal changed user settings or recovery files'
        ok 'complete package removal without pacsave/pacnew files; user settings and recovery preserved'
    }

    echo "== clean install $native-1"
    install_file "$package"
    prepare_conflict
    pacman -Q git qt6-base qt6-declarative kirigami syntax-highlighting pacman
    check_installed
    remove_and_check
    if [ -n "$previous" ]; then
        echo "== upgrade $previous_version to $native-1"
        install_file "$previous"
        [ "$(pacman -Q branch-truce)" = "branch-truce $previous_version" ] || die 'predecessor did not install'
        install_file "$package"
        check_installed
        remove_and_check
    fi
    ok "all Arch package checks passed for $(basename "$package")"
}

if [[ "${BASH_SOURCE[0]}" = "$0" ]]; then
    (check_arch_main "$@")
fi
