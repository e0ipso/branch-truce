#!/usr/bin/env bash
# Builds the DEB of a format with dpkg-buildpackage. Run it through
# package-release.sh, inside the format's build image:
#
#   build-deb.sh FORMAT VERSION ABSOLUTE-OUTPUT-DIR
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)
source "$here/builder-common.sh"
source "$here/compiler-cache.sh"
builder_args "$0" "$@" || exit 2
format=$1 version=$2 output=$3
native=$(package_native_version "$format" "$version") || exit 2
# The changelog's distribution is the release of the build image.
suite=$(. /etc/os-release && printf '%s' "${VERSION_CODENAME-}")
[ -n "$suite" ] || { echo 'error: /etc/os-release of the build image has no VERSION_CODENAME' >&2; exit 1; }
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT

tree=$work/branch-truce-$native
"$here/stage-source.sh" --from "$root" --to "$tree" packaging/debian
mv -- "$tree/packaging/debian" "$tree/debian"
cat > "$tree/debian/changelog" <<CHANGELOG
branch-truce ($native) $suite; urgency=medium

  * Branch Truce $version.

 -- Branch Truce contributors <contributors@example.invalid>  $(TZ=UTC date -R ${SOURCE_DATE_EPOCH:+-d "@$SOURCE_DATE_EPOCH"})
CHANGELOG

cache_prepare
cache_begin native-deb-distro "$work"
status=0
(cd -- "$tree" && BRANCH_TRUCE_VERSION="$version" dpkg-buildpackage -b --no-sign) || status=$?
cache_end
[ "$status" -eq 0 ] || exit "$status"

built=("$work"/*.deb)
[ "${#built[@]}" -eq 1 ] && [ -f "${built[0]}" ] || { echo "error: expected one DEB, found: ${built[*]}" >&2; exit 1; }
builder_publish "$format" "$version" "${built[0]}" "$output"
