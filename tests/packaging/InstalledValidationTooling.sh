#!/usr/bin/env bash
# Exercise the host side of tools/validate-installed-artifact.sh through
# CONTAINER_ENGINE set to a stub engine, with stub swaymsg and grim for
# --wayland, and the pure parts of its container side and tools/atspi-probe.py
# with synthetic input. No Docker, network, root, display, D-Bus or installed
# package is used. Everything is written under this test's temporary
# directory, which is removed on exit.
#
# Needs jq and python3. Without either it runs what it can and exits 77, which
# CTest reports as skipped rather than passed.
#
# Not covered here: the container steps themselves (apt, dpkg, gio, git
# mergetool, wtype on a real compositor), which use the fixed paths /tmp/bt-home
# and /tmp/bt-repo; and wait_window's 60 and 30 second timeouts, which have no
# override.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
work=$(mktemp -d "${TMPDIR:-/tmp}/bt-validation-tooling.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT
# The script prints physical paths.
work=$(cd -- "$work" && pwd -P)
cd -- "$work"
export TMPDIR=$work/tmp
mkdir -p -- "$TMPDIR" "$work/stubs" "$work/sys" "$work/state" "$work/xdg"
script=$root/tools/validate-installed-artifact.sh
probe=$root/tools/atspi-probe.py
assertions=0
output=
status=0
missing=()

fail() { printf 'FAIL %s\n' "$*" >&2; printf 'Last output (exit %s):\n%s\n' "$status" "$output" >&2; exit 1; }
pass() { assertions=$((assertions + 1)); }

jq_path=$(command -v jq || true)
python=$(command -v python3 || true)
[[ -n "$jq_path" ]] || missing+=(jq)
[[ -n "$python" ]] || missing+=(python3)

# A PATH with only the commands the host side uses, so the host's own sway,
# grim or Docker can never be reached. Stubs come first.
for tool in bash env dirname basename mkdir readlink sha256sum cut uname grep tee cp mktemp rm sed wc \
    sleep cat find sort ls head tr; do
    found=$(command -v "$tool") || fail "test needs $tool"
    ln -s -- "$found" "$work/sys/$tool"
done
[[ -z "$jq_path" ]] || ln -s -- "$jq_path" "$work/sys/jq"
stub_path=$work/stubs:$work/sys

# Stub engine: logs each argv; `run` keeps a copy of the /validate fixture;
# `exec` plays one container step, failing those named in STUB_FAIL_STEPS.
cat > "$work/stubs/engine" <<'ENGINE'
#!/usr/bin/env bash
set -euo pipefail
state=$STUB_STATE
{ printf '%q' "$1"; printf ' %q' "${@:2}"; printf '\n'; } >> "$state/engine.log"
case "$1" in
    image) exit "${STUB_IMAGE_STATUS:-0}" ;;
    run)
        while (($# > 0)); do
            case "$1" in
                --volume)
                    case "$2" in
                        *:/validate:ro)
                            printf '%s\n' "${2%:/validate:ro}" > "$state/fixture"
                            cp -a -- "${2%:/validate:ro}" "$state/fixture-copy" ;;
                    esac
                    shift 2 ;;
                *) shift ;;
            esac
        done ;;
    exec)
        step=$6
        case "$step" in
            gui-start|mergetool-start) : > "$state/window" ;;
            gui-stop) rm -f -- "$state/window" ;;
        esac
        if [[ " ${STUB_FAIL_STEPS:-} " == *" $step "* ]]; then
            printf 'stub %s failed\n' "$step"
            exit 3
        fi
        if [[ "$step" == atspi-dump ]]; then
            printf "application 'Branch Truce'\n  push-button | Mark resolved | focusable,enabled,showing\n"
        else
            printf 'PASS stub %s\n' "${*:6}"
        fi ;;
