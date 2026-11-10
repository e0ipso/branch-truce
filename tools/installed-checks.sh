#!/usr/bin/env bash
# The steps of tools/validate-installed-artifact.sh that run inside its
# validation container, as root, against the installed package:
#
#   installed-checks.sh STEP [ARG...]
#
# The header of validate-installed-artifact.sh lists the steps. That script
# copies this file, lib/fixture.sh, lib/report.sh, atspi-probe.py and
# imports.txt (the QML modules src/ui imports) to /validate. A step prints its
# report on stdout and exits 0 when every check passed and 1 when one failed;
# a command that fails unexpectedly ends the step, which fails it. An unknown
# step exits 2.
#
# Sourcing the file defines its functions and constants and runs nothing, so
# tests can call them.

source "$(dirname -- "${BASH_SOURCE[0]}")/lib/fixture.sh"
source "$(dirname -- "${BASH_SOURCE[0]}")/lib/report.sh"

app_id=com.canpicasoft.branch-truce
fixture=
home=
repo=
wayland_socket=/run/bt-wayland/wayland-0
qml_root=/usr/lib/x86_64-linux-gnu/qt6/qml
result_text='merged é ü'
offscreen_env=("QT_QPA_PLATFORM=offscreen" "QT_QUICK_BACKEND=software")
wayland_env=(
    "WAYLAND_DISPLAY=$wayland_socket" "QT_QPA_PLATFORM=wayland" "XDG_SESSION_TYPE=wayland"
    "XDG_CURRENT_DESKTOP=KDE" "KDE_SESSION_VERSION=6" "KDE_FULL_SESSION=true"
    "QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1"
)

usage() { echo "Usage: $0 STEP [ARG...]"; }

# private [NAME=VALUE...] COMMAND...: COMMAND in / with only the private
# environment of $home, which every application and Git command gets. isolated
# runs it in $repo.
private() { fixture_run "$home" / "$@"; }
isolated() { fixture_run "$home" "$repo" "$@"; }

# A fresh home and a repository stopped on one merge conflict in tracked.txt.
fresh_fixture() {
    pkill -x branch-truce 2>/dev/null || true
    fixture_home "$fixture" "$home" && fixture_gitconfig "$fixture" "$home" /usr/bin/branch-truce \
        && fixture_merge_repo "$fixture" "$home" "$repo" tracked.txt || fail failures "fixture merge did not conflict"
}

gui_pids() {
    local pid
    for pid in $(pgrep -x branch-truce || true); do
        # The headless launcher is `branch-truce --mergetool`; the GUI is anything else.
        [[ "$(tr '\0' ' ' 2>/dev/null < "/proc/$pid/cmdline")" == *' --mergetool ' ]] || echo "$pid"
    done
}

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
    private "DBUS_SESSION_BUS_ADDRESS=$(cat "$home/dbus.address")" \
        python3 -I /validate/atspi-probe.py 'Branch Truce' "$@"
}

# owner_of PATH: the package that ships PATH, or nothing. A path no package
# ships is an answer, not an error, so the merged-/usr fallbacks can run.
owner_of() { { dpkg-query -S "$1" 2>/dev/null || true; } | head -1 | cut -d: -f1; }
# Package names in a Depends field, without versions or alternatives' order.
depends_names() { tr ',|' '\n\n' <<<"$1" | sed 's/(.*//; s/:any//; s/[[:space:]]//g' | sed '/^$/d' | LC_ALL=C sort -u; }
# The symbol version tags in `objdump -T` output on stdin, sorted and unique.
# Output without any tag gives an empty list rather than grep's status 1.
symbol_tags() {
    { grep -oE '\b(GLIBC|GLIBCXX|CXXABI|GCC|Qt)_[0-9][0-9A-Z_.]*' || (($? == 1)); } | LC_ALL=C sort -u
}

