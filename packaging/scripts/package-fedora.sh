#!/usr/bin/env bash
# Run inside the Fedora 44 build image. Last line is the validated RPM path.
set -euo pipefail
usage() { echo "usage: $0 --version <application-semver> --output <dir>" >&2; exit 2; }
version= output=
while [ $# -gt 0 ]; do
    [ $# -ge 2 ] || usage
    case "$1" in
        --version) version="$2" ;;
        --output) output="$2" ;;
        *) usage ;;
    esac
    shift 2
done
[ -n "$version" ] && [ -n "$output" ] || usage
packaging_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
root_dir="$(cd "$packaging_dir/.." && pwd)"
native_version="$("$packaging_dir/scripts/native-version.sh" "$version")" || exit 2
[ "$(rpm --eval '%{?fedora}')" = 44 ] && [ "$(rpm --eval '%{_arch}')" = x86_64 ] \
    || { echo 'error: use the Fedora 44 x86_64 build image' >&2; exit 1; }
mkdir -p "$output"
output="$(cd "$output" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
name="branch-truce-$native_version"
tree="$work/$name"
mkdir -p "$tree/packaging" "$tree/assets/branding/platform" "$work/rpmbuild/SOURCES"
cp -a "$root_dir/CMakeLists.txt" "$root_dir/LICENSE" "$root_dir/cmake" "$root_dir/src" "$tree/"
cp -a "$packaging_dir/CMakeLists.txt" "$packaging_dir/com.canpicasoft.branch-truce.desktop" \
    "$packaging_dir/com.canpicasoft.branch-truce.metainfo.xml" "$tree/packaging/"
cp -a "$root_dir/assets/branding/platform/linux" "$tree/assets/branding/platform/"
cp -a "$root_dir/assets/branding/svg" "$tree/assets/branding/"
tar -C "$work" -czf "$work/rpmbuild/SOURCES/$name.tar.gz" "$name"
rpmbuild -bb --define "_topdir $work/rpmbuild" \
    --define "_smp_mflags -j${CMAKE_BUILD_PARALLEL_LEVEL:-2}" \
    --define "branch_truce_version $version" \
    --define "branch_truce_native_version $native_version" \
    "$packaging_dir/rpm/branch-truce.spec"
# Never rename an RPM to conceal metadata drift.
rpm_file="$work/rpmbuild/RPMS/x86_64/$name-1.fc44.x86_64.rpm"
[ -f "$rpm_file" ] || { echo "error: expected RPM $rpm_file was not built" >&2; exit 1; }
identity="$(rpm -qp --qf '%{NAME}|%{EPOCHNUM}|%{VERSION}|%{RELEASE}|%{ARCH}|%{LICENSE}\n' "$rpm_file")"
[ "$identity" = "branch-truce|0|$native_version|1.fc44|x86_64|GPL-3.0-or-later AND CC0-1.0" ] \
    || { echo "error: RPM identity mismatch: $identity" >&2; exit 1; }
cp "$rpm_file" "$output/"
echo "$output/$(basename "$rpm_file")"