esac
ENGINE
# swaymsg answers from a fixed session; the window exists while the stub engine
# has a GUI running, or from the start with STUB_WINDOW_OPEN=1.
cat > "$work/stubs/swaymsg" <<'SWAYMSG'
#!/usr/bin/env bash
printf '%s SWAYSOCK=%s\n' "$*" "${SWAYSOCK:-}" >> "$STUB_STATE/swaymsg.log"
window='{"app_id":"local.branch_truce.BranchTruce","name":"Branch Truce","shell":"xdg_shell","rect":{"x":10,"y":20,"width":900,"height":500}}'
other='{"app_id":"org.kde.konsole","name":"Konsole","shell":"xdg_shell","rect":{"x":0,"y":0,"width":640,"height":480}}'
case "$2" in
    get_version) echo '{"human_readable":"1.10-stub"}' ;;
    get_outputs) echo '[{"name":"HEADLESS-1","current_mode":{"width":1920,"height":1080},"scale":1.5}]' ;;
    get_tree)
        if [[ -e "$STUB_STATE/window" || "${STUB_WINDOW_OPEN:-0}" == 1 ]]; then
            printf '{"nodes":[{"nodes":[%s,%s]}]}\n' "$other" "$window"
        else
            printf '{"nodes":[{"nodes":[%s]}]}\n' "$other"
        fi ;;
esac
SWAYMSG
cat > "$work/stubs/grim" <<'GRIM'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$STUB_STATE/grim.log"
[[ "${STUB_GRIM_STATUS:-0}" == 0 ]] || exit "$STUB_GRIM_STATUS"
printf 'png\n' > "$3"
GRIM
cat > "$work/stubs/wtype" <<'WTYPE'
#!/usr/bin/env bash
printf 'WAYLAND_DISPLAY=%s %s\n' "$WAYLAND_DISPLAY" "$*" >> "$STUB_STATE/wtype.log"
exit "${STUB_WTYPE_STATUS:-0}"
WTYPE
chmod +x "$work/stubs/engine" "$work/stubs/swaymsg" "$work/stubs/grim" "$work/stubs/wtype"

# invoke [NAME=VALUE...] -- COMMAND...: runs COMMAND with the stub PATH and engine
# and none of this session's Wayland variables, keeping its output and status.
invoke() {
    local vars=()
    while [[ "$1" != -- ]]; do vars+=("$1"); shift; done
    shift
    rm -rf -- "$work/state" && mkdir -p -- "$work/state"
    if output=$(env -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR -u SWAYSOCK -u STUB_FAIL_STEPS \
        "PATH=$stub_path" "CONTAINER_ENGINE=$work/stubs/engine" "STUB_STATE=$work/state" \
        "${vars[@]}" "$@" 2>&1); then status=0; else status=$?; fi
}
expect_status() {
    [[ "$status" == "$1" ]] || fail "expected exit $1"
    pass
}
expect_output() {
    [[ "$output" == *"$1"* ]] || fail "output lacks: $1"
    pass
}
expect_no_output() {
    [[ "$output" != *"$1"* ]] || fail "output has: $1"
    pass
}
expect_engine_calls() {
    local calls=0
    [[ ! -e "$work/state/engine.log" ]] || calls=$(wc -l < "$work/state/engine.log")
    [[ "$calls" == "$1" ]] || fail "expected $1 engine calls, got $calls"
    pass
}
expect_rejected() {
    invoke -- "$@"
    expect_status 2
    expect_output 'Usage:'
    expect_engine_calls 0
}
# The script removes its fixture under TMPDIR on every exit path.
expect_no_fixture() {
    [[ -z "$(ls -A -- "$TMPDIR")" ]] || fail "fixtures left in TMPDIR: $(ls -A -- "$TMPDIR")"
    pass
}
expect_files() {
    local actual
    actual=$(cd -- "$1" && LC_ALL=C ls -A | tr '\n' ' ')
    [[ "$actual" == "$2" ]] || fail "files in $1: expected '$2', got '$actual'"
    pass
}
# The container step names and arguments, in order, from the engine log.
steps() { sed -n 's/^exec bt-validate-[a-z]*-[0-9]* \/bin\/bash \/validate\/validate-installed-artifact\.sh --in-container //p' \
    "$work/state/engine.log" | tr '\n' ';'; }
