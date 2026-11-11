#!/usr/bin/env bash
# Root entry point of the CI test container: copies the read-only /src
# checkout, prepares an unprivileged account, then runs desktop-suite.sh as
# that account, with reports in /reports.
#
#   test-desktop.sh VERSION UID GID
set -euo pipefail
case "${1-}" in -h|--help) echo "usage: $0 VERSION UID GID"; exit 0 ;; esac
[ $# -eq 3 ] || { echo "usage: $0 VERSION UID GID" >&2; exit 2; }
version=$1 test_uid=$2 test_gid=$3
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
fidelity=()
[ "${BRANCH_TRUCE_NATIVE_FIDELITY:-0}" -eq 0 ] || fidelity=(--native-fidelity)
runuser -u "$test_user" -- env HOME="$work/home" bash "$work/repo/packaging/scripts/desktop-suite.sh" \
    --source "$work/repo" --work "$work" --version "$version" --reports /reports "${fidelity[@]}"
