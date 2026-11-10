#!/usr/bin/env bash
# Builds the RPM of a format with rpmbuild. Run it through package-release.sh,
# inside the format's build image:
#
#   build-rpm.sh FORMAT VERSION ABSOLUTE-OUTPUT-DIR
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)
source "$here/builder-common.sh"
source "$here/compiler-cache.sh"
builder_args "$0" "$@" || exit 2
format=$1 version=$2 output=$3
# The spec's Version; formats.tsv adds the release revision and dist tag.
native=$("$here/native-version.sh" --style "$(format_field "$format" version_style)" "$version") || exit 2
[ "$(rpm --eval '%{?fedora}')" = 44 ] && [ "$(rpm --eval '%{_arch}')" = x86_64 ] \
    || { echo 'error: use the Fedora 44 x86_64 build image' >&2; exit 1; }
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT

name=branch-truce-$native
mkdir -p "$work/rpmbuild/SOURCES"
"$here/stage-source.sh" --from "$root" --to "$work/$name"
tar -C "$work" -czf "$work/rpmbuild/SOURCES/$name.tar.gz" "$name"

cache_prepare
cache_begin native-rpm-distro "$work"
status=0
rpmbuild -bb --define "_topdir $work/rpmbuild" \
    --define "_smp_mflags -j${CMAKE_BUILD_PARALLEL_LEVEL:-2}" \
    --define "branch_truce_version $version" \
    --define "branch_truce_native_version $native" \
    "$root/packaging/rpm/branch-truce.spec" || status=$?
cache_end
[ "$status" -eq 0 ] || exit "$status"

built=("$work"/rpmbuild/RPMS/*/*.rpm)
[ "${#built[@]}" -eq 1 ] && [ -f "${built[0]}" ] || { echo "error: expected one RPM, found: ${built[*]}" >&2; exit 1; }
builder_publish "$format" "$version" "${built[0]}" "$output"
