#!/usr/bin/env bash
# Builds the Arch package of a format with makepkg. Run it through
# package-release.sh, inside the format's build image:
#
#   package-arch.sh FORMAT VERSION ABSOLUTE-OUTPUT-DIR
#
# Root callers delegate makepkg to the image's unprivileged builder account.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)
source "$here/builder-common.sh"
source "$here/compiler-cache.sh"
builder_args "$0" "$@" || exit 2
format=$1 version=$2 output=$3
# The PKGBUILD's pkgver; formats.tsv adds the release revision.
native=$("$here/native-version.sh" --style "$(format_field "$format" version_style)" "$version") || exit 2
[ "$(uname -m)" = x86_64 ] || { echo 'error: Arch packages require x86_64' >&2; exit 1; }
work=$(mktemp -d /tmp/branch-truce-arch.XXXXXX)
trap 'rm -rf -- "$work"' EXIT

"$here/stage-source.sh" --from "$root" --to "$work/branch-truce"
tar -czf "$work/branch-truce.tar.gz" -C "$work" branch-truce
rm -rf -- "$work/branch-truce"
cp -- "$root/packaging/arch/PKGBUILD" "$work/PKGBUILD"
printf '\nsha256sums=(%q)\n' "$(sha256sum "$work/branch-truce.tar.gz" | cut -d' ' -f1)" >> "$work/PKGBUILD"
export BRANCH_TRUCE_VERSION="$version" BRANCH_TRUCE_ARCH_PKGVER="$native"
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"

cache_prepare
builder=()
if [ "$(id -u)" -eq 0 ]; then
    id builder >/dev/null 2>&1 || { echo 'error: the image must provide an unprivileged builder user' >&2; exit 1; }
    builder=(runuser -u builder --)
    # Probe the cache as makepkg's compiler user, not only as root.
    if [ -n "${BRANCH_TRUCE_COMPILER_CACHE_DIR:-}" ] && ! runuser -u builder -- bash -c \
        'probe=$(mktemp "$BRANCH_TRUCE_COMPILER_CACHE_DIR/.builder-write.XXXXXXXX") && rm -f "$probe"'; then
        echo 'compiler cache unavailable to builder; compiling directly' >&2
        export BRANCH_TRUCE_COMPILER_CACHE_DIR=''
    fi
fi
cache_begin native-arch-None "$work"
if [ "${#builder[@]}" -gt 0 ]; then
    chown -R builder:builder "$work"
    # makepkg's compiler user writes the statistics log.
    report=$(cache_report_path)
    [ -z "$report" ] || chown builder:builder "$report"*
fi
status=0
(cd -- "$work" && "${builder[@]}" makepkg --noconfirm --clean) || status=$?
cache_end
[ "$status" -eq 0 ] || exit "$status"

built=("$work"/*.pkg.tar.zst)
[ "${#built[@]}" -eq 1 ] && [ -f "${built[0]}" ] || { echo "error: expected one package, found: ${built[*]}" >&2; exit 1; }
builder_publish "$format" "$version" "${built[0]}" "$output"