# Binds a Unix socket at DIR/NAME. A relative bind stays within the socket path
# limit however long TMPDIR is.
make_socket() {
    "$python" -I -c 'import os, socket, sys; os.chdir(sys.argv[1]); socket.socket(socket.AF_UNIX).bind(sys.argv[2])' "$1" "$2"
}

mkdir -p -- "$work/debs"
deb_name=branch-truce_1.2.3+kubuntu26.04_amd64
deb=$work/debs/$deb_name.deb
printf 'stub deb\n' > "$deb"
deb_sha=$(sha256sum < "$deb" | cut -d' ' -f1)

# --- argument handling: usage errors exit 2 before the engine runs
expect_rejected "$script"
expect_rejected "$script" --target kubuntu --package "$deb"
expect_rejected "$script" --target kubuntu --output "$work/out"
expect_rejected "$script" --package "$deb" --output "$work/out"
expect_rejected "$script" --target
expect_rejected "$script" --target kubuntu --package
expect_rejected "$script" --target kubuntu --package "$deb" --output
expect_rejected "$script" --target kubuntu --package "$deb" --output "$work/out" --extra
expect_rejected "$script" --target kubuntu --package "$work/debs/missing.deb" --output "$work/out"
# An unknown target is rejected before the output directory exists.
expect_rejected "$script" --target debian --package "$deb" --output "$work/out"
[[ ! -e "$work/out" ]] || fail "output directory created for rejected arguments"
pass
# The container half also rejects an unknown step.
expect_rejected "$script" --in-container bogus
expect_no_fixture

# --- a missing target image stops before any container runs
invoke STUB_IMAGE_STATUS=1 -- "$script" --target kubuntu --package "$deb" --output "$work/out"
expect_status 1
expect_output 'Missing image branch-truce-artifact:kubuntu-26.04-target; run tools/build-artifact.sh --target kubuntu first'
expect_engine_calls 1
[[ "$(cat "$work/state/engine.log")" == 'image inspect branch-truce-artifact:kubuntu-26.04-target' ]] \
    || fail "image query"
pass
[[ ! -e "$work/out/$deb_name.validation" ]] || fail "report written without an image"
pass
expect_no_fixture

# --- target selection and output layout without --wayland
# kubuntu: the relative package and output paths resolve; the output directory is created.
invoke -- "$script" --target kubuntu --package "debs/$deb_name.deb" --output reports/kubuntu
expect_status 0
report=$work/reports/kubuntu/$deb_name.validation
expect_output "Report: $report"
expect_files "$work/reports/kubuntu" "$deb_name.validation "
expected_report="== Artifact
file $deb_name.deb
sha256 $deb_sha
host kernel $(uname -r)
PASS stub install kubuntu26.04 /artifact/$deb_name.deb
PASS stub layout
PASS stub cli
PASS stub desktop
PASS stub mergetool-leave

== Wayland session
SKIP not requested; Wayland, scaling, theme, input and AT-SPI not exercised

== Result"
[[ "$(cat "$report")" == "$expected_report" ]] || fail "report: $(cat "$report")"
pass
[[ "$output" == "$expected_report"$'\n'"Report: $report" ]] || fail "terminal output is the report and its path"
pass
[[ "$(steps)" == "install kubuntu26.04 /artifact/$deb_name.deb;layout;cli;desktop;mergetool-leave;" ]] \
    || fail "container steps: $(steps)"
pass
# The container runs from the target image with the package and fixture read-only.
fixture=$(cat "$work/state/fixture")
[[ "$fixture" == "$TMPDIR"/bt-validate.* ]] || fail "fixture $fixture is not under TMPDIR"
pass
run_line=$(sed -n 2p "$work/state/engine.log")
[[ "$run_line" =~ ^run\ --detach\ --init\ --name\ (bt-validate-kubuntu-[0-9]+)\ --volume\ (.*)$ ]] \
    || fail "run line: $run_line"
container=${BASH_REMATCH[1]}
[[ "${BASH_REMATCH[2]}" == "$deb:/artifact/$deb_name.deb:ro --volume $fixture:/validate:ro branch-truce-artifact:kubuntu-26.04-target sleep infinity" ]] \
    || fail "run mounts and image: $run_line"
