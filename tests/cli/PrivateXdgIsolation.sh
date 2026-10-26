#!/usr/bin/env bash
# Exercise tests/with-private-xdg.sh against a stand-in recovery root. Nothing here touches
# the real HOME: every run points HOME or XDG_DATA_HOME at a temporary directory.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
wrapper="$root/tests/with-private-xdg.sh"
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
assertions=0

fail() {
    printf '%s\n' "$1" >&2
    exit 1
}

# run_wrapper STATUS_VAR OUTPUT_VAR ENV... -- COMMAND...
run_wrapper() {
    local -n status_ref=$1 output_ref=$2
    shift 2
    local environment=()
    while (($# > 0)) && [[ "$1" != -- ]]; do environment+=("$1"); shift; done
    shift
    if output_ref=$(env "${environment[@]}" "$wrapper" "$@" 2>&1); then status_ref=0; else status_ref=$?; fi
}

# A run that stays inside its private directories keeps the command's status and leaves
# no directory behind.
mkdir "$scratch/tmp-clean" "$scratch/data-clean"
run_wrapper status output TMPDIR="$scratch/tmp-clean" XDG_DATA_HOME="$scratch/data-clean" -- \
    bash -c 'for name in HOME XDG_DATA_HOME XDG_CONFIG_HOME XDG_CACHE_HOME XDG_RUNTIME_DIR; do
                 [[ -d "${!name}" && "${!name}" == "$TMPDIR"/branch-truce-xdg.* ]] || exit 11
             done
             [[ $(stat -c %a "$XDG_RUNTIME_DIR") == 700 ]] || exit 12
             mkdir -p "$XDG_DATA_HOME/Branch Truce/recovery/v1/session" && exit 7'
[[ "$status" == 7 ]] || fail "expected the command's exit status 7, got $status: $output"
[[ -z $(ls -A "$scratch/tmp-clean") ]] || fail "private directories were not removed: $(ls -A "$scratch/tmp-clean")"
[[ -z $(ls -A "$scratch/data-clean") ]] || fail 'the stand-in data home changed during a clean run'
assertions=$((assertions + 1))

# A new path under the recovery root of XDG_DATA_HOME fails the run, even when the command succeeds.
mkdir "$scratch/tmp-xdg" "$scratch/data-xdg"
run_wrapper status output TMPDIR="$scratch/tmp-xdg" XDG_DATA_HOME="$scratch/data-xdg" LEAK_TO="$scratch/data-xdg" -- \
    bash -c 'mkdir -p "$LEAK_TO/Branch Truce/recovery/v1/leaked"'
[[ "$status" == 1 && "$output" == *"real recovery root $scratch/data-xdg/Branch Truce/recovery"* \
    && "$output" == *"/v1/leaked"* ]] || fail "XDG_DATA_HOME leak not reported (status $status): $output"
[[ -z $(ls -A "$scratch/tmp-xdg") ]] || fail 'private directories survived a failing run'
assertions=$((assertions + 1))

# Without XDG_DATA_HOME the recovery root comes from HOME/.local/share.
mkdir -p "$scratch/tmp-home" "$scratch/home/.local/share"
run_wrapper status output TMPDIR="$scratch/tmp-home" HOME="$scratch/home" XDG_DATA_HOME= LEAK_TO="$scratch/home/.local/share" -- \
    bash -c 'mkdir -p "$LEAK_TO/Branch Truce/recovery/v1/leaked"'
[[ "$status" == 1 && "$output" == *"$scratch/home/.local/share/Branch Truce/recovery"* ]] \
    || fail "HOME leak not reported (status $status): $output"
assertions=$((assertions + 1))

# Another process updating an existing journal is not a new path.
mkdir -p "$scratch/tmp-existing" "$scratch/data-existing/Branch Truce/recovery/v1/old"
: >"$scratch/data-existing/Branch Truce/recovery/v1/old/journal"
run_wrapper status output TMPDIR="$scratch/tmp-existing" XDG_DATA_HOME="$scratch/data-existing" LEAK_TO="$scratch/data-existing" -- \
    bash -c 'echo update >>"$LEAK_TO/Branch Truce/recovery/v1/old/journal"'
[[ "$status" == 0 ]] || fail "an update to an existing journal failed the run (status $status): $output"
assertions=$((assertions + 1))

# A terminated run still removes its private directories and stops the command.
mkdir "$scratch/tmp-term" "$scratch/data-term"
# CTest signals the process group, so the test does too.
TMPDIR="$scratch/tmp-term" XDG_DATA_HOME="$scratch/data-term" setsid "$wrapper" sleep 60 2>/dev/null &
pid=$!
for _ in $(seq 1 100); do
    [[ -n $(ls -A "$scratch/tmp-term") ]] && break
    sleep 0.05
done
[[ -n $(ls -A "$scratch/tmp-term") ]] || fail 'the wrapper never created its private directory'
kill -TERM -- "-$pid"
if wait "$pid"; then status=0; else status=$?; fi
[[ "$status" == 143 ]] || fail "expected exit 143 after SIGTERM, got $status"
[[ -z $(ls -A "$scratch/tmp-term") ]] || fail 'private directories survived SIGTERM'
assertions=$((assertions + 1))

printf '%s private XDG isolation assertions passed\n' "$assertions"
