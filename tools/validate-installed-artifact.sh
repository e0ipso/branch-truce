#!/usr/bin/env bash
# Installs a branch-truce DEB in a clean container of a validation target and
# exercises the installed application with the source and build trees absent.
#
#   tools/validate-installed-artifact.sh --target <target> --package DEB --output DIR [--wayland]
#
# <target> is a row of packaging/validation-targets.tsv whose release format is
# a DEB, such as kubuntu or tuxedo. The script builds that target image, or
# reuses it from the engine's layer cache. The container needs network access
# for the package lists. Nothing is installed on the host.
# docs/platform-validation.md records the results and what this cannot show:
# the container is not a desktop. packaging/scripts/check-package.sh makes the
# payload, metadata, icon, hardening, private Qt ABI and removal checks.
#
# Each step below is a step of tools/installed-checks.sh, run in the container.
# Steps without a display (QT_QPA_PLATFORM=offscreen):
#   install         apt install --no-install-recommends of the DEB, the packages
#                   it added with their versions, then the validation tools,
#                   which are not part of the closure
#   closure         every NEEDED library of the binary and every QML module
#                   src/ui imports comes from a package Depends names itself;
#                   the RUNPATH and the highest required symbol versions
#   layout          no source tree, build tree or loose QML file on the system
#   cli             --version, --help and --smoke-test on a disposable merge
#                   conflict; every QML module and Qt plugin loaded from the
#                   file system belongs to an installed package
#   desktop         gio launch of the installed desktop entry opens the session
#   mergetool-leave real `git mergetool` with the installed launcher; the GUI it
#                   starts accepts the path; when the GUI dies, Git gets MERGED
#                   back unchanged and the path stays unmerged
#
# With --wayland, the container also displays on the host's Wayland socket
# ($XDG_RUNTIME_DIR/$WAYLAND_DISPLAY). The host needs a sway session (SWAYSOCK
# or $XDG_RUNTIME_DIR/sway-ipc.*.sock), swaymsg, jq and grim. The app runs
# with the session variables of a Plasma session and plasma-integration, so
# kdeglobals selects the colour scheme:
#   wayland-minimal the installed closure alone, which has qt6-wayland, starts
#                   on the Wayland platform
#   session         installs plasma-integration, as Plasma has
#   window LABEL    one desktop-entry launch per QT_SCALE_FACTOR (1, 1.5 and
#                   2) and Breeze scheme (BreezeLight, BreezeDark), labelled
#                   scale<factor>-<scheme>: the window's app_id and size from
#                   sway, an AT-SPI readiness check and a grim screenshot
#                   DIR/<package>-LABEL.png
#   editor-input    AT-SPI focus on the editable result, then text typed
#                   through the Wayland virtual keyboard (wtype), read back
#                   over AT-SPI; DIR/<package>.atspi has the accessible tree
#   mergetool-resolve  `git mergetool` again; the result is typed, Mark
#                   resolved and Confirm resolution are pressed over AT-SPI,
#                   and Git stages the reviewed bytes
#
# The report is printed as the steps run and saved whole, when the run ends,
# as DIR/<package>.validation; DIR also receives the screenshots and the
# AT-SPI tree. A failed step or host check is counted, and the run goes on.
# CONTAINER_ENGINE defaults to docker. The container is removed on exit; the
# image stays. Exit status: 0 every check passed, 1 a check failed, 2 usage
# error.
#
# Sourcing the file defines its functions and runs nothing, so tests can call
# them.

app_id=com.canpicasoft.branch-truce
wayland_socket=/run/bt-wayland/wayland-0
# The engine command that runs one installed-checks.sh step; host_main sets it.
inside_command=()

usage() { echo "Usage: $0 --target <target> --package DEB --output DIR [--wayland]"; }