pass
# Each step runs in that container; the container is removed last.
[[ "$(grep -c "^exec $container /bin/bash /validate/validate-installed-artifact.sh --in-container " "$work/state/engine.log")" == 5 ]] \
    || fail "steps outside $container"
pass
[[ "$(tail -n 1 "$work/state/engine.log")" == "rm --force $container" ]] || fail "container not removed last"
pass
# The container gets this checkout's validation scripts and nothing else.
expect_files "$work/state/fixture-copy" 'artifact-baseline.sh atspi-probe.py merge-fixture.sh validate-installed-artifact.sh '
for file in artifact-baseline.sh atspi-probe.py merge-fixture.sh validate-installed-artifact.sh; do
    cmp -s -- "$root/tools/$file" "$work/state/fixture-copy/$file" || fail "fixture $file differs from tools/$file"
    pass
done
expect_no_fixture

# tuxedo selects its own base and image.
invoke -- "$script" --target tuxedo --package "$deb" --output "$work/reports/tuxedo"
expect_status 0
[[ "$(sed -n 1p "$work/state/engine.log")" == 'image inspect branch-truce-artifact:tuxedo-24.04-target' ]] \
    || fail "tuxedo image"
pass
[[ "$(sed -n 2p "$work/state/engine.log")" == *' --name bt-validate-tuxedo-'*' branch-truce-artifact:tuxedo-24.04-target sleep infinity' ]] \
    || fail "tuxedo container"
pass
[[ "$(steps)" == "install tuxedo24.04 /artifact/$deb_name.deb;layout;cli;desktop;mergetool-leave;" ]] \
    || fail "tuxedo steps: $(steps)"
pass
expect_no_fixture

# --- failure paths
# A failed step after install: the remaining steps still run and the run fails.
invoke STUB_FAIL_STEPS='cli desktop' -- "$script" --target kubuntu --package "$deb" --output "$work/reports/fail"
expect_status 1
report=$work/reports/fail/$deb_name.validation
[[ "$(steps)" == "install kubuntu26.04 /artifact/$deb_name.deb;layout;cli;desktop;mergetool-leave;" ]] \
    || fail "steps after a failure: $(steps)"
pass
grep -qx 'FAIL step cli exited 3' "$report" && grep -qx 'FAIL step desktop exited 3' "$report" || fail "step failures in report"
pass
grep -qx '== Result' "$report" || fail "report lacks its result section"
pass
[[ "$(tail -n 1 "$report")" == 'Validation failed: 2 failed checks' ]] || fail "failure count: $(tail -n 1 "$report")"
pass
expect_output "Report: $report"
[[ "$(tail -n 1 "$work/state/engine.log")" == "rm --force bt-validate-kubuntu-"* ]] || fail "container kept after a failure"
pass
expect_no_fixture

# A failed install stops the run: no later step, but the report still ends
# with its result section and failure count, and the path is printed.
invoke STUB_FAIL_STEPS=install -- "$script" --target kubuntu --package "$deb" --output "$work/reports/install"
expect_status 1
report=$work/reports/install/$deb_name.validation
[[ "$(steps)" == "install kubuntu26.04 /artifact/$deb_name.deb;" ]] || fail "steps after a failed install: $(steps)"
pass
grep -qx 'FAIL step install exited 3' "$report" || fail "install failure in report"
pass
[[ "$(tail -n 4 "$report")" == $'FAIL step install exited 3\n\n== Result\nValidation failed: 1 failed checks' ]] \
    || fail "result after a failed install: $(tail -n 4 "$report")"
pass
[[ "$output" == *$'\nReport: '"$report"$'\nValidation failed: 1 failed checks' ]] \
    || fail "terminal output after a failed install: $output"
pass
[[ "$(tail -n 1 "$work/state/engine.log")" == "rm --force bt-validate-kubuntu-"* ]] || fail "container kept after install failed"
pass
expect_no_fixture

