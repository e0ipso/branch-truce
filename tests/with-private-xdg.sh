#!/usr/bin/env bash
# Run a test command with private HOME, XDG and runtime directories, remove them afterwards,
# and fail when the run left a trace in the caller's real directories:
#   - a new path under the recovery root, <data home>/Branch Truce/recovery, where the data home
#     is the caller's XDG_DATA_HOME when absolute, else $HOME/.local/share;
#   - a new path in an entry of the config or cache home whose name contains "branch" and
#     "truce" in any case (the homes are XDG_CONFIG_HOME and XDG_CACHE_HOME when absolute, else
#     $HOME/.config and $HOME/.cache);
#   - a change to the caller's global Git configuration, $HOME/.gitconfig or <config home>/git/config.
#
# Usage: with-private-xdg.sh COMMAND [ARGUMENT...]
#
# Only new paths count, so another process reading or updating an existing journal does not
# fail the run. The private HOME reaches only code that reads the environment: a process that
# asks the password database for its home still sees the real one, which the Git configuration
# check catches when it writes there.
set -uo pipefail

(($# > 0)) || { echo 'Usage: with-private-xdg.sh COMMAND [ARGUMENT...]' >&2; exit 2; }

real_home=${HOME:?HOME must be set}
absolute_or() { [[ "$1" == /* ]] && printf '%s' "$1" || printf '%s' "$2"; }
data_home=$(absolute_or "${XDG_DATA_HOME:-}" "$real_home/.local/share")
config_home=$(absolute_or "${XDG_CONFIG_HOME:-}" "$real_home/.config")
cache_home=$(absolute_or "${XDG_CACHE_HOME:-}" "$real_home/.cache")
real_root="$data_home/Branch Truce/recovery"
git_configs=("$real_home/.gitconfig" "$config_home/git/config")

list_paths() {
    {
        [[ -d "$real_root" ]] && find "$real_root" -mindepth 1 -print
        for home in "$config_home" "$cache_home"; do
            [[ -d "$home" ]] || continue
            while IFS= read -r -d '' entry; do
                find "$entry" -print
            done < <(find "$home" -mindepth 1 -maxdepth 1 -iname '*branch*truce*' -print0)
        done
    } | LC_ALL=C sort
}

git_config_state() {
    local file
    for file in "${git_configs[@]}"; do
        if [[ -f "$file" ]]; then
            printf '%s %s\n' "$file" "$(cksum <"$file")"
        else
            printf '%s absent\n' "$file"
        fi
    done
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
git_before=$(git_config_state)

env HOME="$private" XDG_DATA_HOME="$private/data" XDG_CONFIG_HOME="$private/config" \
    XDG_CACHE_HOME="$private/cache" XDG_RUNTIME_DIR="$private/runtime" "$@"
status=$?

leaked=0
after=$(list_paths)
created=$(comm -13 <(printf '%s\n' "$before") <(printf '%s\n' "$after"))
if [[ -n "$created" ]]; then
    printf 'This run created paths under the real recovery root %s or the app entries of %s and %s:\n%s\n' \
        "$real_root" "$config_home" "$cache_home" "$created" >&2
    leaked=1
fi
git_after=$(git_config_state)
if [[ "$git_after" != "$git_before" ]]; then
    printf 'This run changed the real global Git configuration:\n%s\n' \
        "$(comm -13 <(printf '%s\n' "$git_before") <(printf '%s\n' "$git_after"))" >&2
    leaked=1
fi
((leaked == 0)) || exit 1
exit "$status"
