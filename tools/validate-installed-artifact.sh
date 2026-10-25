#!/usr/bin/env bash
# Installs a branch-truce .deb in a clean container of its own OS base and
# exercises the installed application with the source and build trees absent.
#
#   tools/validate-installed-artifact.sh --target kubuntu|tuxedo --package DEB --output DIR [--wayland]
#
# The container comes from branch-truce-artifact:<base>-target, which
# tools/build-artifact.sh leaves. It needs network access for the package
# lists. Nothing is installed on the host. docs/platform-validation.md records
# the results and what this cannot show: the container is not a desktop.
#
# Steps without a display (QT_QPA_PLATFORM=offscreen):
#   install         apt install --no-install-recommends of the .deb, the runtime
#                   baseline, then the validation tools, which are not part of
#                   the closure
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
#   wayland-minimal the installed closure alone tries the Wayland platform
#   session         installs qt6-wayland and plasma-integration, as Plasma has
#   window LABEL    one desktop-entry launch per scale and Breeze scheme: the
#                   window's app_id and size from sway, an AT-SPI readiness
#                   check and a grim screenshot DIR/<package>-LABEL.png
#   editor-input    AT-SPI focus on the editable result, then text typed
#                   through the Wayland virtual keyboard (wtype), read back
#                   over AT-SPI; DIR/<package>.atspi has the accessible tree
#   mergetool-resolve  `git mergetool` again; the result is typed, Mark
#                   resolved and Confirm resolution are pressed over AT-SPI,
#                   and Git stages the reviewed bytes
#
# DIR receives <package>.validation (this report), the screenshots and the
# AT-SPI tree. CONTAINER_ENGINE defaults to docker. The container is removed
# on exit; the image stays.
set -euo pipefail

app_id=local.branch_truce.BranchTruce
home=/tmp/bt-home
repo=/tmp/bt-repo
wayland_socket=/run/bt-wayland/wayland-0
result_text='merged é ü'

usage() { echo "Usage: $0 --target kubuntu|tuxedo --package DEB --output DIR [--wayland]" >&2; exit 2; }

base_for() {
    case "$1" in
        kubuntu) echo kubuntu26.04 ;;
        tuxedo) echo tuxedo24.04 ;;
        *) usage ;;
    esac
}

# ---------------------------------------------------------------- host side

