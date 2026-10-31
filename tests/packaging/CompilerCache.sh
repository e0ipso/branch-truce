#!/usr/bin/env bash
# Real compiler integration: relocation, invalidation, namespaces and fallback.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
source "$root/packaging/scripts/compiler-cache.sh"
command -v ccache >/dev/null || { echo 'ccache required' >&2; exit 77; }
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
export BRANCH_TRUCE_COMPILER_CACHE_DIR="$work/cache"
export BRANCH_TRUCE_COMPILER_CACHE_NAMESPACE=fixture-toolchain
export BRANCH_TRUCE_COMPILER_CACHE_REPORT_DIR="$work/reports"
cache_prepare
cache_generator='Unix Makefiles'
if command -v ninja >/dev/null; then cache_generator=Ninja; fi
build_fixture() {
    local name="$1" value="$2" config="${3:-Debug}"
    mkdir -p "$work/$name/source"
    cat > "$work/$name/source/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.28)
project(CacheFixture LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
add_executable(fixture main.cpp)
CMAKE
    printf '#include <cstdio>\nint main() { std::printf("%%d %%s\\n", %s, __FILE__); }\n' "$value" > "$work/$name/source/main.cpp"
    cache_begin "$config" "$work/$name"
    cmake -G "$cache_generator" -S "$work/$name/source" -B "$work/$name/build" -DCMAKE_BUILD_TYPE="$config" "${cache_cmake_args[@]}" >/dev/null
    cmake --build "$work/$name/build" --parallel 2 >/dev/null
    cache_end
    "$work/$name/build/fixture" > "$work/output"
    grep -q "^$value /bt/source/main.cpp$" "$work/output"
}
counter() { awk -v key="$1" '$1 == key { print $2 }' "$cache_report.stats"; }
build_fixture cold 17
[ "$(counter cache_miss)" -gt 0 ]
build_fixture relocated 17
[ "$(( $(counter direct_cache_hit) + $(counter preprocessed_cache_hit) ))" -gt 0 ]
[ "$(counter cache_miss)" -eq 0 ]
build_fixture changed 29
[ "$(counter cache_miss)" -gt 0 ]
# A different configuration must not consume Debug entries.
build_fixture release 17 Release
[ "$(counter cache_miss)" -gt 0 ]
export BRANCH_TRUCE_COMPILER_CACHE_NAMESPACE=another-toolchain
build_fixture isolated 17
[ "$(counter cache_miss)" -gt 0 ]
# A failed cache probe falls back to an ordinary compiler, including bad paths.
export BRANCH_TRUCE_COMPILER_CACHE_DIR=/proc/branch-truce-unwritable
cache_begin Debug "$work/fallback"
[ "${#cache_cmake_args[@]}" -eq 0 ]
mkdir -p "$work/fallback/source"
printf 'int main() { return 0; }\n' > "$work/fallback/source/main.cpp"
cp "$work/cold/source/CMakeLists.txt" "$work/fallback/source/"
cmake -S "$work/fallback/source" -B "$work/fallback/build" "${cache_cmake_args[@]}" >/dev/null
cmake --build "$work/fallback/build" >/dev/null
"$work/fallback/build/fixture"
cache_end
if [ -f /etc/fedora-release ]; then
    bash "$root/tests/packaging/FedoraCompilerCache.sh"
fi
echo 'compiler cache relocation, invalidation, configuration, toolchain and unavailable-cache checks passed'
