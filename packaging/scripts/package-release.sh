#!/usr/bin/env bash
# Builds the branch-truce native release package for a target OS base in a throwaway
# source tree.
#
#   package-release.sh --version <version> --format kubuntu-deb|fedora-rpm|arch-pkg --output <dir>
#
# Prints the absolute path of the produced native package on the last line.

set -euo pipefail

usage() {
    echo "usage: $0 --version <version> --format kubuntu-deb|fedora-rpm|arch-pkg --output <dir>" >&2
    exit 2
}

version=""
format=""
output=""
while [ $# -gt 0 ]; do
    case "$1" in
        --version) version="${2-}"; shift 2 || usage ;;
        --format) format="${2-}"; shift 2 || usage ;;
        --output) output="${2-}"; shift 2 || usage ;;
        *) usage ;;
    esac
done
[ -n "$version" ] && [ -n "$format" ] && [ -n "$output" ] || usage

packaging_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
root_dir="$(cd "$packaging_dir/.." && pwd)"
# package_info assigns these outputs for its Bash callers.
package_filename=''
package_native_version=''
source "$packaging_dir/scripts/package-info.sh"
package_info "$format" "$version" || exit 2
case "$format" in
    fedora-rpm) exec "$packaging_dir/scripts/package-fedora.sh" --version "$version" --output "$output" ;;
    arch-pkg) exec "$packaging_dir/scripts/package-arch.sh" --version "$version" --output "$output" ;;
esac
suite=resolute
native_version="$package_native_version"
mkdir -p "$output"
output="$(cd "$output" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

tree="$work/branch-truce-$native_version"
mkdir -p "$tree/packaging" "$tree/assets/branding/platform"
cp -a "$root_dir/CMakeLists.txt" "$root_dir/cmake" "$root_dir/src" "$tree/"
cp -a "$packaging_dir/CMakeLists.txt" \
    "$packaging_dir/com.canpicasoft.branch-truce.desktop" \
    "$packaging_dir/com.canpicasoft.branch-truce.metainfo.xml" \
    "$tree/packaging/"
# The branding exports hold the installed application icons and the vector
# artwork the executable can embed. tools/build-artifact.sh copies the same.
cp -a "$root_dir/assets/branding/platform/linux" "$tree/assets/branding/platform/"
cp -a "$root_dir/assets/branding/svg" "$tree/assets/branding/"
cp -a "$packaging_dir/debian" "$tree/debian"
rm -f "$tree/debian/compat"
chmod +x "$tree/debian/rules"

cat > "$tree/debian/changelog" <<EOF
branch-truce ($native_version) $suite; urgency=medium

  * Branch Truce $version.

 -- Branch Truce contributors <contributors@example.invalid>  $(TZ=UTC date -R ${SOURCE_DATE_EPOCH:+-d "@$SOURCE_DATE_EPOCH"})
EOF

(cd "$tree" && BRANCH_TRUCE_VERSION="$version" dpkg-buildpackage -b --no-sign)

deb="branch-truce_${native_version}_amd64.deb"
name="$package_filename"
[ "$(dpkg-deb -f "$work/$deb" Package)" = branch-truce ] \
    && [ "$(dpkg-deb -f "$work/$deb" Version)" = "$native_version" ] \
    && [ "$(dpkg-deb -f "$work/$deb" Architecture)" = amd64 ] \
    || { echo 'error: built DEB identity mismatch' >&2; exit 1; }
cp "$work/$deb" "$output/$name"
echo "$output/$name"