host_main() {
    local target= package= output= wayland=0
    while (($# > 0)); do
        case "$1" in
            --target) [[ $# -ge 2 ]] || usage; target=$2; shift 2 ;;
            --package) [[ $# -ge 2 ]] || usage; package=$2; shift 2 ;;
            --output) [[ $# -ge 2 ]] || usage; output=$2; shift 2 ;;
            --wayland) wayland=1; shift ;;
            *) usage ;;
        esac
    done
    [[ -n "$target" && -f "$package" && -n "$output" ]] || usage
    local root base image socket=
    root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
    engine=${CONTAINER_ENGINE:-docker}
    base=$(base_for "$target")
    image=branch-truce-artifact:${target}-${base#"$target"}-target
    package=$(cd -- "$(dirname -- "$package")" && pwd -P)/$(basename -- "$package")
    name=$(basename -- "$package" .deb)
    mkdir -p -- "$output"
    output=$(cd -- "$output" && pwd -P)
    "$engine" image inspect "$image" >/dev/null 2>&1 \
        || { echo "Missing image $image; run tools/build-artifact.sh --target $target first" >&2; exit 1; }
    if ((wayland)); then
        local tool
        for tool in swaymsg jq grim; do
            command -v "$tool" >/dev/null || { echo "--wayland needs $tool on the host" >&2; exit 1; }
        done
        [[ -n "${WAYLAND_DISPLAY:-}" && -n "${XDG_RUNTIME_DIR:-}" ]] \
            || { echo "--wayland needs WAYLAND_DISPLAY and XDG_RUNTIME_DIR" >&2; exit 1; }
        socket=$WAYLAND_DISPLAY
        [[ "$socket" == /* ]] || socket=$XDG_RUNTIME_DIR/$socket
        socket=$(readlink -f -- "$socket")
        [[ -S "$socket" ]] || { echo "No Wayland socket at $socket" >&2; exit 1; }
        if [[ -z "${SWAYSOCK:-}" ]]; then
            local found=("$XDG_RUNTIME_DIR"/sway-ipc.*.sock)
            [[ -S "${found[0]}" ]] || { echo "--wayland needs SWAYSOCK" >&2; exit 1; }
            export SWAYSOCK=${found[0]}
        fi
    fi

    fixture=$(mktemp -d "${TMPDIR:-/tmp}/bt-validate.XXXXXXXX")
    container=bt-validate-$target-$$
    trap host_cleanup EXIT
    cp -- "$root/tools/validate-installed-artifact.sh" "$root/tools/artifact-baseline.sh" \
        "$root/tools/atspi-probe.py" "$fixture/"
    local mounts=(--volume "$package:/artifact/$name.deb:ro" --volume "$fixture:/validate:ro")
    ((wayland == 0)) || mounts+=(--volume "$socket:$wayland_socket")
    "$engine" run --detach --init --name "$container" "${mounts[@]}" "$image" sleep infinity >/dev/null

    local report=$output/$name.validation
    run_steps "$base" "$wayland" "$socket" 2>&1 | tee "$report"
    local failed
    failed=$(grep -c '^FAIL ' "$report" || true)
    echo "Report: $report"
    if [[ "$failed" != 0 ]] || ! grep -q '^== Result' "$report"; then
        echo "Validation failed: $failed failed checks" | tee -a "$report"
        return 1
    fi
}

host_cleanup() {
    local status=$?
    trap - EXIT
    "$engine" rm --force "$container" >/dev/null 2>&1 || true
    rm -rf -- "$fixture"
    exit "$status"
}

# Runs one container step. A step that stops early is a failed check.
inside() {
    local status=0
    "$engine" exec "$container" /bin/bash /validate/validate-installed-artifact.sh --in-container "$@" || status=$?
    ((status == 0)) || echo "FAIL step $1 exited $status"
    return "$status"
}

run_steps() {
    local base=$1 wayland=$2 socket=$3
    set +e
    printf '== Artifact\nfile %s.deb\nsha256 %s\nhost kernel %s\n' "$name" \
        "$(sha256sum < "$package" | cut -d' ' -f1)" "$(uname -r)"
    inside install "$base" "/artifact/$name.deb" || return 1
    inside layout
    inside cli
    inside desktop
    inside mergetool-leave
    if ((wayland == 0)); then
        printf '\n== Wayland session\nSKIP not requested; Wayland, scaling, theme, input and AT-SPI not exercised\n'
    else
        wayland_steps "$socket"
    fi
    printf '\n== Result\n'
}

wayland_steps() {
    local socket=$1
    printf '\n== Wayland session\n'
    echo "socket $socket"
    echo "compositor $(swaymsg -t get_version | jq -r .human_readable | sed 's/^/sway /')"
    swaymsg -t get_outputs | jq -r '.[] | "output \(.name) \(.current_mode.width)x\(.current_mode.height) scale \(.scale)"'
    if [[ -n "$(window_json)" ]]; then
        echo "FAIL a $app_id window is already open on this session"
        return 0
    fi
    inside wayland-minimal
    inside session
    local scale scheme
    for scale in 1 2; do
        for scheme in BreezeLight BreezeDark; do
            window_run "scale$scale-$scheme" "$scale" "$scheme"
        done
    done
    printf '\n== Editor input and accessibility (scale 1, BreezeLight)\n'
    inside gui-start 1 BreezeLight desktop
    if wait_window present 60 && inside wait-ready; then
        inside atspi-dump > "$output/$name.atspi"
        echo "AT-SPI tree: $output/$name.atspi ($(wc -l < "$output/$name.atspi") lines)"
        inside atspi-check
        inside editor-input
        shoot editor-input
    else
        echo "FAIL window for editor input did not become ready"
    fi
    inside gui-stop
    wait_window absent 30 || echo "FAIL window still open after the app stopped"
    printf '\n== Mergetool resolve over Wayland\n'
    inside mergetool-start
    if wait_window present 60; then
        inside mergetool-resolve
        shoot mergetool-resolved
    else
        echo "FAIL the launcher-started window did not appear"
    fi
    inside gui-stop
    wait_window absent 30 || echo "FAIL window still open after the app stopped"
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

shoot() {
    local file=$output/$name-$1.png geometry
    geometry=$(window_json | jq -r '.rect | "\(.x),\(.y) \(.width)x\(.height)"')
    if [[ -n "$geometry" ]] && grim -g "$geometry" "$file"; then
        echo "screenshot $file ($geometry)"
    else
        echo "FAIL screenshot $1"
    fi
}

window_run() {
    local label=$1 scale=$2 scheme=$3
    printf '\n== Window %s\n' "$label"
    inside gui-start "$scale" "$scheme" desktop
    if wait_window present 60; then
        if inside wait-ready; then
            echo "PASS $label: desktop-entry launch shows the merge session"
            shoot "$label"
        else
            echo "FAIL $label: session not ready"
        fi
    else
        echo "FAIL $label: no window"
    fi
    inside gui-stop
    wait_window absent 30 || echo "FAIL $label: window still open after the app stopped"
}

# ------------------------------------------------------ inside the container

pass() { printf 'PASS %s\n' "$*"; }
fail() { printf 'FAIL %s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }

# The isolated environment of every application and Git command.
base_env=(
    "PATH=/usr/bin:/bin" "HOME=$home" "TMPDIR=$home/tmp" "LC_ALL=C.UTF-8"
    "XDG_DATA_HOME=$home/data" "XDG_CONFIG_HOME=$home/config" "XDG_CACHE_HOME=$home/cache"
    "XDG_RUNTIME_DIR=$home/runtime" "GIT_CONFIG_NOSYSTEM=1" "GIT_CONFIG_GLOBAL=$home/gitconfig"
    "GIT_TERMINAL_PROMPT=0"
)
offscreen_env=("QT_QPA_PLATFORM=offscreen" "QT_QUICK_BACKEND=software")
isolated() { (cd "$repo" && env -i "${base_env[@]}" "$@"); }

# A fresh home and a repository stopped on one merge conflict in tracked.txt.
fresh_fixture() {
    pkill -x branch-truce 2>/dev/null || true
    rm -rf -- "$home/data" "$home/config" "$home/cache" "$home/tmp" "$home/runtime" "$repo" \
        "$home/mergetool.log" "$home/mergetool.status"
    mkdir -p "$home/data" "$home/config" "$home/cache" "$home/tmp" "$home/runtime" "$repo"
    chmod 700 "$home/runtime"
    cat > "$home/gitconfig" <<'EOF'
[mergetool "truce"]
	cmd = MERGED=\"$MERGED\" BASE=\"$BASE\" LOCAL=\"$LOCAL\" REMOTE=\"$REMOTE\" '/usr/bin/branch-truce' --mergetool
	trustExitCode = true
[mergetool]
	keepBackup = false
	keepTemporaries = false
[core]
	autocrlf = false
[init]
	defaultBranch = main
[user]
	name = Branch Truce Validation
	email = validation@example.invalid
EOF
    isolated git init --quiet
    printf 'base\n' > "$repo/tracked.txt"
    isolated git add tracked.txt && isolated git commit --quiet -m base
    isolated git checkout --quiet -b side
    printf 'side\n' > "$repo/tracked.txt"
    isolated git commit --quiet -am side && isolated git checkout --quiet main
    printf 'main\n' > "$repo/tracked.txt"
    isolated git commit --quiet -am main
    ! isolated git merge --no-edit side >/dev/null 2>&1 || fail "fixture merge did not conflict"
}

gui_pids() {
    local pid
    for pid in $(pgrep -x branch-truce || true); do
        # The headless launcher is `branch-truce --mergetool`; the GUI is anything else.
        [[ "$(tr '\0' ' ' 2>/dev/null < "/proc/$pid/cmdline")" == *' --mergetool ' ]] || echo "$pid"
    done
}

# wait_until SECONDS COMMAND...: true once COMMAND succeeds within SECONDS.
wait_until() {
    local deadline=$((SECONDS + $1)); shift
    until "$@"; do
        ((SECONDS < deadline)) || return 1
        sleep 0.2
    done
}
session_socket() { compgen -G "$home/runtime/branch-truce/bt-mt-*" >/dev/null; }
no_gui() { [[ -z "$(gui_pids)" ]]; }
intake_present() { compgen -G "$home/data/branch-truce/mergetool/*/*" >/dev/null; }
mergetool_done() { [[ -f "$home/mergetool.status" ]]; }

stop_gui() {
    local pids
    pids=$(gui_pids)
    [[ -z "$pids" ]] || kill -TERM $pids 2>/dev/null || true
    wait_until 20 no_gui
}

probe() {
    env -i "${base_env[@]}" "DBUS_SESSION_BUS_ADDRESS=$(cat "$home/dbus.address")" \
        python3 -I /validate/atspi-probe.py 'Branch Truce' "$@"
}

c_install() {
    local base=$1 deb=$2 version before after
    export DEBIAN_FRONTEND=noninteractive
    step "Install on a clean $base container"
    version=$(dpkg-deb -f "$deb" Version)
    [[ "$version" == *"+$base" ]] && pass "version $version names $base" || fail "version $version lacks +$base"
    before=$(dpkg-query -W -f='${Package}\n' | sort)
    apt-get update -q >/dev/null
    if ! apt-get install -y -q --no-install-recommends "$deb" > /tmp/install.log 2>&1; then
        cat /tmp/install.log; fail "apt install"; return 1
    fi
    after=$(dpkg-query -W -f='${Package}\n' | sort)
    pass "apt-get install --no-install-recommends added $(comm -13 <(echo "$before") <(echo "$after") | wc -l) packages"
    [[ "$(dpkg-query -W -f='${db:Status-Status} ${Version}' branch-truce)" == "installed $version" ]] \
        && pass "branch-truce $version installed" || fail "branch-truce $version not installed"
    step "Runtime baseline"
    /bin/bash /validate/artifact-baseline.sh
    printf 'kernel\t%s (shared with the host)\n' "$(uname -r)"
    step "Validation tools (not part of the closure)"
    local tools=(libglib2.0-bin dbus-daemon at-spi2-core gir1.2-atspi-2.0 python3-gi wtype)
    if apt-get install -y -q --no-install-recommends "${tools[@]}" > /tmp/tools.log 2>&1; then
        dpkg-query -W -f='${Package}\t${Version}\n' "${tools[@]}"
    else
        cat /tmp/tools.log; fail "validation tools did not install"; return 1
    fi
    mkdir -p "$home"
    dbus-daemon --session --fork --print-address=3 3> "$home/dbus.address"
    [[ -s "$home/dbus.address" ]] && pass "private D-Bus session bus" || fail "D-Bus session bus"
}

c_layout() {
    step "No source or build tree"
    local found
    # The application's own QML file names, CMake files and the build path.
    found=$(find / -xdev \( -name CMakeCache.txt -o -name CMakeLists.txt -o -name Main.qml \
        -o -name ComparisonView.qml -o -path /work -o -name 'branch-truce-*' -type d \) -print 2>/dev/null || true)
    [[ -z "$found" ]] && pass "no CMake file, application QML source or /work on the system" \
        || { echo "$found"; fail "build or source files present"; }
    echo "branch-truce files: $(dpkg -L branch-truce | while read -r f; do [[ -f "$f" ]] && echo "$f"; done | tr '\n' ' ')"
}

c_cli() {
    step "Command line of the installed binary"
    fresh_fixture
    local out
    out=$(isolated "${offscreen_env[@]}" branch-truce --version 2>&1) \
        && pass "branch-truce --version: $out" || fail "--version: $out"
    isolated "${offscreen_env[@]}" branch-truce --help > /tmp/help.txt 2>&1 \
        && pass "branch-truce --help ($(wc -l < /tmp/help.txt) lines)" || fail "--help"
    isolated "${offscreen_env[@]}" QML_IMPORT_TRACE=1 QT_DEBUG_PLUGINS=1 \
        branch-truce --smoke-test "$repo" > /tmp/smoke.log 2>&1 || fail "smoke test exited $?"
    local record
    record=$(sed -n 's/^BRANCH_TRUCE_SESSION=//p' /tmp/smoke.log)
    echo "session $record"
    [[ "$record" == *'"state":"ready"'* && "$record" == *'"path":"tracked.txt"'* ]] \
        && pass "--smoke-test opened the merge session" || fail "--smoke-test did not open the session"
    step "Installed resource lookup during --smoke-test"
    local paths path owner unowned=0 embedded
    embedded=$(grep -oE '(qrc)?:/qt/qml/BranchTruce/[^ "]*' /tmp/smoke.log | sort -u | tr '\n' ' ')
    echo "embedded QML: ${embedded:-none}"
    [[ -n "$embedded" ]] && pass "application QML comes from the binary's resources" || fail "no embedded BranchTruce module"
    paths=$(grep -oE '(^|[^:A-Za-z0-9_.])/[^ "():]*(qmldir|\.so)' /tmp/smoke.log | sed 's/^[^/]*//' | sort -u)
    while IFS= read -r path; do
        [[ -e "$path" ]] || continue
        owner=$(dpkg-query -S "$path" 2>/dev/null | head -1 | cut -d: -f1)
        [[ -n "$owner" ]] || owner=$(dpkg-query -S "$(readlink -f "$path")" 2>/dev/null | head -1 | cut -d: -f1)
        [[ -n "$owner" ]] || { echo "unowned $path"; unowned=$((unowned + 1)); }
    done <<<"$paths"
    echo "loaded from the file system: $(grep -c qmldir <<<"$paths") qmldir, $(grep -c '\.so$' <<<"$paths") plugins"
    grep -E '/(platforms|styles|platformthemes)/' <<<"$paths" | sed 's/^/  /' || true
    ((unowned == 0)) && pass "every loaded qmldir and plugin belongs to an installed package" \
        || fail "$unowned loaded files belong to no package"
}

c_desktop() {
    step "Desktop-entry launch (offscreen)"
    fresh_fixture
    local exec_line pid
    exec_line=$(sed -n 's/^Exec=//p' "/usr/share/applications/$app_id.desktop")
    echo "Exec=$exec_line"
    (cd / && env -i "${base_env[@]}" "${offscreen_env[@]}" \
        setsid gio launch "/usr/share/applications/$app_id.desktop" "$repo" > /tmp/desktop.log 2>&1 < /dev/null &)
    if wait_until 60 session_socket; then
        pid=$(gui_pids | head -1)
        echo "process $(tr '\0' ' ' < "/proc/$pid/cmdline"); executable $(readlink "/proc/$pid/exe")"
        pass "gio launch of $app_id.desktop opened the session and listens for the mergetool"
    else
        cat /tmp/desktop.log; fail "desktop-entry launch did not open the session"
    fi
    stop_gui || fail "GUI did not stop"
}

c_mergetool_leave() {
    step "Mergetool handoff with real Git, GUI ends without a decision (offscreen)"
    fresh_fixture
    local before
    before=$(sha256sum < "$repo/tracked.txt")
    echo "git $(isolated git --version)"
    (cd "$repo" && env -i "${base_env[@]}" "${offscreen_env[@]}" setsid bash -c \
        'git mergetool --tool truce --no-prompt tracked.txt > "$0/mergetool.log" 2>&1; echo $? > "$0/mergetool.status"' \
        "$home" < /dev/null &)
    if wait_until 60 session_socket && wait_until 30 intake_present; then
        pass "the launcher started $(tr '\0' ' ' < "/proc/$(gui_pids | head -1)/cmdline")and attached tracked.txt"
    else
        fail "no launcher-started session took the path"
    fi
    stop_gui || fail "GUI did not stop"
    wait_until 30 mergetool_done || fail "git mergetool did not finish"
    sed 's/^/  /' "$home/mergetool.log"
    local status
    status=$(cat "$home/mergetool.status" 2>/dev/null || echo none)
    [[ "$status" != 0 && "$status" != none ]] && pass "git mergetool exited $status" || fail "git mergetool exited $status"
    [[ "$(sha256sum < "$repo/tracked.txt")" == "$before" ]] \
        && pass "MERGED has its launch bytes again" || fail "MERGED changed"
    [[ "$(isolated git ls-files -u -- tracked.txt | wc -l)" == 3 ]] \
        && pass "tracked.txt stays unmerged in the index" || fail "index changed"
}

c_wayland_minimal() {
    step "Wayland with the installed closure only"
    fresh_fixture
    local status=0
    env -i "${base_env[@]}" "QT_QPA_PLATFORM=wayland" "WAYLAND_DISPLAY=$wayland_socket" \
        timeout 60 branch-truce --smoke-test "$repo" > /tmp/wayland-minimal.log 2>&1 || status=$?
    grep -E 'qt.qpa|platform plugin|BRANCH_TRUCE_SESSION' /tmp/wayland-minimal.log | cut -c1-200 | sed 's/^/  /' || true
    echo "qt6-wayland: $(dpkg-query -W -f='${db:Status-Status}' qt6-wayland 2>/dev/null || echo not-installed)"
    if ((status == 0)); then
        pass "--smoke-test on Wayland with the closure alone"
    else
        echo "WARN --smoke-test on Wayland exited $status with the closure alone; Depends does not bring the Wayland shell integration"
    fi
}

c_session() {
    step "Session packages a Plasma desktop has"
    local packages=(qt6-wayland plasma-integration)
    if DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends "${packages[@]}" > /tmp/session.log 2>&1; then
        dpkg-query -W -f='${Package}\t${Version}\n' "${packages[@]}"
        pass "session packages installed"
    else
        cat /tmp/session.log; fail "session packages"
    fi
}

# gui-start SCALE SCHEME desktop|direct
c_gui_start() {
    local scale=$1 scheme=$2 how=$3
    fresh_fixture
    { cat "/usr/share/color-schemes/$scheme.colors"; printf '\n[General]\nColorScheme=%s\n' "$scheme"; } \
        > "$home/config/kdeglobals"
    local launch=(branch-truce "$repo")
    [[ "$how" == direct ]] || launch=(gio launch "/usr/share/applications/$app_id.desktop" "$repo")
    (cd / && env -i "${base_env[@]}" "${wayland_env[@]}" "QT_SCALE_FACTOR=$scale" \
        "DBUS_SESSION_BUS_ADDRESS=$(cat "$home/dbus.address")" \
        setsid "${launch[@]}" > "$home/gui.log" 2>&1 < /dev/null &)
    echo "started ${launch[*]} with QT_SCALE_FACTOR=$scale, colour scheme $scheme"
}

wayland_env=(
    "WAYLAND_DISPLAY=$wayland_socket" "QT_QPA_PLATFORM=wayland" "XDG_SESSION_TYPE=wayland"
    "XDG_CURRENT_DESKTOP=KDE" "KDE_SESSION_VERSION=6" "KDE_FULL_SESSION=true"
    "QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1"
)

c_wait_ready() {
    local ready
    # The Mark resolved button shows once the session has loaded tracked.txt.
    if ready=$(probe wait 'Mark resolved' 90 2>&1); then
        echo "AT-SPI: $ready"
    else
        echo "$ready"; grep -vE '^(libEGL|MESA|$)' "$home/gui.log" | head -5; return 1
    fi
}

c_atspi_dump() { probe dump 2>/dev/null; }

c_atspi_check() {
    step "Accessibility metadata over AT-SPI"
    local tree name
    tree=$(probe dump 2>/dev/null)
    # The role nick is push-button with AT-SPI 2.52 (noble) and button with 2.60 (resolute).
    for name in '(push-)?button \| Mark resolved' '(push-)?button \| Save' 'text \| Editable result' \
        'text \| Passage search' 'combo-box \| Search scope' 'check-box \| Wrap lines' 'list-item \| tracked.txt'; do
        grep -qE -- "^ *$name \|" <<<"$tree" && pass "accessible ${name//\\/}" || fail "no accessible ${name//\\/}"
    done
    echo "objects $(wc -l <<<"$tree"); unnamed buttons: $(grep -cE '^ *(push-)?button \|  \|' <<<"$tree" || true)"
    grep -E '^ *text \| (Left source|Right source)' <<<"$tree" | grep -q read_only \
        && pass "source panes report read_only" || fail "source panes do not report read_only"
}

c_editor_input() {
    step "Editor input through the Wayland virtual keyboard"
    probe focus 'Editable result'
    # The first keys after wtype uploads its keymap can be lost, so it waits.
    if env "WAYLAND_DISPLAY=$wayland_socket" wtype -s 400 -M ctrl a -m ctrl -s 100 "$result_text" -k Return; then
        pass "wtype sent Ctrl+A, '$result_text' and Return"
    else
        fail "wtype"
    fi
    sleep 1
    local text
    text=$(probe text 'Editable result' 2>/dev/null)
    echo "$text"
    [[ "$text" == *"text '$result_text\\u2029'"* ]] \
        && pass "the result holds the typed text, accented letters included" || fail "the result does not hold the typed text"
}

c_mergetool_start() {
    step "Mergetool handoff with real Git on Wayland"
    fresh_fixture
    cp "/usr/share/color-schemes/BreezeLight.colors" "$home/config/kdeglobals"
    (cd "$repo" && env -i "${base_env[@]}" "${wayland_env[@]}" "QT_SCALE_FACTOR=1" \
        "DBUS_SESSION_BUS_ADDRESS=$(cat "$home/dbus.address")" setsid bash -c \
        'git mergetool --tool truce --no-prompt tracked.txt > "$0/mergetool.log" 2>&1; echo $? > "$0/mergetool.status"' \
        "$home" < /dev/null &)
    echo "started git mergetool --tool truce"
}

c_mergetool_resolve() {
    c_wait_ready || { fail "session not ready"; return 0; }
    c_editor_input
    probe press 'Mark resolved'
    probe wait 'Confirm resolution' 30 && probe press 'Confirm resolution' || fail "no Confirm resolution"
    wait_until 60 mergetool_done || fail "git mergetool did not finish"
    sed 's/^/  /' "$home/mergetool.log"
    local status expected staged
    status=$(cat "$home/mergetool.status" 2>/dev/null || echo none)
    [[ "$status" == 0 ]] && pass "git mergetool exited 0" || fail "git mergetool exited $status"
    expected=$(printf '%s\n' "$result_text" | isolated git hash-object --stdin)
    staged=$(isolated git ls-files -s -- tracked.txt)
    echo "index $staged"
    [[ "$staged" == "100644 $expected 0"$'\t'tracked.txt ]] \
        && pass "Git staged the reviewed text $expected" || fail "index does not hold the reviewed text"
    [[ "$(isolated git hash-object tracked.txt)" == "$expected" ]] \
        && pass "MERGED holds the reviewed bytes" || fail "MERGED differs from the reviewed text"
    # The session polls the index for the staged object before it claims the file.
    probe wait '1 files in this session · 1 resolved' 60 \
        && pass "the session reports tracked.txt resolved after Git staged it" \
        || fail "the session did not report tracked.txt resolved"
}

c_gui_stop() { stop_gui || fail "GUI did not stop"; }

container_main() {
    local command=$1; shift
    case "$command" in
        install) c_install "$@" ;;
        layout) c_layout ;;
        cli) c_cli ;;
        desktop) c_desktop ;;
        mergetool-leave) c_mergetool_leave ;;
        wayland-minimal) c_wayland_minimal ;;
        session) c_session ;;
        gui-start) c_gui_start "$@" ;;
        wait-ready) c_wait_ready ;;
        atspi-dump) c_atspi_dump ;;
        atspi-check) c_atspi_check ;;
        editor-input) c_editor_input ;;
        mergetool-start) c_mergetool_start ;;
        mergetool-resolve) c_mergetool_resolve ;;
        gui-stop) c_gui_stop ;;
        *) usage ;;
    esac
}

# Sourcing the file defines its functions but runs neither half.
[[ "${BASH_SOURCE[0]}" == "$0" ]] || return 0
if [[ "${1:-}" == --in-container ]]; then
    shift
    container_main "$@"
else
    host_main "$@"
fi
