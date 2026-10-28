#!/usr/bin/env bash
# Opens a development build on a disposable merge conflict on the current sway
# session and captures its window in the Breeze light and dark colour schemes.
#
#   tools/preview-app.sh [APP]
#
# APP defaults to build/desktop/src/app/branch-truce in this checkout. Each
# capture starts APP on a fresh repository stopped on a merge of side into main
# with three conflicting files: tracked.txt (text), notes.md (Markdown) and
# image.bin (binary). The app gets a private HOME and XDG directories, no D-Bus
# session and a kdeglobals for the scheme, and KDE_COLOR_SCHEME_PATH names the
# scheme file. Its window goes fullscreen, so other windows on the workspace do
# not change its size. The capture waits for the session's recovery journal,
# which the app writes once the files have loaded, then PREVIEW_SETTLE seconds
# more. The script prints one PNG path per scheme on stdout. Without
# PREVIEW_FILE the app selects its first file, image.bin.
#
# Environment:
#   PREVIEW_FILE    fixture file to select at launch: tracked.txt, notes.md or
#                   image.bin, passed hex-encoded through the app's
#                   --mergetool-path option
#   PREVIEW_OUTPUT  directory for the PNGs; default a new
#                   ${TMPDIR:-/tmp}/bt-preview-shots.* directory, which is kept
#   PREVIEW_SETTLE  seconds between the journal appearing and the capture;
#                   default 1
#   PREVIEW_TIMEOUT seconds each launch may take to show its window and load
#                   its files; default 180. An idle host needs under 15, but
#                   the app renders in software here and a busy host slows it
#                   several times over.
#
# Needs WAYLAND_DISPLAY and XDG_RUNTIME_DIR from a sway session (SWAYSOCK or
# $XDG_RUNTIME_DIR/sway-ipc.*.sock), plus git, swaymsg, jq and grim. The
# fixture, home and logs live in one ${TMPDIR:-/tmp}/bt-preview.* directory,
# removed on exit. The checkout and $HOME are not written. Only the app this
# script started is stopped.
set -euo pipefail

schemes=(BreezeLight BreezeDark)
fixture_files=(tracked.txt notes.md image.bin)

usage() { echo "Usage: $0 [APP]" >&2; exit 2; }
die() { echo "preview-app: $*" >&2; exit 1; }

