#!/usr/bin/env bash
# Install this checkout into a disposable prefix and launch only installed files.
# The copied sources and build tree are deleted before any launch. Nothing is
# installed into the system or the caller's home; the fixture is removed on exit.
#
# Usage: tools/verify-install-prefix.sh [--mergetool-tests PATH]
# PATH is a built branch_truce_mergetool_tests binary. When given, it runs
# against the installed executable to cover the resolved (exit 0) handoff.
# Exit status: 0 passed, 1 a check failed, 2 usage error.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
source "$root/tools/lib/fixture.sh"
source "$root/packaging/scripts/compiler-cache.sh"
app_id=com.canpicasoft.branch-truce
mergetool_tests=
case "${1:-}" in
    '') ;;
    -h|--help) echo "Usage: $0 [--mergetool-tests PATH]"; exit 0 ;;
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
# die stops the run at a failed check; fail, in the scripts that keep going,
# only records one.
die() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
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

cache_begin install-prefix-Release "$fixture"
mapfile -t cache_args < <(cache_cmake_args)
step "Configure, build and install"
run cmake -S "$source_dir" -B "$build" -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF -DBRANCH_TRUCE_BUILD_DESKTOP=ON -DBRANCH_TRUCE_WARNINGS_AS_ERRORS=ON \
    "-DCMAKE_INSTALL_PREFIX=$prefix" "${cache_args[@]}"
cache_build_status=0
run cmake --build "$build" --parallel 2 || cache_build_status=$?
cache_end
[[ "$cache_build_status" -eq 0 ]] || exit "$cache_build_status"
run cmake --install "$build"
run rm -rf -- "$source_dir" "$build"
[[ ! -e "$source_dir" && ! -e "$build" ]] || die "copied source or build tree still exists"
pass "copied source and build tree deleted before launch"

step "Inspect deployed files"
mapfile -t expected < <(installed_files "$root/packaging/installed-files.txt" '' | sed 's|^/||')
deployed=$(cd -- "$prefix" && find . \( -type f -o -type l \) -printf '%P\n' | LC_ALL=C sort)
printf '%s\n' "$deployed"
[[ "$deployed" == "$(printf '%s\n' "${expected[@]}" | LC_ALL=C sort)" ]] || die "deployed file list differs"
pass "deployed exactly ${#expected[@]} files"
runpath=$(readelf -d "$installed" | sed -n 's/.*(\(RPATH\|RUNPATH\)).*\[\(.*\)\]/\2/p')
echo "RUNPATH: ${runpath:-none}"
[[ "$runpath" != *"$fixture"* ]] || die "RUNPATH points into the fixture"
run desktop-file-validate "$prefix/share/applications/$app_id.desktop"
run appstreamcli validate --no-net \
    "$prefix/share/metainfo/$app_id.metainfo.xml"
pass "desktop entry and metainfo validate"