# --- --wayland pre-flight: each missing piece stops before any container runs
# wayland_args OUTPUT: the arguments of a --wayland run writing to OUTPUT.
wayland_args() { args=(--target kubuntu --package "$deb" --output "$1" --wayland); }
if [[ -n "$python" && -n "$jq_path" ]]; then
    make_socket "$work/xdg" wayland-0
    ln -s wayland-0 "$work/xdg/wayland-link"
    printf 'not a socket\n' > "$work/xdg/wayland-file"
    preflight() {
        local message=$1 args
        shift
        wayland_args "$work/reports/preflight"
        invoke "$@" -- "$script" "${args[@]}"
        expect_status 1
        expect_output "$message"
        expect_engine_calls 1
        expect_no_fixture
    }
    mv "$work/stubs/grim" "$work/grim"
    preflight '--wayland needs grim on the host' "WAYLAND_DISPLAY=wayland-0" "XDG_RUNTIME_DIR=$work/xdg" "SWAYSOCK=$work/xdg/wayland-0"
    mv "$work/grim" "$work/stubs/grim"
    mv "$work/stubs/swaymsg" "$work/swaymsg"
    preflight '--wayland needs swaymsg on the host' "WAYLAND_DISPLAY=wayland-0" "XDG_RUNTIME_DIR=$work/xdg"
    mv "$work/swaymsg" "$work/stubs/swaymsg"
    preflight '--wayland needs WAYLAND_DISPLAY and XDG_RUNTIME_DIR' "XDG_RUNTIME_DIR=$work/xdg"
    preflight '--wayland needs WAYLAND_DISPLAY and XDG_RUNTIME_DIR' "WAYLAND_DISPLAY=wayland-0"
    preflight "No Wayland socket at $work/xdg/wayland-file" "WAYLAND_DISPLAY=wayland-file" "XDG_RUNTIME_DIR=$work/xdg"
    preflight "No Wayland socket at $work/xdg/wayland-9" "WAYLAND_DISPLAY=$work/xdg/wayland-9" "XDG_RUNTIME_DIR=$work/xdg"
    preflight '--wayland needs SWAYSOCK' "WAYLAND_DISPLAY=wayland-0" "XDG_RUNTIME_DIR=$work/xdg"
    # A window of the app already on the session fails without running the Wayland steps.
    make_socket "$work/xdg" sway-ipc.1000.1.sock
    wayland_env=("WAYLAND_DISPLAY=wayland-link" "XDG_RUNTIME_DIR=$work/xdg")
    wayland_args "$work/reports/open"
    invoke STUB_WINDOW_OPEN=1 "${wayland_env[@]}" -- "$script" "${args[@]}"
    expect_status 1
    expect_output 'FAIL a local.branch_truce.BranchTruce window is already open on this session'
    [[ "$(steps)" == "install kubuntu26.04 /artifact/$deb_name.deb;layout;cli;desktop;mergetool-leave;" ]] \
        || fail "Wayland steps ran beside an open window: $(steps)"
    pass
    expect_no_fixture

    # --- --wayland with stub sway and grim: every window, screenshot and AT-SPI tree
    wayland_args "$work/reports/wayland"
    invoke "${wayland_env[@]}" -- "$script" "${args[@]}"
    expect_status 0
    report=$work/reports/wayland/$deb_name.validation
    # A plain run covers QT_SCALE_FACTOR 1, 1.5 and 2 with both Breeze schemes.
    expect_files "$work/reports/wayland" "$deb_name-editor-input.png $deb_name-mergetool-resolved.png \
$deb_name-scale1-BreezeDark.png $deb_name-scale1-BreezeLight.png $deb_name-scale1.5-BreezeDark.png \
$deb_name-scale1.5-BreezeLight.png $deb_name-scale2-BreezeDark.png $deb_name-scale2-BreezeLight.png \
$deb_name.atspi $deb_name.validation "
    expected_steps="install kubuntu26.04 /artifact/$deb_name.deb;layout;cli;desktop;mergetool-leave;wayland-minimal;session;"
    for window in '1 BreezeLight' '1 BreezeDark' '1.5 BreezeLight' '1.5 BreezeDark' '2 BreezeLight' '2 BreezeDark'; do
        expected_steps+="gui-start $window desktop;wait-ready;gui-stop;"
    done
    expected_steps+="gui-start 1 BreezeLight desktop;wait-ready;atspi-dump;atspi-check;editor-input;gui-stop;"
    expected_steps+="mergetool-start;mergetool-resolve;gui-stop;"
    [[ "$(steps)" == "$expected_steps" ]] || fail "Wayland steps: $(steps)"
    pass
    # The resolved socket, not the symlink, is mounted at the container's fixed path.
    grep -q -- "--volume $work/xdg/wayland-0:/run/bt-wayland/wayland-0 " "$work/state/engine.log" || fail "socket mount"
    pass
    # SWAYSOCK came from the runtime directory.
    [[ -s "$work/state/swaymsg.log" ]] && ! grep -qv "SWAYSOCK=$work/xdg/sway-ipc.1000.1.sock\$" "$work/state/swaymsg.log" \
        || fail "SWAYSOCK discovery"
    pass
    for line in "socket $work/xdg/wayland-0" 'compositor sway 1.10-stub' 'output HEADLESS-1 1920x1080 scale 1.5' \
        'PASS scale1-BreezeLight: desktop-entry launch shows the merge session' \
        'PASS scale1.5-BreezeLight: desktop-entry launch shows the merge session' \
        'PASS scale1.5-BreezeDark: desktop-entry launch shows the merge session' \
        'PASS scale2-BreezeDark: desktop-entry launch shows the merge session' \
        "screenshot $work/reports/wayland/$deb_name-scale1.5-BreezeLight.png (10,20 900x500)" \
        "screenshot $work/reports/wayland/$deb_name-scale2-BreezeDark.png (10,20 900x500)" \
        "AT-SPI tree: $work/reports/wayland/$deb_name.atspi (2 lines)" \
        "screenshot $work/reports/wayland/$deb_name-mergetool-resolved.png (10,20 900x500)" '== Result'; do
        grep -qxF -- "$line" "$report" || fail "report lacks: $line"
        pass
    done
    grep -qx 'window {"app_id":"local.branch_truce.BranchTruce","name":"Branch Truce","shell":"xdg_shell","rect":{"x":10,"y":20,"width":900,"height":500}}' \
        "$report" || fail "window line"
    pass
    ! grep -q '^FAIL' "$report" || fail "failures in a passing run"
    pass
    [[ "$(cat "$work/reports/wayland/$deb_name.atspi")" == "application 'Branch Truce'"$'\n'"  push-button | Mark resolved | focusable,enabled,showing" ]] \
        || fail "AT-SPI tree file"
    pass
    [[ "$(wc -l < "$work/state/grim.log")" == 8 ]] && grep -qx -- "-g 10,20 900x500 $work/reports/wayland/$deb_name-editor-input.png" \
        "$work/state/grim.log" || fail "grim calls"
    pass
    expect_no_fixture

    # A session that never gets ready fails every window, with no screenshot or tree.
    wayland_args "$work/reports/unready"
    invoke STUB_FAIL_STEPS=wait-ready "${wayland_env[@]}" -- "$script" "${args[@]}"
    expect_status 1
    report=$work/reports/unready/$deb_name.validation
    expect_files "$work/reports/unready" "$deb_name-mergetool-resolved.png $deb_name.validation "
    for line in 'FAIL scale1-BreezeLight: session not ready' 'FAIL scale1.5-BreezeDark: session not ready' \
        'FAIL scale2-BreezeDark: session not ready' \
        'FAIL window for editor input did not become ready'; do
        grep -qxF -- "$line" "$report" || fail "report lacks: $line"
        pass
    done
    [[ "$(steps)" != *atspi-dump* && "$(steps)" != *editor-input* ]] || fail "AT-SPI steps without a ready session"
    pass
    [[ "$(tail -n 1 "$report")" == 'Validation failed: 14 failed checks' ]] || fail "failure count: $(tail -n 1 "$report")"
    pass
    expect_no_fixture

    # A screenshot that grim cannot take is a failed check.
    wayland_args "$work/reports/grim"
    invoke STUB_GRIM_STATUS=1 "${wayland_env[@]}" -- "$script" "${args[@]}"
    expect_status 1
    report=$work/reports/grim/$deb_name.validation
    expect_files "$work/reports/grim" "$deb_name.atspi $deb_name.validation "
    grep -qx 'FAIL screenshot scale1-BreezeLight' "$report" && grep -qx 'FAIL screenshot scale1.5-BreezeDark' "$report" \
        && grep -qx 'FAIL screenshot mergetool-resolved' "$report" \
        || fail "screenshot failures"
    pass
    [[ "$(tail -n 1 "$report")" == 'Validation failed: 8 failed checks' ]] || fail "failure count: $(tail -n 1 "$report")"
    pass
    expect_no_fixture
