#!/usr/bin/env bash
# Exercise setup CLI rejection paths before tools or packages are consulted.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
assertions=0

expect_rejected() {
    local expected_message=$1 output status
    shift
    if output=$("$@" 2>&1); then status=0; else status=$?; fi
    if [[ "$status" != 2 || "$output" != *"$expected_message"* ]]; then
        printf 'Expected exit 2 with %q from:' "$expected_message" >&2
        printf ' %q' "$@" >&2
        printf '\nActual exit %s:\n%s\n' "$status" "$output" >&2
        exit 1
    fi
    assertions=$((assertions + 1))
}

expect_rejected 'Usage:' "$root/tools/prepare-worktree.sh" invalid-mode
for mode in desktop core sanitize; do
    expect_rejected 'Build directory must be absolute.' \
        "$root/tools/prepare-worktree.sh" "$mode" 'relative build directory'
done
expect_rejected 'Usage:' "$root/tools/provision-debian.sh" --invalid-mode
printf '%s CLI guard assertions passed\n' "$assertions"
