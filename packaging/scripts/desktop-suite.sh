#!/usr/bin/env bash
# Builds a copied checkout and runs its complete desktop CTest suite as the
# current, unprivileged user. test-desktop.sh runs it inside the CI test image.
#
#   desktop-suite.sh --source DIR --work DIR --version SEMVER --reports DIR [--native-fidelity]
#
# DIR/build holds the build. REPORTS receives the CTest inventory
# (tests.json), JUnit results (ctest.xml), LastTest.log and
# native-fidelity-scope.json. With --native-fidelity it also runs the native
# replay collection (REPORTS/native) and a second build with syntax
# highlighting off for that suite alone (REPORTS/highlighting-off, with
# scope.json). Exits with the first failing status, after every step ran.
set -euo pipefail

source "$(dirname -- "${BASH_SOURCE[0]}")/cli.sh"
usage() { echo "usage: $0 --source DIR --work DIR --version SEMVER --reports DIR [--native-fidelity]"; }
declare -A options=()
cli_parse options - 'source=! work=! version=! reports=! native-fidelity' "$@" || { cli_usage $? usage; exit; }
source_dir=${options[source]} work=${options[work]} version=${options[version]} reports=${options[reports]}
fidelity=false
[ -z "${options[native-fidelity]-}" ] || fidelity=true

# Fail before configuring or building when the report directory is not writable.
test -w "$reports"
: > "$reports/write-probe"
rm "$reports/write-probe"
source "$source_dir/packaging/scripts/compiler-cache.sh"

# configure BUILD-DIR CACHE-PROFILE [CMAKE-ARGUMENT...]: a Debug desktop build tree with tests.
configure() {
    local build=$1 profile=$2
    shift 2
    local -a cache_args
    cache_begin "$profile" "$work"
    mapfile -t cache_args < <(cache_cmake_args)
    cmake -S "$source_dir" -B "$build" -G "Unix Makefiles" \
        -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
        -DBRANCH_TRUCE_BUILD_DESKTOP=ON -DBRANCH_TRUCE_WARNINGS_AS_ERRORS=ON \
        -DBRANCH_TRUCE_VERSION="$version" "$@" "${cache_args[@]}"
}

result=0
keep_first_failure() { if [ "$1" -ne 0 ] && [ "$result" -eq 0 ]; then result=$1; fi; }

configure "$work/build" desktop-Debug
build_status=0
cmake --build "$work/build" --parallel 2 || build_status=$?
cache_end
[ "$build_status" -eq 0 ] || exit "$build_status"
ctest --test-dir "$work/build" --show-only=json-v1 > "$reports/tests.json"
unset BRANCH_TRUCE_UI_EVIDENCE_DIR
if $fidelity; then export BRANCH_TRUCE_UI_EVIDENCE_DIR="$reports/ui-scenes"; fi
status=0
ctest --test-dir "$work/build" --parallel 2 --output-on-failure --output-junit "$reports/ctest.xml" || status=$?
keep_first_failure "$status"
cp "$work/build/Testing/Temporary/LastTest.log" "$reports/LastTest.log"

# Full fidelity is requested by the canonical CI leg, separately from the
# complete desktop and package checks that every target runs.
if $fidelity; then
    status=0
    python3 "$source_dir/tools/native-replay/ci.py" --source "$source_dir" \
        --build "$work/build" --output "$reports/native" || status=$?
    keep_first_failure "$status"

    # One separate variant proves the dedicated suite with highlighting OFF;
    # the default build above is the source of every other check.
    off=$reports/highlighting-off
    mkdir -p "$off"
    configure_status=0 build_status=0 inventory_status=0 test_status=0 executed=false
    configure "$work/build-highlighting-off" desktop-Debug-highlighting-off \
        -DBRANCH_TRUCE_SYNTAX_HIGHLIGHTING=OFF > "$off/configure.log" 2>&1 || configure_status=$?
    if [ "$configure_status" -eq 0 ]; then
        cmake --build "$work/build-highlighting-off" --parallel 2 \
            --target branch-truce branch_truce_app_syntax_highlighting_tests \
            > "$off/build.log" 2>&1 || build_status=$?
    else
        build_status=125
    fi
    cache_end
    if [ "$configure_status" -eq 0 ] && [ "$build_status" -eq 0 ]; then
        ctest --test-dir "$work/build-highlighting-off" --show-only=json-v1 \
            -R "^app[.]syntax-highlighting$" > "$off/tests.json" || inventory_status=$?
        executed=true
        ctest --test-dir "$work/build-highlighting-off" --output-on-failure --no-tests=error \
            -R "^app[.]syntax-highlighting$" --output-junit "$off/ctest.xml" \
            > "$off/ctest.log" 2>&1 || test_status=$?
        if [ -f "$work/build-highlighting-off/Testing/Temporary/LastTest.log" ]; then
            cp "$work/build-highlighting-off/Testing/Temporary/LastTest.log" "$off/LastTest.log"
        fi
    else
        test_status=125 inventory_status=125
        jq -cn '{tests: [], not_run: true}' > "$off/tests.json"
    fi
    junit=false lastlog=false
    [ ! -f "$off/ctest.xml" ] || junit=true
    [ ! -f "$off/LastTest.log" ] || lastlog=true
    jq -cn --argjson executed "$executed" --argjson configure "$configure_status" \
        --argjson build "$build_status" --argjson inventory "$inventory_status" --argjson test "$test_status" \
        --argjson junit "$junit" --argjson lastlog "$lastlog" \
        '{requested: true, syntax_highlighting: false, test_executed: $executed, configure_exit: $configure,
          build_exit: $build, inventory_exit: $inventory, test_exit: $test, junit_present: $junit,
          last_test_log_present: $lastlog, not_run_exit: 125}' > "$off/scope.json"
    for status in "$configure_status" "$build_status" "$inventory_status" "$test_status"; do
        keep_first_failure "$status"
    done
fi
jq -cn --argjson requested "$fidelity" '{requested: $requested}' > "$reports/native-fidelity-scope.json"
exit "$result"
