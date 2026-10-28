#!/usr/bin/env bash
# Install this checkout into a disposable prefix and launch only installed files.
# The copied sources and build tree are deleted before any launch. Nothing is
# installed into the system or the caller's home; the fixture is removed on exit.
#
# Usage: tools/verify-install-prefix.sh [--mergetool-tests PATH]
# PATH is a built branch_truce_mergetool_tests binary. When given, it runs
# against the installed executable to cover the resolved (exit 0) handoff.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
app_id=com.canpicasoft.branch-truce
mergetool_tests=
case "${1:-}" in
    '') ;;
    --mergetool-tests)
        [[ $# -eq 2 && -x "$2" ]] || { echo "Usage: $0 [--mergetool-tests PATH]" >&2; exit 2; }
        mergetool_tests=$(cd -- "$(dirname -- "$2")" && pwd -P)/$(basename -- "$2") ;;
    *) echo "Usage: $0 [--mergetool-tests PATH]" >&2; exit 2 ;;
esac
for tool in cmake c++ make git tar gio desktop-file-validate appstreamcli readelf; do
    command -v "$tool" >/dev/null || { echo "Missing prerequisite: $tool" >&2; exit 1; }
done

fixture=$(mktemp -d "${TMPDIR:-/tmp}/bt-install.XXXXXXXX")
source_dir="$fixture/source"
build="$fixture/build"
prefix="$fixture/prefix"
home="$fixture/home"
repo="$fixture/repo"
installed="$prefix/bin/branch-truce"

installed_pids() {
    local exe pid
    for exe in /proc/[0-9]*/exe; do
        pid=${exe#/proc/}; pid=${pid%/exe}
        [[ "$(readlink -- "$exe" 2>/dev/null)" == "$installed" ]] && printf '%s\n' "$pid"
    done
    return 0
}
# GUI processes run the installed executable without --mergetool.
gui_pids() {
    local pid
    for pid in $(installed_pids); do
        tr '\0' '\n' < "/proc/$pid/cmdline" 2>/dev/null | grep -qx -- '--mergetool' || printf '%s\n' "$pid"
    done
    return 0
}
cleanup() {
    local status=$? pid
    trap - EXIT
    for pid in $(installed_pids); do kill -KILL "$pid" 2>/dev/null || true; done
    rm -rf -- "$fixture"
    if [[ -e "$fixture" ]]; then
        echo "Cleanup failed; retained fixture: $fixture" >&2
        exit 1
    fi
    echo "Install-prefix fixture removed: $fixture"
    exit "$status"
}
trap cleanup EXIT

step() { printf '\n== %s\n' "$*"; }
show() { printf '+'; printf ' %q' "$@"; printf '\n'; }
run() { show "$@"; "$@"; }
pass() { printf 'PASS: %s\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
# Bounded wait for a condition; the deadline is in tenths of a second.
wait_for() {
    local tenths=$1; shift
    while ! "$@"; do
        ((tenths-- > 0)) || return 1
        sleep 0.1
    done
}

step "Copy tracked and untracked non-ignored files outside the checkout"
mkdir -p -- "$source_dir"
(cd -- "$root" && git ls-files -z --cached --others --exclude-standard) |
    while IFS= read -r -d '' file; do [[ -e "$root/$file" ]] && printf '%s\0' "$file"; done |
    tar -C "$root" --null -T - -cf - | tar -C "$source_dir" -xf -
echo "Copied source: $source_dir (from $root at $(git -C "$root" rev-parse --short HEAD))"

step "Configure, build and install"
run cmake -S "$source_dir" -B "$build" -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF -DBRANCH_TRUCE_BUILD_DESKTOP=ON -DBRANCH_TRUCE_WARNINGS_AS_ERRORS=ON \
    "-DCMAKE_INSTALL_PREFIX=$prefix"
run cmake --build "$build" --parallel 2
run cmake --install "$build"
run rm -rf -- "$source_dir" "$build"
[[ ! -e "$source_dir" && ! -e "$build" ]] || fail "copied source or build tree still exists"
pass "copied source and build tree deleted before launch"

step "Inspect deployed files"
expected=(
    "bin/branch-truce"
    "share/applications/$app_id.desktop"
    "share/icons/hicolor/scalable/apps/$app_id.svg"
    "share/metainfo/$app_id.metainfo.xml"
)
deployed=$(cd -- "$prefix" && find . \( -type f -o -type l \) -printf '%P\n' | LC_ALL=C sort)
printf '%s\n' "$deployed"
[[ "$deployed" == "$(printf '%s\n' "${expected[@]}" | LC_ALL=C sort)" ]] || fail "deployed file list differs"
pass "deployed exactly ${#expected[@]} files"
runpath=$(readelf -d "$installed" | sed -n 's/.*(\(RPATH\|RUNPATH\)).*\[\(.*\)\]/\2/p')
echo "RUNPATH: ${runpath:-none}"
[[ "$runpath" != *"$fixture"* ]] || fail "RUNPATH points into the fixture"
run desktop-file-validate "$prefix/share/applications/$app_id.desktop"
# No public homepage exists yet; keep that one finding visible as info.
run appstreamcli validate --no-net --override=url-homepage-missing=info \
    "$prefix/share/metainfo/$app_id.metainfo.xml"
pass "desktop entry and metainfo validate"

step "Create a disposable merge conflict"
# The session socket is $XDG_RUNTIME_DIR/branch-truce/bt-mt-<32 hex>. A longer
# path than Qt accepts (106 bytes) moves it to /tmp/branch-truce-<uid>.
((${#home} + 8 + 13 + 39 <= 106)) || fail "fixture path too long for the session socket: $home/runtime"
mkdir -p -- "$home/data" "$home/config" "$home/cache" "$home/tmp" "$home/runtime" "$repo"
chmod 700 -- "$home/runtime"
environment=(
    "PATH=$prefix/bin:/usr/bin:/bin" "HOME=$home" "TMPDIR=$home/tmp" "LC_ALL=C.UTF-8"
    "XDG_DATA_HOME=$home/data" "XDG_CONFIG_HOME=$home/config" "XDG_CACHE_HOME=$home/cache"
    "XDG_RUNTIME_DIR=$home/runtime" "XDG_DATA_DIRS=$prefix/share:/usr/share"
    "GIT_CONFIG_NOSYSTEM=1" "GIT_CONFIG_GLOBAL=/dev/null" "GIT_TERMINAL_PROMPT=0"
    "QT_QPA_PLATFORM=offscreen" "QT_QUICK_BACKEND=software"
)
isolated() { (cd -- "$repo" && env -i "${environment[@]}" "$@"); }
fixture_git() { isolated git --no-pager "$@" >/dev/null; }
fixture_git init --quiet --initial-branch=main
fixture_git config user.name 'Branch Truce Install Check'
fixture_git config user.email 'install@example.invalid'
fixture_git config core.autocrlf false
printf 'base\n' > "$repo/tracked.txt"
fixture_git add -- tracked.txt
fixture_git commit --quiet -m base
fixture_git checkout --quiet -b side
printf 'side\n' > "$repo/tracked.txt"
fixture_git commit --quiet -am side
fixture_git checkout --quiet main
printf 'main\n' > "$repo/tracked.txt"
fixture_git commit --quiet -am main
isolated git merge --no-edit side >/dev/null 2>&1 && fail "fixture merge did not conflict"
launch_bytes=$(sha256sum < "$repo/tracked.txt")
resolved_name=$(isolated sh -c 'command -v branch-truce')
[[ "$resolved_name" == "$installed" ]] || fail "branch-truce resolves to $resolved_name"
pass "conflicted repository $repo; branch-truce resolves to $resolved_name"
unmerged() { [[ -n "$(isolated git ls-files -u -- tracked.txt)" ]]; }
socket_present() { compgen -G "$home/runtime/branch-truce/bt-mt-*" >/dev/null; }
gui_running() { [[ -n "$(gui_pids)" ]]; }
no_gui() { [[ -z "$(gui_pids)" ]]; }
stop_gui() {
    local pid
    for pid in $(gui_pids); do kill -TERM "$pid"; done
    wait_for 100 no_gui || fail "installed GUI did not stop"
}
# A blocked launcher still runs, MERGED keeps its launch bytes and Git still
# reports the path unmerged.
expect_blocked() {
    sleep 3
    kill -0 "$1" 2>/dev/null || fail "$2 returned before a decision"
    [[ "$(sha256sum < "$repo/tracked.txt")" == "$launch_bytes" ]] || fail "MERGED changed while blocked"
    unmerged || fail "tracked.txt was staged while blocked"
}

step "CLI launch from the installed prefix"
show env -i "${environment[@]}" branch-truce --version
isolated branch-truce --version
show env -i "${environment[@]}" branch-truce --smoke-test "$repo"
smoke=$(isolated branch-truce --smoke-test "$repo")
printf '%s\n' "$smoke"
record=$(printf '%s\n' "$smoke" | sed -n 's/^BRANCH_TRUCE_SESSION=//p')
[[ "$record" == *'"state":"ready"'* && "$record" == *'"operation":"Merge"'* \
    && "$record" == *'"path":"tracked.txt"'* ]] || fail "smoke test did not open the merge session"
pass "installed CLI loaded the embedded QML and opened the merge session"

step "Desktop-entry launch and direct launcher handoff"
show env -i "${environment[@]}" gio launch "$prefix/share/applications/$app_id.desktop" "$repo"
isolated gio launch "$prefix/share/applications/$app_id.desktop" "$repo"
wait_for 300 socket_present || fail "desktop-launched GUI did not open a session socket"
gui_running || fail "desktop-launched GUI is not the installed executable"
pass "desktop entry started $(gui_pids | head -1 | xargs -I{} readlink /proc/{}/exe) with a live session"
sides="$fixture/sides"
mkdir -p -- "$sides"
printf 'base\n' > "$sides/base"; printf 'main\n' > "$sides/local"; printf 'side\n' > "$sides/remote"
launcher_env=("MERGED=tracked.txt" "BASE=$sides/base" "LOCAL=$sides/local" "REMOTE=$sides/remote"
    "BRANCH_TRUCE_MERGETOOL_NO_SPAWN=1")
show env -i "${environment[@]}" "${launcher_env[@]}" branch-truce --mergetool
isolated env "${launcher_env[@]}" branch-truce --mergetool &
launcher=$!
expect_blocked "$launcher" "installed launcher"
pass "installed launcher attached to the desktop-launched GUI and is blocking"
intakes=$(find "$home/data/branch-truce/mergetool" -mindepth 2 -maxdepth 2 -type d | wc -l)
((intakes == 1)) || fail "expected one launcher intake, found $intakes"
stop_gui
status=0; wait "$launcher" || status=$?
((status != 0)) || fail "launcher returned success after the GUI closed"
[[ "$(sha256sum < "$repo/tracked.txt")" == "$launch_bytes" ]] || fail "MERGED was not restored"
unmerged || fail "tracked.txt was staged without a resolution"
pass "closing the GUI released the launcher with exit $status and left tracked.txt unmerged"

step "Git mergetool parent spawns the installed GUI"
rm -f -- "$home"/runtime/branch-truce/bt-mt-*
fixture_git config mergetool.truce.cmd \
    'MERGED="$MERGED" BASE="$BASE" LOCAL="$LOCAL" REMOTE="$REMOTE" branch-truce --mergetool'
fixture_git config mergetool.truce.trustExitCode true
fixture_git config mergetool.keepBackup false
show env -i "${environment[@]}" git mergetool --no-prompt --tool=truce -- tracked.txt
isolated git --no-pager mergetool --no-prompt --tool=truce -- tracked.txt < /dev/null &
parent=$!
wait_for 300 gui_running || fail "launcher did not start the installed GUI"
wait_for 300 socket_present || fail "spawned GUI did not open a session socket"
expect_blocked "$parent" "git mergetool"
pass "launcher started $(gui_pids | head -1 | xargs -I{} readlink /proc/{}/exe) and git mergetool is blocking"
stop_gui
status=0; wait "$parent" || status=$?
((status != 0)) || fail "git mergetool succeeded without a resolution"
[[ "$(sha256sum < "$repo/tracked.txt")" == "$launch_bytes" ]] || fail "Git did not restore MERGED"
unmerged || fail "Git staged tracked.txt without a resolution"
pass "git mergetool exited $status after the GUI closed; tracked.txt stays unmerged"

if [[ -n "$mergetool_tests" ]]; then
    step "Mergetool protocol tests against the installed executable"
    # The suite isolates each fixture's environment itself. It hosts the session
    # in-process, and its launchers do not inherit TMPDIR, so keep the default.
    run env -i "PATH=/usr/bin:/bin" "HOME=$home" "LC_ALL=C.UTF-8" "$mergetool_tests" "$installed"
    pass "mergetool protocol tests passed with $installed"
fi
printf '\nInstall-prefix verification passed: %s\n' "$prefix"
