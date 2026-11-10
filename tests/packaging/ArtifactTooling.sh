#!/usr/bin/env bash
# Exercise the host side of tools/inspect-artifact.sh through CONTAINER_ENGINE
# set to a stub engine, and its container-side decisions with synthetic input.
# No Docker, network, root or installed package is used. The script gets TMPDIR
# inside this test's temporary directory and writes its output there; the
# directory is removed on exit.
#
# The in-container half (--in-container) is not run as a whole: it installs the
# package with apt. Its closure decisions are tested on their own by sourcing
# the script and feeding synthetic objdump -T output, Depends fields and qmldir
# trees to its functions, with stand-ins for dpkg-query.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
work=$(mktemp -d "${TMPDIR:-/tmp}/bt-artifact-tooling.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT
# The script prints physical paths.
work=$(cd -- "$work" && pwd -P)
export TMPDIR=$work/tmp
mkdir -p -- "$TMPDIR" "$work/bin" "$work/state"
assertions=0
output=
status=0

fail() { printf 'FAIL %s\n' "$*" >&2; printf 'Last output (exit %s):\n%s\n' "$status" "$output" >&2; exit 1; }
pass() { assertions=$((assertions + 1)); }
quoted() { printf '%q' "$1"; printf ' %q' "${@:2}"; }

# Runs a command with the stub engine, keeping its combined output and exit status.
invoke() {
    rm -rf -- "$work/state" && mkdir -p -- "$work/state"
    if output=$(CONTAINER_ENGINE=$work/bin/engine "$@" 2>&1); then status=0; else status=$?; fi
}
expect_status() {
    [[ "$status" == "$1" ]] || fail "expected exit $1"
    pass
}
expect_output() {
    [[ "$output" == *"$1"* ]] || fail "output lacks: $1"
    pass
}
expect_rejected() {
    invoke "$@"
    expect_status 2
    expect_output 'Usage:'
    [[ ! -s "$work/state/engine.log" ]] || fail "engine ran for rejected arguments: $(quoted "$@")"
    pass
}
# The script removes its fixture under TMPDIR on every exit path.
expect_no_fixture() {
    [[ -z "$(ls -A -- "$TMPDIR")" ]] || fail "fixtures left in TMPDIR: $(ls -A -- "$TMPDIR")"
    pass
}
engine_line() { sed -n "${1}p" "$work/state/engine.log"; }
engine_calls() { wc -l < "$work/state/engine.log"; }

# Stub engine: logs each argv; `build` fails with STUB_BUILD_STATUS; `run` keeps
# a copy of the /inspect volume and plays the inspection container.
cat > "$work/bin/engine" <<'ENGINE'
#!/usr/bin/env bash
set -euo pipefail
state=$STUB_STATE
{ printf '%q' "$1"; printf ' %q' "${@:2}"; printf '\n'; } >> "$state/engine.log"
case "$1" in
    build) exit "${STUB_BUILD_STATUS:-0}" ;;
    run)
        while (($# > 0)); do
            case "$1" in
                --volume)
                    case "$2" in
                        *:/inspect:ro)
                            printf '%s\n' "${2%:/inspect:ro}" > "$state/inspect-volume"
                            cp -a -- "${2%:/inspect:ro}" "$state/inspect" ;;
                    esac
                    shift 2 ;;
                *) shift ;;
            esac
        done
        printf '%s\n' "$STUB_REPORT"
        exit "${STUB_RUN_STATUS:-0}" ;;
    *) exit 9 ;;
esac
ENGINE
chmod +x "$work/bin/engine"
export STUB_STATE=$work/state

inspect=$root/tools/inspect-artifact.sh
mkdir -p -- "$work/debs"
deb_name=branch-truce_1.2.3_amd64-ubuntu26.04
printf 'not inspected here\n' > "$work/debs/$deb_name.deb"
image_build() {
    quoted build --quiet --target target --tag "branch-truce-validate:$1" --build-arg BRANCH_TRUCE_KDE_CI_REFRESH= \
        --file "$root/packaging/$2" "$root/packaging"
}

# --- argument handling
expect_rejected "$inspect"
expect_rejected "$inspect" --target kubuntu
expect_rejected "$inspect" --package "$work/debs/$deb_name.deb"
expect_rejected "$inspect" --target kubuntu --package "$work/debs/missing.deb"
expect_rejected "$inspect" --target kubuntu --package "$work/debs"
expect_rejected "$inspect" --target debian --package "$work/debs/$deb_name.deb"
expect_rejected "$inspect" --target kubuntu --package
expect_rejected "$inspect" --target kubuntu --package "$work/debs/$deb_name.deb" --output
expect_rejected "$inspect" --target kubuntu --package "$work/debs/$deb_name.deb" --verbose
expect_output 'error: unknown option --verbose'
expect_rejected "$inspect" --target debian --package "$work/debs/$deb_name.deb"
expect_output "error: validation-targets.tsv has no row 'debian'"
expect_rejected "$inspect" --in-container
expect_rejected "$inspect" --in-container /artifact/x.deb extra
expect_no_fixture