c_install() {
    local deb=$1 version before after
    export DEBIAN_FRONTEND=noninteractive
    . /etc/os-release
    step "Install on a clean ${PRETTY_NAME:-target} container"
    version=$(dpkg-deb -f "$deb" Version)
    before=$(dpkg-query -W -f='${Package}\t${Version}\n' | sort)
    apt-get update -q >/dev/null
    if ! apt-get install -y -q --no-install-recommends "$deb" > /tmp/install.log 2>&1; then
        cat /tmp/install.log; fail failures "apt install"; return 1
    fi
    after=$(dpkg-query -W -f='${Package}\t${Version}\n' | sort)
    pass "apt-get install --no-install-recommends added $(comm -13 <(echo "$before") <(echo "$after") | wc -l) packages"
    [[ "$(dpkg-query -W -f='${db:Status-Status} ${Version}' branch-truce)" == "installed $version" ]] \
        && pass "branch-truce $version installed" || fail failures "branch-truce $version not installed"
    step "Runtime closure"
    comm -13 <(echo "$before") <(echo "$after")
    printf 'kernel\t%s (shared with the host)\n' "$(uname -r)"
    step "Validation tools (not part of the closure)"
    local tools=(binutils libglib2.0-bin dbus-daemon at-spi2-core gir1.2-atspi-2.0 python3-gi wtype)
    if apt-get install -y -q --no-install-recommends "${tools[@]}" > /tmp/tools.log 2>&1; then
        dpkg-query -W -f='${Package}\t${Version}\n' "${tools[@]}"
    else
        cat /tmp/tools.log; fail failures "validation tools did not install"; return 1
    fi
    mkdir -p "$home"
    dbus-daemon --session --fork --print-address=3 3> "$home/dbus.address"
    [[ -s "$home/dbus.address" ]] && pass "private D-Bus session bus" || fail failures "D-Bus session bus"
}

c_closure() {
    local depends
    depends=$(dpkg-query -W -f='${Depends}' branch-truce)
    step "ELF closure of /usr/bin/branch-truce"
    echo "Depends: $depends"
    check_elf_closure /usr/bin/branch-truce "$depends"
    step "QML closure"
    check_qml_closure /validate/imports.txt "$depends"
}

