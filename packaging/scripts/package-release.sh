#!/usr/bin/env bash
# Builds the branch-truce DEB package for a target OS base in a throwaway
# source tree.
#
#   package-release.sh --version <version> --format kubuntu-deb|tuxedo-deb --output <dir>
#
# Prints the absolute path of the produced .deb on the last line.

set -euo pipefail

usage() {
    echo "usage: $0 --version <version> --format kubuntu-deb|tuxedo-deb --output <dir>" >&2
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

case "$format" in
    kubuntu-deb)
        suite=resolute
        suffix=-ubuntu26.04
        ;;
    tuxedo-deb)
        suite=noble
        suffix=-tuxedo24.04
        ;;
    *) usage ;;
esac

packaging_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
root_dir="$(cd "$packaging_dir/.." && pwd)"
native_version="$("$packaging_dir/scripts/native-version.sh" "$version")" || exit 2
mkdir -p "$output"
output="$(cd "$output" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

tree="$work/branch-truce-$native_version"
mkdir -p "$tree/packaging"
cp -a "$root_dir/CMakeLists.txt" "$root_dir/cmake" "$root_dir/src" "$tree/"
cp -a "$packaging_dir/CMakeLists.txt" \
    "$packaging_dir/com.canpicasoft.branch-truce.desktop" \
    "$packaging_dir/com.canpicasoft.branch-truce.metainfo.xml" \
    "$packaging_dir/com.canpicasoft.branch-truce.svg" \
    "$tree/packaging/"
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
name="branch-truce_${native_version}_amd64${suffix}.deb"
cp "$work/$deb" "$output/$name"
echo "$output/$name"