fi

# --- container side, sourced, with the AT-SPI probe replaced by synthetic answers
# Runs CODE in a new bash that first sources the script. ARGS become the positional
# parameters. The container functions use $home, which CODE points into this directory.
sourced() {
    local code=$1
    shift
    rm -rf -- "$work/state" "$work/c-home" && mkdir -p -- "$work/state" "$work/c-home"
    if output=$(env -u STUB_FAIL_STEPS "PATH=$stub_path" "CONTAINER_ENGINE=$work/stubs/engine" "STUB_STATE=$work/state" \
        "BT_VALIDATE=$script" "BT_HOME=$work/c-home" \
        "$BASH" -c 'source "$BT_VALIDATE"; home=$BT_HOME; '"$code" bash "$@" 2>&1); then status=0; else status=$?; fi
}
expect_only_output() {
    [[ "$output" == "$1" ]] || fail "output is not exactly: $1"
    pass
}
# Sourcing defines the functions and runs neither half.
for args in '' '--target kubuntu --package /nonexistent.deb --output /nonexistent' '--in-container install kubuntu26.04 /x.deb'; do
    # shellcheck disable=SC2086
    sourced 'declare -F host_main container_main c_atspi_check c_wait_ready c_editor_input >/dev/null; echo sourced' $args
    expect_status 0
    expect_only_output sourced
    expect_engine_calls 0
