#!/usr/bin/env bash
# Run a command that launches the real branch-truce binary with private HOME and XDG
# directories, remove them afterwards, and fail if the run created anything under the
# recovery root that the caller's own environment resolves to.
#
# Usage: with-private-xdg.sh COMMAND [ARGUMENT...]
#
# The recovery root is <data home>/Branch Truce/recovery, where the data home is the
# caller's XDG_DATA_HOME when absolute, else $HOME/.local/share. The check only looks for
# new paths, so another process reading or updating an existing journal does not fail it.
set -uo pipefail

(($# > 0)) || { echo 'Usage: with-private-xdg.sh COMMAND [ARGUMENT...]' >&2; exit 2; }

data_home=${XDG_DATA_HOME:-}
[[ "$data_home" == /* ]] || data_home=${HOME:?HOME must be set}/.local/share
real_root="$data_home/Branch Truce/recovery"

list_paths() {
    [[ -d "$real_root" ]] || return 0
    LC_ALL=C find "$real_root" -mindepth 1 -print | LC_ALL=C sort
}

private=$(mktemp -d "${TMPDIR:-/tmp}/branch-truce-xdg.XXXXXX") || exit 2
trap 'rm -rf -- "$private"' EXIT
# CTest ends a timed-out test by signalling its whole process group. Bash runs these traps once
# the command has exited. The command stays in the foreground: a background command would
# inherit an ignored SIGINT, which hides interrupt handling from the tests that send one.
trap 'exit 143' TERM
trap 'exit 130' INT

mkdir -m 0700 "$private/runtime" "$private/data" "$private/config" "$private/cache" || exit 2
before=$(list_paths)

env HOME="$private" XDG_DATA_HOME="$private/data" XDG_CONFIG_HOME="$private/config" \
    XDG_CACHE_HOME="$private/cache" XDG_RUNTIME_DIR="$private/runtime" "$@"
status=$?

after=$(list_paths)
created=$(comm -13 <(printf '%s\n' "$before") <(printf '%s\n' "$after"))
if [[ -n "$created" ]]; then
    printf 'This run created paths under the real recovery root %s:\n%s\n' "$real_root" "$created" >&2
    exit 1
fi
exit "$status"
