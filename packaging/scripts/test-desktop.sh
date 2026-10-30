#!/usr/bin/env bash
# Root container entry point: copy first, then run every build/test as its owner.
set -euo pipefail
version="${1:?version}"; test_uid="${2:?uid}"; test_gid="${3:?gid}"
[ "$test_uid" -ne 0 ] || { echo 'error: tests require an unprivileged user' >&2; exit 2; }
work="$(mktemp -d /tmp/bt-ci.XXXXXX)"
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
chown -R "$test_uid:$test_gid" "$work" /reports
runuser -u "$test_user" -- env HOME="$work/home" bash -euc '
    work="$1"; version="$2"
    # Fail before configuring/building when the report mount is not writable.
    test -w /reports
    : > /reports/write-probe
    rm /reports/write-probe
    cmake -S "$work/repo" -B "$work/build" -G "Unix Makefiles" \
        -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
        -DBRANCH_TRUCE_BUILD_DESKTOP=ON -DBRANCH_TRUCE_WARNINGS_AS_ERRORS=ON \
        -DBRANCH_TRUCE_VERSION="$version"
    cmake --build "$work/build" --parallel 2
    ctest --test-dir "$work/build" --show-only=json-v1 > /reports/tests.json
    result=0
    ctest --test-dir "$work/build" --parallel 2 --output-on-failure --output-junit /reports/ctest.xml || result=$?
    cp "$work/build/Testing/Temporary/LastTest.log" /reports/LastTest.log
    exit "$result"
' bash "$work" "$version"