done

# The tree the probe prints for the application's accessible controls.
if [[ -n "$python" ]]; then
    invoke -- "$python" -I -B "$root/tests/packaging/AtspiProbeFixture.py" "$probe" selftest
    expect_status 0
    [[ "$output" =~ ^[0-9]+\ atspi-probe\ assertions\ passed$ ]] || fail "atspi-probe self-test"
    assertions=$((assertions + ${output%% *}))
    invoke -- "$python" -I -B "$root/tests/packaging/AtspiProbeFixture.py" "$probe" dump
    expect_status 0
    probe_tree=$output
    invoke -- "$python" -I -B "$root/tests/packaging/AtspiProbeFixture.py" "$probe" text
    expect_status 0
    probe_text=$output
    [[ "$probe_text" == "text 'merged é ü\\u2029'"$'\neditable-text interface: True' ]] || fail "probe text output"
    pass
else
    # The same lines the probe prints, for the shell checks below.
    probe_tree="application 'Branch Truce'
application | Branch Truce |
  frame | Branch Truce | enabled,showing
    list-item | tracked.txt | focusable,enabled,showing
    text | Left source | focusable,read_only,enabled,showing
    text | Editable result | focusable,editable,enabled,showing
    text | Right source | focusable,read_only,enabled,showing
    text | Passage search | focusable,editable,enabled,showing
    combo-box | Search scope | focusable,enabled,showing
    check-box | Wrap lines | focusable,enabled,showing
    push-button | Save | focusable,enabled,showing
    push-button | Mark resolved | focusable,enabled,showing"
    probe_text="text 'merged é ü\\u2029'"$'\neditable-text interface: True'
fi
export PROBE_TREE=$probe_tree PROBE_TEXT=$probe_text

