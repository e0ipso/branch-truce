#!/usr/bin/env bash
# Build inside the Arch build/test image. Prints the absolute package path last.
# Root callers delegate makepkg to the image's unprivileged builder account.
set -euo pipefail

# Arch sorts a trailing alphabetic component before the corresponding release.
# Keep the same supported SemVer subset as the other native release recipes.
arch_native_version() {
    local pattern='^((0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*))(-([A-Za-z]+)(\.([1-9][0-9]*))?)?$'
    if [ "$#" -ne 1 ] || ! [[ "$1" =~ $pattern ]]; then
        echo 'error: unsupported Arch version; use MAJOR.MINOR.PATCH[-<letters>[.<positive number>]]' >&2
        return 2
    fi
    printf '%s%s%s\n' "${BASH_REMATCH[1]}" "${BASH_REMATCH[6]}" "${BASH_REMATCH[8]}"
}

arch_package_main() {
    local version='' output='' native root work tree archive checksum package
    while [ "$#" -gt 0 ]; do
        [ "$#" -ge 2 ] || { echo 'error: missing option value' >&2; return 2; }
        case "$1" in
            --version) version="$2" ;;
            --output) output="$2" ;;
            *) echo "error: unknown option $1" >&2; return 2 ;;
        esac
        shift 2
    done
    [ -n "$version" ] && [ -n "$output" ] || {
        echo "usage: $0 --version <application-semver> --output <dir>" >&2; return 2;
    }
    native="$(arch_native_version "$version")" || return 2
    [ "$(uname -m)" = x86_64 ] || { echo 'error: Arch packages require x86_64' >&2; return 2; }
    root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
    mkdir -p "$output"
    output="$(realpath "$output")"
    work="$(mktemp -d /tmp/branch-truce-arch.XXXXXX)"
    # This helper executes in a subshell, so cleanup cannot change a caller's trap.
    # Capture the quoted path now: this local is out of scope at subshell exit.
    # shellcheck disable=SC2064
    trap "$(printf 'rm -rf -- %q' "$work")" EXIT
    tree="$work/branch-truce"
    mkdir -p "$tree/packaging" "$tree/assets/branding/platform"
    cp -a "$root/CMakeLists.txt" "$root/cmake" "$root/src" "$tree/"
    cp -a "$root/packaging/CMakeLists.txt" \
        "$root/packaging/com.canpicasoft.branch-truce.desktop" \
        "$root/packaging/com.canpicasoft.branch-truce.metainfo.xml" "$tree/packaging/"
    cp -a "$root/packaging/debian/copyright" "$tree/packaging/copyright"
    cp -a "$root/assets/branding/platform/linux" "$tree/assets/branding/platform/"
    cp -a "$root/assets/branding/svg" "$tree/assets/branding/"
    archive="$work/branch-truce.tar.gz"
    tar -czf "$archive" -C "$work" branch-truce
    rm -rf "$tree"
    checksum="$(sha256sum "$archive")"
    cp "$root/packaging/arch/PKGBUILD" "$work/PKGBUILD"
    printf '\nsha256sums=(%q)\n' "${checksum%% *}" >> "$work/PKGBUILD"
    export BRANCH_TRUCE_VERSION="$version" BRANCH_TRUCE_ARCH_PKGVER="$native"
    export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
    source "$root/packaging/scripts/compiler-cache.sh"
    cache_prepare
    if [ "$(id -u)" -eq 0 ]; then
        id builder >/dev/null 2>&1 || { echo 'error: the image must provide an unprivileged builder user' >&2; return 2; }
        if [ -n "${BRANCH_TRUCE_COMPILER_CACHE_DIR:-}" ]; then
            # Probe as makepkg's compiler user, not just the container root.
            if ! runuser -u builder -- bash -euc '
                id
                cache_builder_probe="$(mktemp "$BRANCH_TRUCE_COMPILER_CACHE_DIR/.builder-write.XXXXXXXX")"
                rm -f "$cache_builder_probe"
            '; then
                echo 'compiler cache unavailable to builder; compiling directly' >&2
                export BRANCH_TRUCE_COMPILER_CACHE_DIR=''
            fi
        fi
    fi
    cache_begin native-arch-None "$work"
    local build_status=0
    if [ "$(id -u)" -eq 0 ]; then
        chown -R builder:builder "$work"
        # The stats log is written by makepkg's actual compiler user.
        [ -z "$cache_report" ] || chown builder:builder "$cache_report"*
        (cd "$work" && runuser -u builder -- makepkg --noconfirm --clean) || build_status=$?
    else
        (cd "$work" && makepkg --noconfirm --clean) || build_status=$?
    fi
    cache_end
    [ "$build_status" -eq 0 ] || return "$build_status"
    package="branch-truce-$native-1-x86_64.pkg.tar.zst"
    cp "$work/$package" "$output/$package"
    printf '%s\n' "$output/$package"
}

if [[ "${BASH_SOURCE[0]}" = "$0" ]]; then
    (arch_package_main "$@")
fi