# A failed image build stops before any container runs.
STUB_BUILD_STATUS=1 invoke "$inspect" --target kubuntu --package "$work/debs/$deb_name.deb"
expect_status 1
expect_output 'error: the kubuntu image did not build from packaging/ci/kubuntu.Dockerfile'
[[ "$(engine_calls)" == 1 && "$(engine_line 1)" == "$(image_build kubuntu ci/kubuntu.Dockerfile)" ]] \
    || fail "failed image build engine calls: $(cat "$work/state/engine.log")"
pass
expect_no_fixture

# --- with the stub engine
# A relative package path is mounted by its absolute path; the report is kept.
report=$'== Result\nAll inspection checks passed for branch-truce 1.2.3'
if output=$(cd -- "$work" && rm -rf -- state && mkdir state && STUB_REPORT=$report \
    CONTAINER_ENGINE=$work/bin/engine "$inspect" --target tuxedo --package "debs/$deb_name.deb" --output reports 2>&1)
then status=0; else status=$?; fi
expect_status 0
expect_no_fixture
fixture=$(cat "$work/state/inspect-volume")
[[ "$fixture" == "$TMPDIR"/bt-inspect.*/inspect ]] || fail "inspection fixture $fixture is outside TMPDIR"
pass
[[ "$(engine_calls)" == 2 ]] || fail "expected 2 engine calls: $(cat "$work/state/engine.log")"
pass
# The target's image comes from its Dockerfile in validation-targets.tsv, and the inspection runs in it.
[[ "$(engine_line 1)" == "$(image_build tuxedo tuxedo/Dockerfile)" ]] || fail "image build: $(engine_line 1)"
pass
[[ "$(engine_line 2)" == "$(quoted run --rm --volume "$work/debs/$deb_name.deb:/artifact/$deb_name.deb:ro" \
    --volume "$fixture:/inspect:ro" branch-truce-validate:tuxedo \
    /bin/bash /inspect/inspect-artifact.sh --in-container "/artifact/$deb_name.deb")" ]] \
    || fail "inspection container: $(engine_line 2)"
pass
[[ "$output" == "$report"$'\n'"Report: $work/reports/$deb_name.inspection"$'\n'"Inspection exit status: 0" ]] \
    || fail "inspection output"
pass
[[ "$(cat "$work/reports/$deb_name.inspection")" == "$report" ]] || fail "saved report differs"
pass
# The container gets this checkout's script and the QML modules src/ui imports, nothing else.
[[ "$(cd -- "$work/state/inspect" && LC_ALL=C ls -A | tr '\n' ' ')" == 'imports.txt inspect-artifact.sh ' ]] \
    || fail "inspection volume: $(ls -A "$work/state/inspect")"
