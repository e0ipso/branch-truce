#!/usr/bin/env bash
# Root container entry point: copy first, then run every build/test as its owner.
set -euo pipefail
version="${1:?version}"; test_uid="${2:?uid}"; test_gid="${3:?gid}"
[ "$test_uid" -ne 0 ] || { echo 'error: tests require an unprivileged user' >&2; exit 2; }
case "${BRANCH_TRUCE_NATIVE_FIDELITY:-0}" in
    0|1) ;;
    *) echo 'error: BRANCH_TRUCE_NATIVE_FIDELITY must be 0 or 1' >&2; exit 2 ;;
esac
work="$(mktemp -d /var/tmp/bt-ci.XXXXXX)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/repo" "$work/home" /reports
# Exclude disposable outputs before copying, including private agent metadata.
tar -C /src --exclude=./build --exclude=./node_modules --exclude=./dist --exclude=./.ai -cf - . | tar -C "$work/repo" -xf -
if [ -f "$work/repo/.git" ]; then rm "$work/repo/.git"; fi
if [ ! -d "$work/repo/.git" ]; then
    git -C "$work/repo" init --quiet --initial-branch=main
    git -C "$work/repo" add -A
    git -C "$work/repo" -c user.name='Container Check' -c user.email=container-check@example.invalid \
        commit --quiet -m 'Worktree snapshot'
fi
# Missing uid is an expected lookup result, not a preparation failure.
test_user="$(getent passwd "$test_uid" | cut -d: -f1)" || test_user=''
if [ -z "$test_user" ]; then
    getent group "$test_gid" >/dev/null || groupadd --gid "$test_gid" bt-test
    useradd --uid "$test_uid" --gid "$test_gid" --no-create-home bt-test
    test_user=bt-test
fi
if [ -n "${BRANCH_TRUCE_COMPILER_CACHE_DIR:-}" ]; then
    source /src/packaging/scripts/compiler-cache.sh
    cache_prepare
    # Group zero owns the rootless bind mount without changing its host owner.
    usermod -a -G root "$test_user"
fi
chown -R "$test_uid:$test_gid" "$work" /reports
runuser -u "$test_user" -- env HOME="$work/home" bash -euc '
    work="$1"; version="$2"
    # Fail before configuring/building when the report mount is not writable.
    test -w /reports
    : > /reports/write-probe
    rm /reports/write-probe
    if command -v ccache >/dev/null; then
        bash "$work/repo/tests/packaging/CompilerCache.sh"
    fi
    source "$work/repo/packaging/scripts/compiler-cache.sh"
    cache_begin desktop-Debug "$work"
    cmake -S "$work/repo" -B "$work/build" -G "Unix Makefiles" \
        -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
        -DBRANCH_TRUCE_BUILD_DESKTOP=ON -DBRANCH_TRUCE_WARNINGS_AS_ERRORS=ON \
        -DBRANCH_TRUCE_VERSION="$version" "${cache_cmake_args[@]}"
    build_status=0
    cmake --build "$work/build" --parallel 2 || build_status=$?
    cache_end
    [ "$build_status" -eq 0 ] || exit "$build_status"
    ctest --test-dir "$work/build" --show-only=json-v1 > /reports/tests.json
    unset BRANCH_TRUCE_UI_EVIDENCE_DIR
    if [ "${BRANCH_TRUCE_NATIVE_FIDELITY:-0}" -eq 1 ]; then
        export BRANCH_TRUCE_UI_EVIDENCE_DIR=/reports/ui-scenes
    fi
    result=0
    ctest --test-dir "$work/build" --parallel 2 --output-on-failure --output-junit /reports/ctest.xml || result=$?
    cp "$work/build/Testing/Temporary/LastTest.log" /reports/LastTest.log
    # Full fidelity is requested by the canonical CI leg, separately from the
    # complete desktop and package checks that remain required on every target.
    native_requested=false
    if [ "${BRANCH_TRUCE_NATIVE_FIDELITY:-0}" -eq 1 ]; then
        native_requested=true
        native_result=0
        python3 "$work/repo/tools/native-replay/ci.py" --source "$work/repo" \
            --build "$work/build" --output /reports/native || native_result=$?
        if [ "$native_result" -ne 0 ] && [ "$result" -eq 0 ]; then result="$native_result"; fi
        # One separate variant proves the dedicated suite with highlighting OFF;
        # the full default desktop build remains the source of all other checks.
        mkdir -p /reports/highlighting-off
        off_configure=0; off_build=0; off_test=0; off_inventory=0; off_executed=false
        cache_begin desktop-Debug-highlighting-off "$work"
        cmake -S "$work/repo" -B "$work/build-highlighting-off" -G "Unix Makefiles" \
            -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
            -DBRANCH_TRUCE_BUILD_DESKTOP=ON -DBRANCH_TRUCE_WARNINGS_AS_ERRORS=ON \
            -DBRANCH_TRUCE_SYNTAX_HIGHLIGHTING=OFF -DBRANCH_TRUCE_VERSION="$version" \
            "${cache_cmake_args[@]}" > /reports/highlighting-off/configure.log 2>&1 || off_configure=$?
        if [ "$off_configure" -eq 0 ]; then
            cmake --build "$work/build-highlighting-off" --parallel 2 \
                --target branch-truce branch_truce_syntax_highlighting_tests \
                > /reports/highlighting-off/build.log 2>&1 || off_build=$?
        else
            off_build=125
        fi
        cache_end
        if [ "$off_configure" -eq 0 ] && [ "$off_build" -eq 0 ]; then
            ctest --test-dir "$work/build-highlighting-off" --show-only=json-v1 \
                -R "^app[.]syntax-highlighting$" > /reports/highlighting-off/tests.json || off_inventory=$?
            off_executed=true
            ctest --test-dir "$work/build-highlighting-off" --output-on-failure --no-tests=error \
                -R "^app[.]syntax-highlighting$" --output-junit /reports/highlighting-off/ctest.xml \
                > /reports/highlighting-off/ctest.log 2>&1 || off_test=$?
            if [ -f "$work/build-highlighting-off/Testing/Temporary/LastTest.log" ]; then
                cp "$work/build-highlighting-off/Testing/Temporary/LastTest.log" /reports/highlighting-off/LastTest.log
            fi
        else
            off_test=125; off_inventory=125
            printf "{\"tests\":[],\"not_run\":true}\n" > /reports/highlighting-off/tests.json
        fi
        off_junit=false; off_lastlog=false
        if [ -f /reports/highlighting-off/ctest.xml ]; then off_junit=true; fi
        if [ -f /reports/highlighting-off/LastTest.log ]; then off_lastlog=true; fi
        printf "{\"requested\":true,\"syntax_highlighting\":false,\"test_executed\":%s,\"configure_exit\":%s,\"build_exit\":%s,\"inventory_exit\":%s,\"test_exit\":%s,\"junit_present\":%s,\"last_test_log_present\":%s,\"not_run_exit\":125}\n" \
            "$off_executed" "$off_configure" "$off_build" "$off_inventory" "$off_test" "$off_junit" "$off_lastlog" > /reports/highlighting-off/scope.json
        for off_status in "$off_configure" "$off_build" "$off_inventory" "$off_test"; do
            if [ "$off_status" -ne 0 ] && [ "$result" -eq 0 ]; then result="$off_status"; fi
        done
    fi
    printf "{\"requested\":%s}\n" "$native_requested" > /reports/native-fidelity-scope.json
    exit "$result"
' bash "$work" "$version"