(($# <= 1)) || usage
[[ "${1:-}" != -* ]] || usage
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
app=${1:-$root/build/desktop/src/app/branch-truce}
[[ -x "$app" && -f "$app" ]] || die "no executable app at $app; build it or pass its path"
app=$(cd -- "$(dirname -- "$app")" && pwd -P)/$(basename -- "$app")
settle=${PREVIEW_SETTLE:-1}
[[ "$settle" =~ ^[0-9]+(\.[0-9]+)?$ ]] || die "PREVIEW_SETTLE must be a number of seconds"
timeout=${PREVIEW_TIMEOUT:-180}
[[ "$timeout" =~ ^[1-9][0-9]*$ ]] || die "PREVIEW_TIMEOUT must be a whole number of seconds"

for tool in git swaymsg jq grim od; do
    command -v "$tool" >/dev/null || die "needs $tool"
done
[[ -n "${WAYLAND_DISPLAY:-}" && -n "${XDG_RUNTIME_DIR:-}" ]] || die "needs WAYLAND_DISPLAY and XDG_RUNTIME_DIR"
socket=$WAYLAND_DISPLAY
[[ "$socket" == /* ]] || socket=$XDG_RUNTIME_DIR/$socket
socket=$(readlink -f -- "$socket")
[[ -S "$socket" ]] || die "no Wayland socket at $socket"
if [[ -z "${SWAYSOCK:-}" ]]; then
    found=("$XDG_RUNTIME_DIR"/sway-ipc.*.sock)
    [[ -S "${found[0]}" ]] || die "needs SWAYSOCK"
    export SWAYSOCK=${found[0]}
fi
for scheme in "${schemes[@]}"; do
    [[ -f "/usr/share/color-schemes/$scheme.colors" ]] || die "no /usr/share/color-schemes/$scheme.colors"
done
select_args=()
if [[ -n "${PREVIEW_FILE:-}" ]]; then
    # The app falls back to its first file when the path is not in the session,
    # which would label a capture of the wrong file.
    [[ " ${fixture_files[*]} " == *" $PREVIEW_FILE "* ]] \
        || die "PREVIEW_FILE must be one of: ${fixture_files[*]}"
    select_args=(--mergetool-path "$(printf '%s' "$PREVIEW_FILE" | od -An -v -tx1 | tr -d ' \n')")
fi

made_output=0
if [[ -n "${PREVIEW_OUTPUT:-}" ]]; then
    mkdir -p -- "$PREVIEW_OUTPUT"
    output=$(cd -- "$PREVIEW_OUTPUT" && pwd -P)
else
    output=$(mktemp -d "${TMPDIR:-/tmp}/bt-preview-shots.XXXXXXXX")
    made_output=1
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/bt-preview.XXXXXXXX")
home=$work/home
repo=$work/repo
app_pid=
scheme=
log=

stop_app() {
    [[ -n "$app_pid" ]] || return 0
    local tries=0
    kill -TERM "$app_pid" 2>/dev/null || true
    while kill -0 "$app_pid" 2>/dev/null && ((tries++ < 50)); do sleep 0.1; done
    kill -KILL "$app_pid" 2>/dev/null || true
    wait "$app_pid" 2>/dev/null || true
    app_pid=
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    stop_app
    rm -rf -- "$work"
    # A run that captured nothing leaves no empty directory behind.
    ((made_output == 0)) || rmdir -- "$output" 2>/dev/null || true
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
cd /

source "$root/tools/merge-fixture.sh"

# The isolated environment of the fixture's Git commands and the app.
base_env=(
    "PATH=/usr/bin:/bin" "HOME=$home" "TMPDIR=$home/tmp" "LC_ALL=C.UTF-8"
    "XDG_DATA_HOME=$home/data" "XDG_CONFIG_HOME=$home/config" "XDG_CACHE_HOME=$home/cache"
    "XDG_RUNTIME_DIR=$home/runtime" "GIT_CONFIG_NOSYSTEM=1" "GIT_CONFIG_GLOBAL=$home/gitconfig"
    "GIT_TERMINAL_PROMPT=0"
)
isolated() { (cd "$repo" && env -i "${base_env[@]}" "$@"); }

has_window() {
    swaymsg -t get_tree | jq -e --argjson pid "$app_pid" 'any(.. | objects; .pid? == $pid)' >/dev/null
}

has_journal() { compgen -G "$home/data/Branch Truce/recovery/v1/*/journal" >/dev/null; }

# wait_for WHAT CHECK DEADLINE: true once CHECK succeeds while the app runs.
wait_for() {
    until "$2"; do
        if ! kill -0 "$app_pid" 2>/dev/null; then
            tail -n 20 -- "$log" >&2
            die "$scheme: the app exited while waiting for $1"
        fi
        if ((SECONDS >= $3)); then
            tail -n 20 -- "$log" >&2
            die "$scheme: waited $timeout s for $1; raise PREVIEW_TIMEOUT on a busy host"
        fi
        sleep 0.2
    done
}

window_rect() {
    swaymsg -t get_tree | jq -r --argjson pid "$app_pid" \
        '[.. | objects | select(.pid? == $pid and .visible? == true) | .rect | "\(.x),\(.y) \(.width)x\(.height)"] | .[0] // empty'
}

# capture SCHEME: one launch on a fresh fixture, one PNG.
capture() {
    scheme=$1
    local colors=/usr/share/color-schemes/$scheme.colors file=$output/branch-truce-$scheme.png
    log=$work/$scheme.log
    local deadline rect=
    make_merge_fixture "$app" "${fixture_files[@]}" || die "the fixture merge did not stop on a conflict"
    { cat "$colors"; printf '\n[General]\nColorScheme=%s\n' "$scheme"; } > "$home/config/kdeglobals"
    env -i "${base_env[@]}" "WAYLAND_DISPLAY=$socket" "QT_QPA_PLATFORM=wayland" \
        "XDG_SESSION_TYPE=wayland" "XDG_CURRENT_DESKTOP=KDE" "KDE_SESSION_VERSION=6" \
        "KDE_FULL_SESSION=true" "KDE_COLOR_SCHEME_PATH=$colors" \
        "$app" "${select_args[@]}" "$repo" > "$log" 2>&1 < /dev/null &
    app_pid=$!
    deadline=$((SECONDS + timeout))
    wait_for "its window" has_window "$deadline"
    swaymsg "[pid=$app_pid] fullscreen enable, focus" >/dev/null
    wait_for "the loaded session" has_journal "$deadline"
    sleep "$settle"
    rect=$(window_rect)
    [[ -n "$rect" ]] || die "$scheme: the window is not visible"
    grim -g "$rect" "$file" || die "$scheme: grim failed"
    [[ -s "$file" ]] || die "$scheme: empty capture $file"
    stop_app
    printf '%s\n' "$file"
}

for scheme in "${schemes[@]}"; do
    capture "$scheme"
done
echo "Captures are in $output; remove it when done." >&2
