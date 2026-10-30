#!/usr/bin/env bash
# Real distro CMake/RPM flags must cache across paths and retain valid DWARF.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
for cache_rpm_tool in rpmbuild ccache cmake readelf; do
    command -v "$cache_rpm_tool" >/dev/null || exit 77
done
source "$root/packaging/scripts/compiler-cache.sh"
cache_rpm_work="$(mktemp -d)"
trap 'rm -rf "$cache_rpm_work"' EXIT
export BRANCH_TRUCE_COMPILER_CACHE_DIR="$cache_rpm_work/cache"
export BRANCH_TRUCE_COMPILER_CACHE_NAMESPACE=rpm-relocation-fixture
export BRANCH_TRUCE_COMPILER_CACHE_REPORT_DIR="$cache_rpm_work/reports"
cache_prepare
for cache_rpm_pass in cold warm changed; do
    cache_rpm_build="$cache_rpm_work/$cache_rpm_pass"
    mkdir -p "$cache_rpm_build/rpmbuild/SOURCES" "$cache_rpm_build/cache-fixture"
    cache_rpm_value=17
    [ "$cache_rpm_pass" != changed ] || cache_rpm_value=29
    cat > "$cache_rpm_build/cache-fixture/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.28)
project(CacheFixture LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
add_executable(cache-fixture main.cpp)
install(TARGETS cache-fixture DESTINATION bin)
CMAKE
    printf '#include <cstdio>\nint main() { std::printf("%%d\\n", %s); }\n' "$cache_rpm_value" > "$cache_rpm_build/cache-fixture/main.cpp"
    tar -C "$cache_rpm_build" -czf "$cache_rpm_build/rpmbuild/SOURCES/cache-fixture.tar.gz" cache-fixture
    cat > "$cache_rpm_build/probe.spec" <<'SPEC'
Name: cache-fixture
Version: 1
Release: 1%{?dist}
Summary: Compiler path fixture
License: MIT
Source0: cache-fixture.tar.gz
%description
Compiler cache RPM integration fixture.
%prep
%autosetup -n cache-fixture
%build
%cmake -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES="${BRANCH_TRUCE_CACHE_CMAKE_INCLUDE}"
%cmake_build
cp "%{_vpath_builddir}/cache-fixture" "$CACHE_RPM_FIXTURE_BINARY"
%install
%cmake_install
%files
%{_bindir}/cache-fixture
SPEC
    cache_begin native-rpm-distro "$cache_rpm_build"
    export CACHE_RPM_FIXTURE_BINARY="$cache_rpm_build/compiled-fixture"
    if ! rpmbuild -bb --define "_topdir $cache_rpm_build/rpmbuild" \
        --define '_smp_mflags -j2' "$cache_rpm_build/probe.spec" > "$cache_rpm_build/rpm.log" 2>&1; then
        cat "$cache_rpm_build/rpm.log"
        exit 1
    fi
    cache_end
    [ "$("$CACHE_RPM_FIXTURE_BINARY")" = "$cache_rpm_value" ]
    readelf --debug-dump=info "$CACHE_RPM_FIXTURE_BINARY" | grep 'DW_AT_comp_dir.* /bt/build' >/dev/null
    cache_rpm_hits="$(awk '$1 == "direct_cache_hit" || $1 == "preprocessed_cache_hit" { total += $2 } END { print total+0 }' "$cache_report.stats")"
    cache_rpm_misses="$(awk '$1 == "cache_miss" { print $2 }' "$cache_report.stats")"
    if [ "$cache_rpm_pass" = warm ]; then
        [ "$cache_rpm_hits" -gt 0 ] && [ "$cache_rpm_misses" -eq 0 ] || {
            echo 'FAIL: relocated native RPM compiler never reads the cache' >&2; exit 1;
        }
    else
        [ "$cache_rpm_misses" -gt 0 ]
    fi
done
echo 'native RPM cache relocation, source invalidation and normalized DWARF passed'