host_main() {
    local root target package output socket='' engine name image fixture container cleanup
    root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
    source "$root/packaging/scripts/cli.sh"
    source "$root/packaging/scripts/formats.sh"
    source "$root/tools/lib/targets.sh"
    source "$root/tools/lib/report.sh"
    local -A options=()
    cli_parse options - 'target=! package=! output=! wayland' "$@" || { cli_usage $? usage; return; }
    target=${options[target]} package=${options[package]} output=${options[output]}
    [[ -f "$package" ]] || { echo "error: $package is not a file" >&2; usage >&2; return 2; }
    deb_target "$target" || { usage >&2; return 2; }
    if [[ -n "${options[wayland]-}" ]]; then
        local tool
        for tool in swaymsg jq grim; do
            command -v "$tool" >/dev/null || { echo "--wayland needs $tool on the host" >&2; return 1; }
        done
        [[ -n "${WAYLAND_DISPLAY:-}" && -n "${XDG_RUNTIME_DIR:-}" ]] \
            || { echo "--wayland needs WAYLAND_DISPLAY and XDG_RUNTIME_DIR" >&2; return 1; }
        socket=$WAYLAND_DISPLAY
        [[ "$socket" == /* ]] || socket=$XDG_RUNTIME_DIR/$socket
        socket=$(readlink -f -- "$socket")
        [[ -S "$socket" ]] || { echo "No Wayland socket at $socket" >&2; return 1; }
        if [[ -z "${SWAYSOCK:-}" ]]; then
            local found=("$XDG_RUNTIME_DIR"/sway-ipc.*.sock)
            [[ -S "${found[0]}" ]] || { echo "--wayland needs SWAYSOCK" >&2; return 1; }
            export SWAYSOCK=${found[0]}
        fi
    fi
    engine=${CONTAINER_ENGINE:-docker}
    package=$(cd -- "$(dirname -- "$package")" && pwd -P)/$(basename -- "$package")
    name=$(basename -- "$package" .deb)
    mkdir -p -- "$output" && output=$(cd -- "$output" && pwd -P) || return 1
    image=$(target_image "$engine" "$root" "$target" "${BRANCH_TRUCE_KDE_CI_REFRESH:-}") || return 1

    fixture=$(mktemp -d "${TMPDIR:-/tmp}/bt-validate.XXXXXXXX") || return 1
    container=bt-validate-$target-$$
    printf -v cleanup '%q rm --force %q >/dev/null 2>&1; rm -rf -- %q' "$engine" "$container" "$fixture"
    # shellcheck disable=SC2064
    trap "$cleanup" EXIT
    # The container sees the step script, its libraries and the QML modules the
    # application imports, from the sources it was built from.
    mkdir -- "$fixture/lib" \
        && cp -- "$root/tools/installed-checks.sh" "$root/tools/atspi-probe.py" "$fixture/" \
        && cp -- "$root/tools/lib/fixture.sh" "$root/tools/lib/report.sh" "$fixture/lib/" \
        && sed -n 's/^import \([A-Za-z][A-Za-z0-9.]*\).*/\1/p' "$root"/src/ui/*.qml | LC_ALL=C sort -u > "$fixture/imports.txt" \
        || return 1
    local mounts=(--volume "$package:/artifact/$name.deb:ro" --volume "$fixture:/validate:ro")
    [[ -z "$socket" ]] || mounts+=(--volume "$socket:$wayland_socket")
    "$engine" run --detach --init --name "$container" "${mounts[@]}" "$image" sleep infinity >/dev/null \
        || { echo "error: the $target container did not start" >&2; return 1; }
    inside_command=("$engine" exec "$container" /bin/bash /validate/installed-checks.sh)

    local report=$output/$name.validation partial=$output/.$name.validation.partial
    run_steps "$package" "$name" "$socket" "$output/$name" > >(tee -- "$partial") 2>&1
    wait "$!"
    ((${failures:-0} == 0)) || echo "Validation failed: $failures failed checks" | tee -a -- "$partial"
    mv -- "$partial" "$report" || return 1
    echo "Report: $report"
    ((${failures:-0} == 0))
}

# deb_target TARGET: TARGET is a validation target that takes a DEB.
deb_target() {
    local format family
    format=$(target_field "$1" format) && family=$(format_field "$format" family) || return 2
    [[ "$family" == deb ]] || { echo "error: $1 takes the $format package, not a DEB" >&2; return 2; }
}

# inside STEP [ARG...]: one installed-checks.sh step in the container. A step
# that fails, or stops early, is a failed check.
inside() {
    local status=0
    "${inside_command[@]}" "$@" || status=$?
    ((status == 0)) || fail "step $1 exited $status"
    return "$status"
}

# run_steps PACKAGE NAME SOCKET PREFIX: the report of every step. SOCKET is the
# host's Wayland socket, or empty without --wayland; screenshots and the AT-SPI
# tree are written to PREFIX-LABEL.png and PREFIX.atspi.
run_steps() {
    local package=$1 name=$2 socket=$3 prefix=$4 step
    printf '== Artifact\nfile %s.deb\nsha256 %s\nhost kernel %s\n' "$name" \
        "$(sha256sum < "$package" | cut -d' ' -f1)" "$(uname -r)"
    # Nothing else can run without the package, so the result follows at once.
    if inside install "/artifact/$name.deb"; then
        for step in closure layout cli desktop mergetool-leave; do inside "$step"; done
        if [[ -z "$socket" ]]; then
            printf '\n== Wayland session\nSKIP not requested; Wayland, scaling, theme, input and AT-SPI not exercised\n'
        else
            wayland_steps "$socket" "$prefix"
        fi
    fi
    printf '\n== Result\n'
}

# wayland_steps SOCKET PREFIX
wayland_steps() {
    local socket=$1 prefix=$2 scale scheme
    step 'Wayland session'
    echo "socket $socket"
    echo "compositor $(swaymsg -t get_version | jq -r .human_readable | sed 's/^/sway /')"
    swaymsg -t get_outputs | jq -r '.[] | "output \(.name) \(.current_mode.width)x\(.current_mode.height) scale \(.scale)"'
    if [[ -n "$(window_json)" ]]; then
        fail "a $app_id window is already open on this session"
        return
    fi
    inside wayland-minimal
    inside session
    for scale in 1 1.5 2; do
        for scheme in BreezeLight BreezeDark; do
            window_run "$prefix" "scale$scale-$scheme" "$scale" "$scheme"
        done
    done
    step 'Editor input and accessibility (scale 1, BreezeLight)'
    inside gui-start 1 BreezeLight desktop
    if wait_window present 60 && inside wait-ready; then
        "${inside_command[@]}" atspi-dump > "$prefix.atspi" || fail "AT-SPI tree"
        echo "AT-SPI tree: $prefix.atspi ($(wc -l < "$prefix.atspi") lines)"
        inside atspi-check
        inside editor-input
        shoot "$prefix" editor-input
    else
        fail "window for editor input did not become ready"
    fi
    inside gui-stop
    wait_window absent 30 || fail "window still open after the app stopped"
    step 'Mergetool resolve over Wayland'
    inside mergetool-start
    if wait_window present 60; then
        inside mergetool-resolve
        shoot "$prefix" mergetool-resolved
    else
        fail "the launcher-started window did not appear"
    fi
    inside gui-stop
    wait_window absent 30 || fail "window still open after the app stopped"
}

window_json() {
    swaymsg -t get_tree | jq -c --arg id "$app_id" \
        '[.. | objects | select(.app_id? == $id) | {app_id, name, shell, rect}] | .[0] // empty'
}

# wait_window present|absent SECONDS
wait_window() {
    local want=$1 deadline=$((SECONDS + $2)) json
    while :; do
        json=$(window_json)
        if [[ "$want" == present && -n "$json" ]]; then
            echo "window $json"
            return 0
        fi
        [[ "$want" == absent && -z "$json" ]] && return 0
        ((SECONDS < deadline)) || { echo "window not $want after $2 s"; return 1; }
        sleep 0.5
    done
}

# shoot PREFIX LABEL: a screenshot of the app's window as PREFIX-LABEL.png.
shoot() {
    local file=$1-$2.png geometry
    geometry=$(window_json | jq -r '.rect | "\(.x),\(.y) \(.width)x\(.height)"')
    if [[ -n "$geometry" ]] && grim -g "$geometry" "$file"; then
        echo "screenshot $file ($geometry)"
    else
        fail "screenshot $2"
    fi
}

# window_run PREFIX LABEL SCALE SCHEME
window_run() {
    local prefix=$1 label=$2 scale=$3 scheme=$4
    step "Window $label"
    inside gui-start "$scale" "$scheme" desktop
    if ! wait_window present 60; then
        fail "$label: no window"
    elif inside wait-ready; then
        pass "$label: desktop-entry launch shows the merge session"
        shoot "$prefix" "$label"
    else
        fail "$label: session not ready"
    fi
    inside gui-stop
    wait_window absent 30 || fail "$label: window still open after the app stopped"
}

[[ "${BASH_SOURCE[0]}" == "$0" ]] || return 0
set -uo pipefail
host_main "$@"