# check_elf_closure BINARY DEPENDS: every NEEDED library of BINARY resolves and
# comes from a package DEPENDS names itself, not only through another package.
# dpkg may register a library under /lib or /usr/lib on merged-/usr systems, so
# each form is looked up.
check_elf_closure() {
    local binary=$1 names needed runpath lib path real owner before=${failures:-0}
    names=$(depends_names "$2")
    needed=$(readelf -d "$binary" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
    runpath=$(readelf -d "$binary" | sed -n 's/.*(\(RPATH\|RUNPATH\)).*\[\(.*\)\]/\2/p')
    echo "RUNPATH: ${runpath:-none}"
    for lib in $needed; do
        path=$(ldd "$binary" | awk -v lib="$lib" '$1 == lib {print $3}')
        [[ -n "$path" && "$path" != 'not' ]] || { fail failures "$lib does not resolve"; continue; }
        real=$(readlink -f "$path")
        owner=$(owner_of "$real")
        [[ -n "$owner" ]] || owner=$(owner_of "$path")
        [[ -n "$owner" ]] || owner=$(owner_of "${real/#\/usr\/lib\///lib/}")
        printf '%s\t%s\t%s\n' "$lib" "$path" "${owner:-no package}"
        grep -qx -- "$owner" <<<"$names" || fail failures "$lib comes from ${owner:-no package}, which Depends does not name"
    done
    if ((${failures:-0} == before)); then pass "$(wc -w <<<"$needed") NEEDED libraries come from packages named in Depends"; fi
    echo "Highest required symbol versions:"
    local tags namespace
    tags=$(objdump -T "$binary" | symbol_tags)
    for namespace in GLIBC GLIBCXX CXXABI GCC; do
        grep "^${namespace}_[0-9.]*$" <<<"$tags" | sort -V | tail -1 | sed 's/^/  /' || true
    done
    echo "Qt symbol version tags: $(grep '^Qt_' <<<"$tags" | tr '\n' ' ' || true)"
}

# check_qml_closure IMPORTS DEPENDS: each module listed in the file IMPORTS has
# a qmldir under $qml_root from a package DEPENDS names.
check_qml_closure() {
    local imports=$1 names module dir owner before=${failures:-0}
    names=$(depends_names "$2")
    while IFS= read -r module; do
        dir=$qml_root/${module//.//}
        if [[ -f "$dir/qmldir" ]]; then
            owner=$(owner_of "$dir/qmldir")
            printf '%s\t%s\t%s\n' "$module" "$dir/qmldir" "${owner:-no package}"
            grep -qx -- "$owner" <<<"$names" || fail failures "QML $module comes from ${owner:-no package}, which Depends does not name"
        else
            fail failures "QML $module has no $dir/qmldir"
        fi
    done < "$imports"
    if ((${failures:-0} == before)); then pass "$(wc -l < "$imports") imported QML modules come from packages named in Depends"; fi
}

c_layout() {
    step "No source or build tree"
    local found
    # The application's own QML file names, CMake files and the build path.
    found=$(find / -xdev \( -name CMakeCache.txt -o -name CMakeLists.txt -o -name Main.qml \
        -o -name ComparisonView.qml -o -path /work -o -name 'branch-truce-*' -type d \) -print 2>/dev/null || true)
    [[ -z "$found" ]] && pass "no CMake file, application QML source or /work on the system" \
        || { echo "$found"; fail failures "build or source files present"; }
    echo "branch-truce files: $(dpkg -L branch-truce | while read -r f; do [[ -f "$f" ]] && echo "$f"; done | tr '\n' ' ')"
}

c_cli() {
    step "Command line of the installed binary"
    fresh_fixture
    local out
    # The application runs with a deadline, so a hang fails the step.
    out=$(isolated "${offscreen_env[@]}" timeout 60 branch-truce --version 2>&1) \
        && pass "branch-truce --version: $out" || fail failures "--version: $out"
    isolated "${offscreen_env[@]}" timeout 60 branch-truce --help > /tmp/help.txt 2>&1 \
        && pass "branch-truce --help ($(wc -l < /tmp/help.txt) lines)" || fail failures "--help"
    isolated "${offscreen_env[@]}" QML_IMPORT_TRACE=1 QT_DEBUG_PLUGINS=1 \
        timeout 60 branch-truce --smoke-test "$repo" > /tmp/smoke.log 2>&1 || fail failures "smoke test exited $?"
    echo "session $(sed -n 's/^BRANCH_TRUCE_SESSION=//p' /tmp/smoke.log)"
    smoke_record_ready "$(cat /tmp/smoke.log)" \
        && pass "--smoke-test opened the merge session" || fail failures "--smoke-test did not open the session"
    check_loaded_resources /tmp/smoke.log
}

# check_loaded_resources LOG: the application QML in LOG, the output of a run
# with QML_IMPORT_TRACE and QT_DEBUG_PLUGINS, came from the binary's resources,
# and every qmldir and plugin it loaded from the file system belongs to an
# installed package. A search that finds nothing is a failed check, never an
# early exit.
check_loaded_resources() {
    local log=$1 paths path owner unowned=0 embedded
    step "Installed resource lookup during --smoke-test"
    embedded=$({ grep -oE '(qrc)?:/qt/qml/BranchTruce/[^ "]*' "$log" || true; } | sort -u | tr '\n' ' ')
    echo "embedded QML: ${embedded:-none}"
    [[ -n "$embedded" ]] && pass "application QML comes from the binary's resources" || fail failures "no embedded BranchTruce module"
    paths=$({ grep -oE '(^|[^:A-Za-z0-9_.])/[^ "():]*(qmldir|\.so)' "$log" || true; } | sed 's/^[^/]*//' | sort -u)
    while IFS= read -r path; do
        [[ -n "$path" && -e "$path" ]] || continue
        owner=$(owner_of "$path")
        [[ -n "$owner" ]] || owner=$(owner_of "$(readlink -f "$path")")
        [[ -n "$owner" ]] || { echo "unowned $path"; unowned=$((unowned + 1)); }
    done <<<"$paths"
    echo "loaded from the file system: $(grep -c qmldir <<<"$paths" || true) qmldir, $(grep -c '\.so$' <<<"$paths" || true) plugins"
    grep -E '/(platforms|styles|platformthemes)/' <<<"$paths" | sed 's/^/  /' || true
    ((unowned == 0)) && pass "every loaded qmldir and plugin belongs to an installed package" \
        || fail failures "$unowned loaded files belong to no package"
}

c_desktop() {
    step "Desktop-entry launch (offscreen)"
    fresh_fixture
    local exec_line pid
    exec_line=$(sed -n 's/^Exec=//p' "/usr/share/applications/$app_id.desktop")
    echo "Exec=$exec_line"
    (private "${offscreen_env[@]}" \
        setsid gio launch "/usr/share/applications/$app_id.desktop" "$repo" > /tmp/desktop.log 2>&1 < /dev/null &)
    if wait_until 60 session_socket_present "$home"; then
        pid=$(gui_pids | head -1)
        echo "process $(tr '\0' ' ' < "/proc/$pid/cmdline"); executable $(readlink "/proc/$pid/exe")"
        pass "gio launch of $app_id.desktop opened the session and listens for the mergetool"
    else
        cat /tmp/desktop.log; fail failures "desktop-entry launch did not open the session"
    fi
    stop_gui || fail failures "GUI did not stop"
}

c_mergetool_leave() {
    step "Mergetool handoff with real Git, GUI ends without a decision (offscreen)"
    fresh_fixture
    local before
    before=$(sha256sum < "$repo/tracked.txt")
    echo "git $(isolated git --version)"
    (isolated "${offscreen_env[@]}" setsid bash -c \
        'git mergetool --tool truce --no-prompt tracked.txt > "$0/mergetool.log" 2>&1; echo $? > "$0/mergetool.status"' \
        "$home" < /dev/null &)
    if wait_until 60 session_socket_present "$home" && wait_until 30 intake_present; then
        pass "the launcher started $(tr '\0' ' ' < "/proc/$(gui_pids | head -1)/cmdline")and attached tracked.txt"
    else
        fail failures "no launcher-started session took the path"
    fi
    stop_gui || fail failures "GUI did not stop"
    wait_until 30 mergetool_done || fail failures "git mergetool did not finish"
    sed 's/^/  /' "$home/mergetool.log"
    local status
    status=$(cat "$home/mergetool.status" 2>/dev/null || echo none)
    [[ "$status" != 0 && "$status" != none ]] && pass "git mergetool exited $status" || fail failures "git mergetool exited $status"
    [[ "$(sha256sum < "$repo/tracked.txt")" == "$before" ]] \
        && pass "MERGED has its launch bytes again" || fail failures "MERGED changed"
    [[ "$(isolated git ls-files -u -- tracked.txt | wc -l)" == 3 ]] \
        && pass "tracked.txt stays unmerged in the index" || fail failures "index changed"
}

c_wayland_minimal() {
    step "Wayland with the installed closure only"
    fresh_fixture
    local status=0
    private "QT_QPA_PLATFORM=wayland" "WAYLAND_DISPLAY=$wayland_socket" \
        timeout 60 branch-truce --smoke-test "$repo" > /tmp/wayland-minimal.log 2>&1 || status=$?
    grep -E 'qt.qpa|platform plugin|BRANCH_TRUCE_SESSION' /tmp/wayland-minimal.log | cut -c1-200 | sed 's/^/  /' || true
    echo "qt6-wayland: $(dpkg-query -W -f='${db:Status-Status} ${Version}' qt6-wayland 2>/dev/null || echo not-installed)"
    if ((status == 0)); then
        pass "--smoke-test on Wayland with the closure alone"
    else
        fail failures "--smoke-test on Wayland exited $status with the closure alone"
    fi
}

c_session() {
    step "Session packages a Plasma desktop has"
    local packages=(plasma-integration)
    if DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends "${packages[@]}" > /tmp/session.log 2>&1; then
        dpkg-query -W -f='${Package}\t${Version}\n' "${packages[@]}"
        pass "session packages installed"
    else
        cat /tmp/session.log; fail failures "session packages"
    fi
}

# gui-start SCALE SCHEME: the desktop entry, on Wayland with that scale and Breeze scheme.
c_gui_start() {
    local scale=$1 scheme=$2 launch=(gio launch "/usr/share/applications/$app_id.desktop" "$repo")
    fresh_fixture
    { cat "/usr/share/color-schemes/$scheme.colors"; printf '\n[General]\nColorScheme=%s\n' "$scheme"; } \
        > "$home/config/kdeglobals"
    (private "${wayland_env[@]}" "QT_SCALE_FACTOR=$scale" \
        "DBUS_SESSION_BUS_ADDRESS=$(cat "$home/dbus.address")" \
        setsid "${launch[@]}" > "$home/gui.log" 2>&1 < /dev/null &)
    echo "started ${launch[*]} with QT_SCALE_FACTOR=$scale, colour scheme $scheme"
}

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
        grep -qE -- "^ *$name \|" <<<"$tree" && pass "accessible ${name//\\/}" || fail failures "no accessible ${name//\\/}"
    done
    echo "objects $(wc -l <<<"$tree"); unnamed buttons: $(grep -cE '^ *(push-)?button \|  \|' <<<"$tree" || true)"
    grep -E '^ *text \| (Left source|Right source)' <<<"$tree" | grep -q read_only \
        && pass "source panes report read_only" || fail failures "source panes do not report read_only"
}

c_editor_input() {
    step "Editor input through the Wayland virtual keyboard"
    local focus text want="text '$result_text\\u2029'" deadline
    if ! focus=$(probe focus 'Editable result' 2>&1); then
        fail failures "AT-SPI focus on the editable result: $focus"
        return 0
    fi
    echo "$focus"
    # The first keys after wtype uploads its keymap can be lost, so it waits.
    if ! env "WAYLAND_DISPLAY=$wayland_socket" wtype -s 400 -M ctrl a -m ctrl -s 100 "$result_text" -k Return; then
        fail failures "wtype"
        return 0
    fi
    pass "wtype sent Ctrl+A, '$result_text' and Return"
    # The editor gets the keys after wtype returns, so read the text until it holds them.
    deadline=$((SECONDS + 10))
    until text=$(probe text 'Editable result' 2>/dev/null); [[ "$text" == *"$want"* ]] || ((SECONDS >= deadline)); do
        sleep 0.2
    done
    echo "$text"
    [[ "$text" == *"$want"* ]] \
        && pass "the result holds the typed text, accented letters included" || fail failures "the result does not hold the typed text"
}

c_mergetool_start() {
    step "Mergetool handoff with real Git on Wayland"
    fresh_fixture
    cp "/usr/share/color-schemes/BreezeLight.colors" "$home/config/kdeglobals"
    (isolated "${wayland_env[@]}" "QT_SCALE_FACTOR=1" \
        "DBUS_SESSION_BUS_ADDRESS=$(cat "$home/dbus.address")" setsid bash -c \
        'git mergetool --tool truce --no-prompt tracked.txt > "$0/mergetool.log" 2>&1; echo $? > "$0/mergetool.status"' \
        "$home" < /dev/null &)
    echo "started git mergetool --tool truce"
}

c_mergetool_resolve() {
    c_wait_ready || { fail failures "session not ready"; return 0; }
    c_editor_input
    local pressed
    pressed=$(probe press 'Mark resolved' 2>&1) || { fail failures "press Mark resolved: $pressed"; return 0; }
    echo "$pressed"
    probe wait 'Confirm resolution' 30 || { fail failures "no Confirm resolution"; return 0; }
    pressed=$(probe press 'Confirm resolution' 2>&1) || { fail failures "press Confirm resolution: $pressed"; return 0; }
    echo "$pressed"
    wait_until 60 mergetool_done || fail failures "git mergetool did not finish"
    sed 's/^/  /' "$home/mergetool.log"
    local status expected staged
    status=$(cat "$home/mergetool.status" 2>/dev/null || echo none)
    [[ "$status" == 0 ]] && pass "git mergetool exited 0" || fail failures "git mergetool exited $status"
    expected=$(printf '%s\n' "$result_text" | isolated git hash-object --stdin)
    staged=$(isolated git ls-files -s -- tracked.txt)
    echo "index $staged"
    [[ "$staged" == "100644 $expected 0"$'\t'tracked.txt ]] \
        && pass "Git staged the reviewed text $expected" || fail failures "index does not hold the reviewed text"
    [[ "$(isolated git hash-object tracked.txt)" == "$expected" ]] \
        && pass "MERGED holds the reviewed bytes" || fail failures "MERGED differs from the reviewed text"
    # The session polls the index for the staged object before it claims the file. Match the
    # resolved count, not the whole progress label, which other words surround and which moves.
    probe wait-match '(?<![0-9])1 resolved\b' 60 \
        && pass "the session reports tracked.txt resolved after Git staged it" \
        || fail failures "the session did not report tracked.txt resolved"
}

c_gui_stop() { stop_gui || fail failures "GUI did not stop"; }

# installed_checks_main STEP [ARG...]: runs c_STEP, with - in STEP read as _,
# and returns 1 when one of its checks failed.
installed_checks_main() {
    local failures=0 step=c_${1-}
    step=${step//-/_}
    declare -F -- "$step" >/dev/null || { usage >&2; return 2; }
    shift
    if [[ "$step" == c_install ]]; then
        fixture=$(fixture_root) || return 1
        (set -o noclobber; printf '%s\n' "$fixture" > /run/bt-installed-fixture-root) || return 1
    else
        fixture=$(cat /run/bt-installed-fixture-root) || return 1
    fi
    home=$fixture/home repo=$fixture/repo
    fixture_path "$fixture" "$home" && fixture_path "$fixture" "$repo" || return 1
    "$step" "$@"
    ((${failures:-0} == 0))
}

[[ "${BASH_SOURCE[0]}" == "$0" ]] || return 0
set -euo pipefail
installed_checks_main "$@"
