#!/usr/bin/env bash
# Read shared prerequisites, then configure only this checkout. No provisioning.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
mode=${1:-desktop}
case "$mode" in desktop|core|sanitize) ;; *) echo "Usage: $0 [desktop|core|sanitize] [absolute-build-directory]" >&2; exit 2 ;; esac
build=${2:-"$root/build/$mode"}
[[ "$build" == /* ]] || { echo 'Build directory must be absolute.' >&2; exit 2; }
for tool in cmake c++ make git; do
    command -v "$tool" >/dev/null || { echo "Missing shared prerequisite: $tool" >&2; exit 1; }
done
cmake --version
c++ --version
git --version
version=$(git version | awk '{print $3}')
[[ "$(printf '%s\n' 2.43.0 "$version" | sort -V | head -1)" == 2.43.0 ]] || {
    echo 'Git >= 2.43 is required.' >&2; exit 1;
}
options=(-S "$root" -B "$build" -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Debug
    -DBUILD_TESTING=ON -DBRANCH_TRUCE_WARNINGS_AS_ERRORS=ON)
case "$mode" in
    desktop) options+=(-DBRANCH_TRUCE_BUILD_DESKTOP=ON -DBRANCH_TRUCE_SANITIZERS=OFF) ;;
    core) options+=(-DBRANCH_TRUCE_BUILD_DESKTOP=OFF -DBRANCH_TRUCE_SANITIZERS=OFF) ;;
    sanitize) options+=(-DBRANCH_TRUCE_BUILD_DESKTOP=OFF -DBRANCH_TRUCE_SANITIZERS=ON) ;;
esac
cmake "${options[@]}"