step "Create a disposable merge conflict"
# The session socket is $XDG_RUNTIME_DIR/branch-truce/bt-mt-<32 hex>. A longer
# path than Qt accepts (106 bytes) moves it to /tmp/branch-truce-<uid>.
((${#home} + 8 + 13 + 39 <= 106)) || die "fixture path too long for the session socket: $home/runtime"
fixture_home "$home"
fixture_gitconfig "$home"
mapfile -d '' environment < <(fixture_env "$home")
environment+=("PATH=$prefix/bin:/usr/bin:/bin" "XDG_DATA_DIRS=$prefix/share:/usr/share"
    "QT_QPA_PLATFORM=offscreen" "QT_QUICK_BACKEND=software")
isolated() { (cd -- "$repo" && env -i "${environment[@]}" "$@"); }
fixture_merge_repo "$home" "$repo" tracked.txt || die "fixture merge did not conflict on tracked.txt"
launch_bytes=$(sha256sum < "$repo/tracked.txt")
resolved_name=$(isolated sh -c 'command -v branch-truce')
[[ "$resolved_name" == "$installed" ]] || die "branch-truce resolves to $resolved_name"
pass "conflicted repository $repo; branch-truce resolves to $resolved_name"
unmerged() { [[ -n "$(isolated git ls-files -u -- tracked.txt)" ]]; }
socket_present() { compgen -G "$home/runtime/branch-truce/bt-mt-*" >/dev/null; }
gui_running() { [[ -n "$(gui_pids)" ]]; }
no_gui() { [[ -z "$(gui_pids)" ]]; }
stop_gui() {
    local pid
    for pid in $(gui_pids); do kill -TERM "$pid"; done
    wait_for 100 no_gui || die "installed GUI did not stop"
}
# A blocked launcher still runs, MERGED keeps its launch bytes and Git still
# reports the path unmerged.
expect_blocked() {
    sleep 3
    kill -0 "$1" 2>/dev/null || die "$2 returned before a decision"
    [[ "$(sha256sum < "$repo/tracked.txt")" == "$launch_bytes" ]] || die "MERGED changed while blocked"
    unmerged || die "tracked.txt was staged while blocked"
}

step "CLI launch from the installed prefix"
# The application runs with a deadline, so a hang fails the check.
show env -i "${environment[@]}" branch-truce --version
isolated timeout --kill-after=10 60 branch-truce --version || die "installed --version exited $?"
show env -i "${environment[@]}" branch-truce --smoke-test "$repo"
smoke=$(isolated timeout --kill-after=10 60 branch-truce --smoke-test "$repo") || die "installed --smoke-test exited $?"
printf '%s\n' "$smoke"
smoke_record_ready "$smoke" || die "smoke test did not open the merge session"
pass "installed CLI loaded the embedded QML and opened the merge session"

step "Desktop-entry launch and direct launcher handoff"
show env -i "${environment[@]}" gio launch "$prefix/share/applications/$app_id.desktop" "$repo"
isolated gio launch "$prefix/share/applications/$app_id.desktop" "$repo"
wait_for 300 socket_present || die "desktop-launched GUI did not open a session socket"
gui_running || die "desktop-launched GUI is not the installed executable"
pass "desktop entry started $(gui_pids | head -1 | xargs -I{} readlink /proc/{}/exe) with a live session"
sides="$fixture/sides"
mkdir -p -- "$sides"
printf 'base\n' > "$sides/base"; printf 'main\n' > "$sides/local"; printf 'side\n' > "$sides/remote"
launcher_env=("MERGED=tracked.txt" "BASE=$sides/base" "LOCAL=$sides/local" "REMOTE=$sides/remote")
show env -i "${environment[@]}" "${launcher_env[@]}" branch-truce --mergetool --no-window
isolated env "${launcher_env[@]}" branch-truce --mergetool --no-window &
launcher=$!
expect_blocked "$launcher" "installed launcher"
pass "installed launcher attached to the desktop-launched GUI and is blocking"
intakes=$(find "$home/data/branch-truce/mergetool" -mindepth 2 -maxdepth 2 -type d | wc -l)
((intakes == 1)) || die "expected one launcher intake, found $intakes"
stop_gui
status=0; wait "$launcher" || status=$?
((status != 0)) || die "launcher returned success after the GUI closed"
[[ "$(sha256sum < "$repo/tracked.txt")" == "$launch_bytes" ]] || die "MERGED was not restored"
unmerged || die "tracked.txt was staged without a resolution"
pass "closing the GUI released the launcher with exit $status and left tracked.txt unmerged"

step "Git mergetool parent spawns the installed GUI"
rm -f -- "$home"/runtime/branch-truce/bt-mt-*
git_config() { isolated git config "$@"; }
git_config mergetool.truce.cmd \
    'MERGED="$MERGED" BASE="$BASE" LOCAL="$LOCAL" REMOTE="$REMOTE" branch-truce --mergetool'
git_config mergetool.truce.trustExitCode true
git_config mergetool.keepBackup false
show env -i "${environment[@]}" git mergetool --no-prompt --tool=truce -- tracked.txt
isolated git --no-pager mergetool --no-prompt --tool=truce -- tracked.txt < /dev/null &
parent=$!
wait_for 300 gui_running || die "launcher did not start the installed GUI"
wait_for 300 socket_present || die "spawned GUI did not open a session socket"
expect_blocked "$parent" "git mergetool"
pass "launcher started $(gui_pids | head -1 | xargs -I{} readlink /proc/{}/exe) and git mergetool is blocking"
stop_gui
status=0; wait "$parent" || status=$?
((status != 0)) || die "git mergetool succeeded without a resolution"
[[ "$(sha256sum < "$repo/tracked.txt")" == "$launch_bytes" ]] || die "Git did not restore MERGED"
unmerged || die "Git staged tracked.txt without a resolution"
pass "git mergetool exited $status after the GUI closed; tracked.txt stays unmerged"

if [[ -n "$mergetool_tests" ]]; then
    step "Mergetool protocol tests against the installed executable"
    # The suite isolates each fixture's environment itself. It hosts the session
    # in-process, and its launchers do not inherit TMPDIR, so keep the default.
    run env -i "PATH=/usr/bin:/bin" "HOME=$home" "LC_ALL=C.UTF-8" "$mergetool_tests" "$installed"
    pass "mergetool protocol tests passed with $installed"
fi
printf '\nInstall-prefix verification passed: %s\n' "$prefix"