atspi_passes='PASS accessible (push-)?button | Mark resolved
PASS accessible (push-)?button | Save
PASS accessible text | Editable result
PASS accessible text | Passage search
PASS accessible combo-box | Search scope
PASS accessible check-box | Wrap lines
PASS accessible list-item | tracked.txt'
# c_atspi_check accepts the probe's tree with the push-button role nick of AT-SPI 2.52 ...
sourced 'probe() { printf "%s\n" "$PROBE_TREE"; }; c_atspi_check'
expect_status 0
expect_only_output $'\n== Accessibility metadata over AT-SPI\n'"$atspi_passes"$'\nobjects 12; unnamed buttons: 0\nPASS source panes report read_only'
# ... and the button nick of AT-SPI 2.60.
sourced 'probe() { printf "%s\n" "${PROBE_TREE//push-button/button}"; }; c_atspi_check'
expect_status 0
expect_only_output $'\n== Accessibility metadata over AT-SPI\n'"$atspi_passes"$'\nobjects 12; unnamed buttons: 0\nPASS source panes report read_only'
# Missing controls, unnamed buttons and editable source panes are reported.
sourced 'probe() { printf "%s\n" "$PROBE_TREE" | grep -v "Wrap lines" | sed "s/| Save |/|  |/; s/read_only,//"; }; c_atspi_check'
expect_status 0
expect_output 'FAIL no accessible (push-)?button | Save'
expect_output 'FAIL no accessible check-box | Wrap lines'
expect_output 'objects 11; unnamed buttons: 1'
expect_output 'FAIL source panes do not report read_only'
expect_output 'PASS accessible (push-)?button | Mark resolved'

# c_wait_ready waits 90 s for Mark resolved and shows the app's log when it times out.
sourced 'probe() { echo "probe $*" >> "$STUB_STATE/probe.log"; echo "showing '"'Mark resolved'"'"; }; c_wait_ready'
expect_status 0
expect_only_output "AT-SPI: showing 'Mark resolved'"
[[ "$(cat "$work/state/probe.log")" == 'probe wait Mark resolved 90' ]] || fail "wait-ready probe arguments"
pass
sourced 'printf "libEGL warning: x\n\nMESA: y\nqt.qpa.wayland: no shell\n1\n2\n3\n4\n5\n" > "$home/gui.log"
    probe() { echo "no showing accessible object named '"'Mark resolved'"'"; return 1; }; c_wait_ready'
expect_status 1
expect_only_output $'no showing accessible object named \'Mark resolved\'\nqt.qpa.wayland: no shell\n1\n2\n3\n4'

# c_editor_input types through wtype on the container's socket and reads the result
# back with the probe's own text format, accented letters and paragraph separator included.
sourced 'probe() { echo "probe $*" >> "$STUB_STATE/probe.log"; case $1 in text) printf "%s\n" "$PROBE_TEXT" ;; *) echo "grab_focus True" ;; esac; }
    c_editor_input'
expect_status 0
expect_output "PASS wtype sent Ctrl+A, 'merged é ü' and Return"
expect_output 'PASS the result holds the typed text, accented letters included'
[[ "$(cat "$work/state/probe.log")" == $'probe focus Editable result\nprobe text Editable result' ]] || fail "editor probe calls"
pass
[[ "$(cat "$work/state/wtype.log")" == 'WAYLAND_DISPLAY=/run/bt-wayland/wayland-0 -s 400 -M ctrl a -m ctrl -s 100 merged é ü -k Return' ]] \
    || fail "wtype arguments: $(cat "$work/state/wtype.log")"
pass
sourced 'probe() { case $1 in text) echo "text '"'merged é ü'"'" ;; *) echo "grab_focus False" ;; esac; }
    export STUB_WTYPE_STATUS=1; c_editor_input'
expect_status 0
expect_output 'FAIL wtype'
expect_output 'FAIL the result does not hold the typed text'
expect_no_fixture

if ((${#missing[@]} > 0)); then
    printf 'SKIP Wayland host steps and atspi-probe tests need: %s (%s assertions passed)\n' "${missing[*]}" "$assertions"
    exit 77
fi
printf '%s installed validation tooling assertions passed\n' "$assertions"
