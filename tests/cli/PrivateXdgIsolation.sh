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

# A new path in an app entry of the config or cache home fails the run.
for kind in CONFIG CACHE; do
    mkdir -p "$scratch/tmp-$kind" "$scratch/home-$kind/existing"
    run_wrapper status output TMPDIR="$scratch/tmp-$kind" "XDG_${kind}_HOME=$scratch/home-$kind" \
        LEAK_TO="$scratch/home-$kind" -- bash -c 'mkdir -p "$LEAK_TO/branch-truce" && : >"$LEAK_TO/branch-truce/state"'
    [[ "$status" == 1 && "$output" == *"$scratch/home-$kind/branch-truce/state"* ]] \
        || fail "XDG_${kind}_HOME leak not reported (status $status): $output"
    assertions=$((assertions + 1))
done

# Another tool's entry in the config home is not the app's.
mkdir -p "$scratch/tmp-other" "$scratch/config-other"
run_wrapper status output TMPDIR="$scratch/tmp-other" XDG_CONFIG_HOME="$scratch/config-other" \
    LEAK_TO="$scratch/config-other" -- bash -c 'mkdir -p "$LEAK_TO/other-tool"'
[[ "$status" == 0 ]] || fail "another tool's config entry failed the run (status $status): $output"
assertions=$((assertions + 1))

# A change to the caller's global Git configuration fails the run, even when the command succeeds.
mkdir -p "$scratch/tmp-git" "$scratch/git-home"
printf '[user]\n\tname = Someone\n' >"$scratch/git-home/.gitconfig"
run_wrapper status output TMPDIR="$scratch/tmp-git" HOME="$scratch/git-home" XDG_CONFIG_HOME= \
    LEAK_TO="$scratch/git-home" -- bash -c 'printf "[core]\n\tautocrlf = true\n" >>"$LEAK_TO/.gitconfig"'
[[ "$status" == 1 && "$output" == *'changed the real global Git configuration'* \
    && "$output" == *"$scratch/git-home/.gitconfig"* ]] || fail "Git configuration change not reported (status $status): $output"
assertions=$((assertions + 1))

# A terminated run still removes its private directories and stops the command.
mkdir "$scratch/tmp-term" "$scratch/data-term"
# CTest signals the process group, so the test does too.
# Directory creation precedes the wrapper's traps and child launch. The child reports its PID
# through a FIFO, so TERM cannot land before that child joins the group. Opening the FIFO read
# and write never blocks, and the read waits for the PID without polling.
ready="$scratch/term-ready"
mkfifo "$ready"
exec 3<>"$ready"
TMPDIR="$scratch/tmp-term" XDG_DATA_HOME="$scratch/data-term" setsid "$wrapper" \
    bash -c 'printf "%s\n" "$$" >"$1"; exec sleep 60' bash "$ready" 2>/dev/null &
pid=$!
read -r -t 10 child <&3 || fail 'the wrapper never started its command'
exec 3<&-
[[ -n $(ls -A "$scratch/tmp-term") ]] || fail 'the wrapper never created its private directory'
kill -TERM -- "-$pid"
if wait "$pid"; then status=0; else status=$?; fi
[[ "$status" == 143 ]] || fail "expected exit 143 after SIGTERM, got $status"
[[ -z $(ls -A "$scratch/tmp-term") ]] || fail 'private directories survived SIGTERM'
! kill -0 "$child" 2>/dev/null || fail 'the wrapped command survived SIGTERM'
assertions=$((assertions + 1))

printf '%s private XDG isolation assertions passed\n' "$assertions"