pass
cmp -s -- "$inspect" "$work/state/inspect/inspect-artifact.sh" || fail "container script differs"
pass
imports=$work/state/inspect/imports.txt
[[ -s "$imports" ]] && LC_ALL=C sort -u -c "$imports" || fail "imports.txt is empty or not sorted and unique"
pass
grep -qx 'org.kde.kirigami' "$imports" || fail "imports.txt lacks org.kde.kirigami"
pass
while IFS= read -r module; do
    grep -qE "^import ${module//./\\.}([[:space:]]|$)" "$root"/src/ui/*.qml || fail "imports.txt names $module, which src/ui does not import"
    pass
done < "$imports"

# A failed inspection returns the container's status and writes no report without --output.
report=$'FAIL QML QtQuick comes from qml6-module-qtquick, which Depends does not name\nFAILED: 1 checks'
STUB_REPORT=$report STUB_RUN_STATUS=1 invoke "$inspect" --target kubuntu --package "$work/debs/$deb_name.deb"
expect_status 1
[[ "$output" == "$report"$'\n'"Inspection exit status: 1" ]] || fail "failed inspection output"
pass
[[ "$(engine_line 2)" == *" branch-truce-validate:kubuntu /bin/bash /inspect/inspect-artifact.sh --in-container "* ]] \
    || fail "kubuntu inspection container: $(engine_line 2)"
pass
expect_no_fixture

# --- sourced, for the closure decisions
# Runs CODE in a new bash that first sources the script. ARGS become the
# positional parameters, which the script's dispatch would see if it ran.
sourced() {
    local code=$1
    shift
    rm -rf -- "$work/state" && mkdir -p -- "$work/state"
    if output=$(CONTAINER_ENGINE=$work/bin/engine BT_INSPECT=$inspect \
        "$BASH" -c 'source "$BT_INSPECT"; '"$code" bash "$@" 2>&1); then status=0; else status=$?; fi
}
expect_only_output() {
    [[ "$output" == "$1" ]] || fail "output is not exactly: $1"
    pass
}

# Sourcing defines the functions, sets no shell options and runs neither half.
expect_sourced_only() {
    sourced 'declare -F host_main container_main symbol_tags owner_of check_elf_closure check_qml_closure >/dev/null
        [[ $- != *e* && $- != *u* ]] && ! shopt -qo pipefail && echo sourced' "$@"
    expect_status 0
    expect_only_output sourced
    [[ ! -s "$work/state/engine.log" ]] || fail "sourcing with $(quoted "$@") ran the container engine"
    pass
}
expect_sourced_only
expect_sourced_only --target kubuntu --package "$work/debs/$deb_name.deb"
expect_sourced_only --in-container /nonexistent/branch-truce.deb

objdump_head=$'\n/usr/bin/branch-truce:     file format elf64-x86-64\n\nDYNAMIC SYMBOL TABLE:'
undefined() { printf '0000000000000000      DF *UND*\t0000000000000000 (%s) %s\n' "$1" "$2"; }
public_dump=$objdump_head$'\n'$(
    undefined GLIBC_2.34 __libc_start_main
    undefined GLIBC_2.2.5 free
    undefined GLIBCXX_3.4.32 _ZSt28__throw_bad_array_new_lengthv
    undefined CXXABI_1.3.9 _ZdlPvm
    undefined GCC_3.0 _Unwind_Resume
    undefined Qt_6 _ZN7QObjectD2Ev
    undefined GLIBC_2.34 pthread_create
    printf '0000000000000000 g    DF .text\t0000000000000026  Base        main\n'
)
public_tags=$'CXXABI_1.3.9\nGCC_3.0\nGLIBCXX_3.4.32\nGLIBC_2.2.5\nGLIBC_2.34\nQt_6'

# symbol_tags lists each version tag once, sorted, and an output without
# any tag gives an empty list instead of failing under set -e and pipefail.
sourced 'set -euo pipefail; symbol_tags <<<"$1"' "$public_dump"
expect_status 0
expect_only_output "$public_tags"
sourced 'set -euo pipefail; tags=$(symbol_tags <<<"$1"); echo "tags=[$tags]"' "$objdump_head"
expect_status 0
expect_only_output 'tags=[]'

# owner_of names the package that ships a path, and prints nothing, without
# failing the caller under set -e and pipefail, when no package ships it. The
# merged-/usr fallbacks of the ELF closure depend on that.
mkdir -p -- "$work/dpkg"
cat > "$work/dpkg/dpkg-query" <<'DPKG'
#!/usr/bin/env bash
case "$2" in
    /usr/lib/x86_64-linux-gnu/libowned.so.6) echo "libowned6:amd64: $2" ;;
    */qml/QtQuick/qmldir) echo "qml6-module-qtquick:amd64: $2" ;;
    */qml/org/kde/kirigami/qmldir) echo "qml6-module-org-kde-kirigami:amd64: $2" ;;
    *) exit 1 ;;
esac
DPKG
chmod +x "$work/dpkg/dpkg-query"
sourced 'set -euo pipefail; PATH=$1:$PATH; owner=$(owner_of /usr/lib/x86_64-linux-gnu/libowned.so.6); echo "owner=[$owner]"' "$work/dpkg"
expect_status 0
expect_only_output 'owner=[libowned6]'
sourced 'set -euo pipefail; PATH=$1:$PATH; owner=$(owner_of /lib/x86_64-linux-gnu/libowned.so.6); echo "owner=[$owner]"' "$work/dpkg"
expect_status 0
expect_only_output 'owner=[]'

# check_qml_closure passes a module whose qmldir comes from a package Depends
# names, and fails one from another package or without a qmldir.
mkdir -p -- "$work/qml/QtQuick" "$work/qml/org/kde/kirigami" "$work/qml/Unowned"
touch "$work/qml/QtQuick/qmldir" "$work/qml/org/kde/kirigami/qmldir" "$work/qml/Unowned/qmldir"
printf '%s\n' QtQuick org.kde.kirigami > "$work/imports-owned.txt"
qml_closure() { sourced 'set -euo pipefail; PATH=$1:$PATH; qml_root=$2; check_qml_closure "$3" "$4"; echo "failures=$failures"' \
    "$work/dpkg" "$work/qml" "$@"; }
qml_closure "$work/imports-owned.txt" 'libc6 (>= 2.34), qml6-module-qtquick | other, qml6-module-org-kde-kirigami (>= 6.0)'
expect_status 0
expect_output 'PASS 2 imported QML modules come from packages named in Depends'
expect_output 'failures=0'
qml_closure "$work/imports-owned.txt" 'libc6 (>= 2.34), qml6-module-org-kde-kirigami'
expect_status 0
expect_output 'FAIL QML QtQuick comes from qml6-module-qtquick, which Depends does not name'
expect_output 'failures=1'
printf '%s\n' Unowned Missing.Module > "$work/imports-bad.txt"
qml_closure "$work/imports-bad.txt" 'qml6-module-qtquick'
expect_status 0
expect_output 'FAIL QML Unowned comes from no package, which Depends does not name'
expect_output "FAIL QML Missing.Module has no $work/qml/Missing/Module/qmldir"
expect_output 'failures=2'
expect_no_fixture

printf '%s artifact tooling assertions passed\n' "$assertions"
