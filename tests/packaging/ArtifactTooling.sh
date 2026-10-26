#!/usr/bin/env bash
# Exercise the host side of tools/build-artifact.sh and tools/inspect-artifact.sh
# through CONTAINER_ENGINE set to a stub engine. No Docker, network, root or
# installed package is used. The scripts get TMPDIR inside this test's temporary
# directory and write their output there; the directory is removed on exit.
#
# The in-container half of tools/inspect-artifact.sh (--in-container) is not run:
# it reads /inspect and writes /tmp at fixed paths, so it needs the target container.
# Its private Qt ABI and document-save icon decisions are tested on their own by
# sourcing the script and feeding synthetic objdump -T output, Depends fields and
# appearance records to its functions.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
work=$(mktemp -d "${TMPDIR:-/tmp}/bt-artifact-tooling.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT
# The scripts print physical paths.
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
# The scripts remove their fixtures under TMPDIR on every exit path.
expect_no_fixture() {
    [[ -z "$(ls -A -- "$TMPDIR")" ]] || fail "fixtures left in TMPDIR: $(ls -A -- "$TMPDIR")"
    pass
}
engine_line() { sed -n "${1}p" "$work/state/engine.log"; }
engine_calls() { wc -l < "$work/state/engine.log"; }

# Stub engine: logs each argv, answers image queries, and for `run` either plays
# dpkg-buildpackage (a /work volume), the appearance probe build (a /probe/lib
# volume) or the inspection container (an /inspect volume).
cat > "$work/bin/engine" <<'ENGINE'
#!/usr/bin/env bash
set -euo pipefail
state=$STUB_STATE
{ printf '%q' "$1"; printf ' %q' "${@:2}"; printf '\n'; } >> "$state/engine.log"
case "$1 ${2:-}" in
    'image inspect')
        case "${3:-}" in
            --format)
                case "$4" in
                    '{{.Id}}') echo sha256:stub-build-image ;;
                    '{{json .RootFS.Layers}}') echo '["sha256:stub-layer"]' ;;
                esac ;;
            *) exit "${STUB_IMAGE_STATUS:-0}" ;;
        esac ;;
    build\ *) ;;
    run\ *)
        work_volume= inspect_volume= probe_volume= workdir=
        while (($# > 0)); do
            case "$1" in
                --volume)
                    case "$2" in
                        *:/work) work_volume=${2%:/work} ;;
                        *:/inspect:ro) inspect_volume=${2%:/inspect:ro} ;;
                        *:/probe/lib) probe_volume=${2%:/probe/lib} ;;
                    esac
                    shift 2 ;;
                --workdir) workdir=$2; shift 2 ;;
                *) shift ;;
            esac
        done
        if [[ -n "$work_volume" ]]; then
            tree=$work_volume/${workdir#/work/}
            version=${tree##*/branch-truce-}
            printf '%s\n' "$work_volume" > "$state/work-volume"
            cp -- "$tree/debian/changelog" "$state/changelog"
            (cd -- "$tree" && find . -type f | LC_ALL=C sort) > "$state/snapshot"
            [[ "${STUB_BUILD_DEB:-1}" == 1 ]] || exit 0
            printf 'stub deb\n' > "$work_volume/branch-truce_${version}_amd64.deb"
            printf 'stub buildinfo\n' > "$work_volume/branch-truce_${version}_amd64.buildinfo"
            printf 'os\tStub OS\tstub\n' > "$work_volume/baseline.txt"
        elif [[ -n "$probe_volume" ]]; then
            cp -a -- "$probe_volume/../src" "$state/probe-src"
            echo 'stub probe build output'
            [[ "${STUB_PROBE_STATUS:-0}" == 0 ]] || exit "$STUB_PROBE_STATUS"
            printf 'stub probe\n' > "$probe_volume/libappearance_probe.so"
        elif [[ -n "$inspect_volume" ]]; then
            printf '%s\n' "$inspect_volume" > "$state/inspect-volume"
            cp -a -- "$inspect_volume" "$state/inspect"
            printf '%s\n' "$STUB_REPORT"
            exit "${STUB_RUN_STATUS:-0}"
        fi ;;
esac
ENGINE
chmod +x "$work/bin/engine"
export STUB_STATE=$work/state

build=$root/tools/build-artifact.sh
# The release version build-artifact.sh reads from package.json and passes to debian/rules.
release=$(sed -n 's/^  "version": "\([^"]*\)",\{0,1\}$/\1/p' "$root/package.json")
[[ "$release" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "package.json version '$release'"
inspect=$root/tools/inspect-artifact.sh

# --- tools/build-artifact.sh argument handling
expect_rejected "$build"
expect_rejected "$build" --target kubuntu
expect_rejected "$build" --output "$work/out"
expect_rejected "$build" --target
expect_rejected "$build" --target kubuntu --output
expect_rejected "$build" --target kubuntu --output "$work/out" --extra
expect_rejected "$build" --target debian --output "$work/out-unknown"
[[ ! -e "$work/out-unknown" ]] || fail "an unknown target created its output directory"
pass

# A missing container engine stops the build before any output exists.
missing_engine=$work/bin/no-such-engine
rm -rf -- "$work/state" && mkdir -p -- "$work/state"
if output=$(CONTAINER_ENGINE=$missing_engine "$build" --target kubuntu --output "$work/out-missing" 2>&1); then
    status=0; else status=$?; fi
expect_status 1
expect_output "Missing prerequisite: $missing_engine"
[[ ! -e "$work/out-missing" ]] || fail "a missing engine created the output directory"
pass
expect_no_fixture

# --- tools/build-artifact.sh with the stub engine
build_succeeds() {
    local target=$1 base=$2 suite=$3 image=$4 out=$work/out-$1
    invoke "$build" --target "$target" --output "$out"
    expect_status 0
    expect_no_fixture

    local version name
    version=$(sed -n "s/^branch-truce (\\(.*\\)) $suite; urgency=medium\$/\\1/p" "$work/state/changelog")
    [[ "$version" == "$release+$base" ]] || fail "$target changelog version '$version' for $release, $base/$suite"
    pass
    name=branch-truce_${version}_amd64
    [[ "$(ls -- "$out" | LC_ALL=C sort | tr '\n' ' ')" == "$name.buildinfo $name.deb $name.manifest " ]] \
        || fail "$target output files: $(ls -- "$out")"
    pass
    [[ "${output##*$'\n'}" == "$out/$name.deb" ]] || fail "$target did not print the .deb path last"
    pass

    # Both image stages, then the package build without network.
    (($(engine_calls) == 5)) || fail "$target engine calls: $(cat "$work/state/engine.log")"
    pass
    local stage line=1
    for stage in target build; do
        [[ "$(engine_line "$line")" == "$(quoted build --quiet --target "$stage" --tag "$image-$stage" \
            --build-arg BRANCH_TRUCE_REFRESH= --file "$root/packaging/$target/Dockerfile" "$root/packaging")" ]] \
            || fail "$target $stage image build: $(engine_line "$line")"
        pass
        line=$((line + 1))
    done
    local work_volume
    work_volume=$(cat "$work/state/work-volume")
    [[ "$work_volume" == "$TMPDIR"/bt-artifact.*/work ]] || fail "$target build fixture $work_volume is outside TMPDIR"
    pass
    [[ "$(engine_line 3)" == "$(quoted run --rm --network none --volume "$work_volume:/work" \
        --volume "$root/tools/artifact-baseline.sh:/opt/branch-truce/baseline.sh:ro" \
        --workdir "/work/branch-truce-$version" --env 'DEB_BUILD_OPTIONS=parallel=2 nocheck noddebs' \
        --env "BRANCH_TRUCE_VERSION=$release" \
        "$image-build" /bin/bash -c '/opt/branch-truce/baseline.sh > ../baseline.txt && dpkg-buildpackage -b --no-sign')" ]] \
        || fail "$target package build: $(engine_line 3)"
    pass

    # The snapshot holds the build inputs and the generated Debian directory.
    local member
    for member in ./CMakeLists.txt ./debian/control ./debian/rules ./debian/changelog ./packaging/debian/control \
        "./packaging/$target/Dockerfile" ./src/app/main.cpp; do
        grep -qxF -- "$member" "$work/state/snapshot" || fail "$target snapshot lacks $member"
        pass
    done
    ! grep -qE '^\./(tests|tools|docs|build)/' "$work/state/snapshot" || fail "$target snapshot has paths outside the build inputs"
    pass
    # control declares debhelper-compat, so the package build must not see debian/compat.
    ! grep -qxF ./debian/compat "$work/state/snapshot" || fail "$target snapshot keeps debian/compat"
    pass

    local manifest=$out/$name.manifest from
    from=$(sed -n 's/^FROM \([^ ]*\) AS target$/\1/p' "$root/packaging/$target/Dockerfile")
    local expected
    expected=$(printf '%s\n' "artifact	$name.deb" \
        "sha256	$(sha256sum < "$out/$name.deb" | cut -d' ' -f1)" \
        "target	$target" \
        "base_image	$from" \
        "build_image	$image-build	sha256:stub-build-image" \
        "build_layers_sha256	$(echo '["sha256:stub-layer"]' | sha256sum | cut -d' ' -f1)")
    [[ "$(head -6 "$manifest")" == "$expected" ]] || fail "$target manifest head: $(head -6 "$manifest")"
    pass
    [[ "$(sed -n 7p "$manifest")" =~ ^source_sha256$'\t'[0-9a-f]{64}$ ]] || fail "$target source digest"
    pass
    [[ "$(sed -n 8p "$manifest")" == "source_head	$(git -C "$root" rev-parse HEAD)" ]] || fail "$target source head"
    pass
    [[ "$(sed -n 9p "$manifest")" =~ ^source_changed_paths$'\t'[0-9]+$ ]] || fail "$target changed paths"
    pass
    [[ "$(sed -n 10p "$manifest")" == "source_date_epoch	$(git -C "$root" log -1 --format=%at -- CMakeLists.txt cmake src packaging)" ]] \
        || fail "$target source date epoch"
    pass
    [[ "$(sed -n '11,$p' "$manifest")" == "os	Stub OS	stub" ]] || fail "$target manifest lacks the container baseline"
    pass
}
build_succeeds kubuntu kubuntu26.04 resolute branch-truce-artifact:kubuntu-26.04
build_succeeds tuxedo tuxedo24.04 noble branch-truce-artifact:tuxedo-24.04

# A package build that leaves no .deb fails and copies nothing.
STUB_BUILD_DEB=0 invoke "$build" --target kubuntu --output "$work/out-nodeb"
expect_status 1
expect_output 'dpkg-buildpackage produced no branch-truce_'
[[ -d "$work/out-nodeb" && -z "$(ls -A -- "$work/out-nodeb")" ]] || fail "a failed build left output files"
pass
expect_no_fixture

# --- tools/inspect-artifact.sh argument handling
mkdir -p -- "$work/debs"
deb_name=branch-truce_0.0.0+tuxedo24.04_amd64
printf 'not inspected here\n' > "$work/debs/$deb_name.deb"
expect_rejected "$inspect"
expect_rejected "$inspect" --target kubuntu
expect_rejected "$inspect" --package "$work/debs/$deb_name.deb"
expect_rejected "$inspect" --target kubuntu --package "$work/debs/missing.deb"
expect_rejected "$inspect" --target kubuntu --package "$work/debs"
expect_rejected "$inspect" --target debian --package "$work/debs/$deb_name.deb"
expect_rejected "$inspect" --target kubuntu --package
expect_rejected "$inspect" --target kubuntu --package "$work/debs/$deb_name.deb" --output
expect_rejected "$inspect" --target kubuntu --package "$work/debs/$deb_name.deb" --verbose
expect_rejected "$inspect" --in-container kubuntu26.04
expect_rejected "$inspect" --in-container kubuntu26.04 /artifact/x.deb extra
expect_no_fixture

# A missing target image stops before any container runs.
STUB_IMAGE_STATUS=1 invoke "$inspect" --target kubuntu --package "$work/debs/$deb_name.deb"
expect_status 1
expect_output 'Missing image branch-truce-artifact:kubuntu-26.04-target; run tools/build-artifact.sh --target kubuntu first'
[[ "$(engine_calls)" == 1 && "$(engine_line 1)" == "image inspect branch-truce-artifact:kubuntu-26.04-target" ]] \
    || fail "missing image engine calls: $(cat "$work/state/engine.log")"
pass
expect_no_fixture

# --- tools/inspect-artifact.sh with the stub engine
# A relative package path is mounted by its absolute path; the report is kept.
report=$'== Result\nAll inspection checks passed for branch-truce 0.0.0+tuxedo24.04'
if output=$(cd -- "$work" && rm -rf -- state && mkdir state && STUB_REPORT=$report \
    CONTAINER_ENGINE=$work/bin/engine "$inspect" --target tuxedo --package "debs/$deb_name.deb" --output reports 2>&1)
then status=0; else status=$?; fi
expect_status 0
expect_no_fixture
fixture=$(cat "$work/state/inspect-volume")
[[ "$fixture" == "$TMPDIR"/bt-inspect.* ]] || fail "inspection fixture $fixture is outside TMPDIR"
pass
[[ "$(engine_calls)" == 3 ]] || fail "expected 3 engine calls: $(cat "$work/state/engine.log")"
pass
# The appearance probe is built in the -build image of the base, offline, from this checkout's source.
[[ "$(engine_line 2)" == "$(quoted run --rm --network none \
    --volume "$fixture/probe/src:/probe/src:ro" --volume "$fixture/probe/lib:/probe/lib" \
    branch-truce-artifact:tuxedo-24.04-build /bin/bash -c \
    'cmake -S /probe/src -B /tmp/probe-build && cmake --build /tmp/probe-build \
            && cp /tmp/probe-build/libappearance_probe.so /probe/lib/')" ]] \
    || fail "probe build container: $(engine_line 2)"
pass
cmp -s -- "$root/tests/fixtures/AppearanceProbe.cpp" "$work/state/probe-src/AppearanceProbe.cpp" || fail "probe source differs"
pass
grep -q 'add_library(appearance_probe MODULE AppearanceProbe.cpp)' "$work/state/probe-src/CMakeLists.txt" || fail "probe CMakeLists lacks the module"
pass
# The container gets the built probe at the path container_main preloads.
[[ -f "$work/state/inspect/probe/lib/libappearance_probe.so" ]] || fail "inspection volume lacks probe/lib/libappearance_probe.so"
pass
[[ "$(engine_line 3)" == "$(quoted run --rm --volume "$work/debs/$deb_name.deb:/artifact/$deb_name.deb:ro" \
    --volume "$fixture:/inspect:ro" branch-truce-artifact:tuxedo-24.04-target \
    /bin/bash /inspect/inspect-artifact.sh --in-container tuxedo24.04 "/artifact/$deb_name.deb")" ]] \
    || fail "inspection container: $(engine_line 3)"
pass
[[ "$output" == "$report"$'\n'"Report: $work/reports/$deb_name.inspection"$'\n'"Inspection exit status: 0" ]] \
    || fail "inspection output"
pass
[[ "$(cat "$work/reports/$deb_name.inspection")" == "$report" ]] || fail "saved report differs"
pass
# The container gets this checkout's scripts and the QML modules src/ui imports.
cmp -s -- "$inspect" "$work/state/inspect/inspect-artifact.sh" || fail "container script differs"
pass
cmp -s -- "$root/tools/artifact-baseline.sh" "$work/state/inspect/artifact-baseline.sh" || fail "baseline script differs"
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
report=$'FAIL Qt_6_PRIVATE_API symbols without an exact Qt private ABI pin in Depends\nFAILED: 1 checks'
STUB_REPORT=$report STUB_RUN_STATUS=1 invoke "$inspect" --target kubuntu --package "$work/debs/$deb_name.deb"
expect_status 1
[[ "$output" == "$report"$'\n'"Inspection exit status: 1" ]] || fail "failed inspection output"
pass
[[ "$(engine_line 3)" == *" branch-truce-artifact:kubuntu-26.04-target /bin/bash /inspect/inspect-artifact.sh --in-container kubuntu26.04 "* ]] \
    || fail "kubuntu inspection container: $(engine_line 3)"
pass
expect_no_fixture

# A failed probe build stops before the inspection container and says how to get the -build image.
STUB_PROBE_STATUS=125 invoke "$inspect" --target kubuntu --package "$work/debs/$deb_name.deb"
expect_status 1
expect_output 'stub probe build output'
expect_output 'Could not build the appearance probe in branch-truce-artifact:kubuntu-26.04-build; run tools/build-artifact.sh --target kubuntu first'
[[ "$(engine_calls)" == 2 ]] || fail "failed probe build engine calls: $(cat "$work/state/engine.log")"
pass
expect_no_fixture

# --- tools/inspect-artifact.sh sourced, for the private Qt ABI check
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

# Sourcing defines the functions and runs neither host_main nor container_main.
expect_sourced_only() {
    sourced 'declare -F host_main container_main symbol_tags check_private_abi check_icon_resolves build_appearance_probe >/dev/null; echo sourced' "$@"
    expect_status 0
    expect_only_output sourced
    [[ ! -s "$work/state/engine.log" ]] || fail "sourcing with $(quoted "$@") ran the container engine"
    pass
}
expect_sourced_only
expect_sourced_only --target kubuntu --package "$work/debs/$deb_name.deb"
expect_sourced_only --in-container kubuntu26.04 /nonexistent/branch-truce.deb
# The inspection decides with these functions, not with a copy of them.
sourced 'declare -f container_main'
expect_status 0
expect_output 'tags=$(objdump -T "$binary" | symbol_tags)'
expect_output 'check_private_abi "$tags" "$depends"'
expect_output 'check_icon_resolves "$appearance"'
expect_output 'LD_PRELOAD=/inspect/probe/lib/libappearance_probe.so'

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
private_dump=$public_dump$'\n'$(undefined Qt_6_PRIVATE_API _ZN11QQmlPrivate8qmlregisterENS_16RegistrationTypeEPv)
public_tags=$'CXXABI_1.3.9\nGCC_3.0\nGLIBCXX_3.4.32\nGLIBC_2.2.5\nGLIBC_2.34\nQt_6'

# symbol_tags lists each version tag once, sorted, and an output without
# any tag gives an empty list instead of aborting under set -e and pipefail.
sourced 'symbol_tags <<<"$1"' "$public_dump"
expect_status 0
expect_only_output "$public_tags"
sourced 'symbol_tags <<<"$1"' "$private_dump"
expect_status 0
expect_only_output "$public_tags"$'\nQt_6_PRIVATE_API'
sourced 'tags=$(symbol_tags <<<"$1"); echo "tags=[$tags]"' "$objdump_head"
expect_status 0
expect_only_output 'tags=[]'

# check_private_abi on the tags of a dump, as container_main calls it.
private_abi() { sourced 'check_private_abi "$(symbol_tags <<<"$1")" "$2"; echo "failures=$failures"' "$@"; }
no_pin='libc6 (>= 2.34), libqt6core6t64 (>= 6.10.2), qml6-module-org-kde-kirigami, git'
base_pin='libc6 (>= 2.34), qt6-base-private-abi (= 6.10.2), libqt6core6t64 (>= 6.10.2)'
declarative_pin='libc6 (>= 2.34), qt6-declarative-private-abi (= 6.10.2) | other, git'

# No private symbols: passes whatever Depends says, including no tags at all.
for dump in "$public_dump" "$objdump_head"; do
    private_abi "$dump" "$no_pin"
    expect_status 0
    expect_only_output $'PASS no Qt_6_PRIVATE_API symbols\nfailures=0'
done
# Private symbols with an exact pin pass.
for depends in "$base_pin" "$declarative_pin"; do
    private_abi "$private_dump" "$depends"
    expect_status 0
    expect_only_output $'PASS Qt_6_PRIVATE_API symbols with an exact Qt private ABI pin in Depends\nfailures=0'
done
# Private symbols without one fail, including an unversioned or lower-bound private ABI package.
for depends in "$no_pin" 'libc6 (>= 2.34), qt6-base-private-abi, libqt6core6t64 (>= 6.10.2)' \
    'libc6 (>= 2.34), qt6-base-private-abi (>= 6.10.2), libqt6core6t64 (>= 6.10.2)'; do
    private_abi "$private_dump" "$depends"
    expect_status 0
    expect_only_output $'FAIL Qt_6_PRIVATE_API symbols without an exact Qt private ABI pin in Depends\nfailures=1'
done

# check_icon_resolves on the appearance record the preloaded probe prints. Only a
# resolved document-save passes; a null icon, a missing key and no record fail.
icon_check() { sourced 'check_icon_resolves "$1"; echo "failures=$failures"' "$@"; }
icon_check '{"displayName":"Branch Truce","documentSave":true,"fallbackTheme":"hicolor","style":"org.kde.desktop","theme":"breeze"}'
expect_status 0
expect_only_output $'PASS QIcon::fromTheme(document-save) resolves in the installed application\nfailures=0'
icon_check '{"displayName":"Branch Truce","documentSave":false,"fallbackTheme":"hicolor","style":"org.kde.desktop","theme":"breeze"}'
expect_status 0
expect_only_output $'FAIL QIcon::fromTheme(document-save) is null in the installed application; does Depends name the Qt SVG icon engine package?\nfailures=1'
icon_check '{"style":"Basic"}'
expect_status 0
expect_only_output $'FAIL QIcon::fromTheme(document-save) is null in the installed application; does Depends name the Qt SVG icon engine package?\nfailures=1'
icon_check ''
expect_status 0
expect_only_output $'FAIL no BRANCH_TRUCE_APPEARANCE record from the installed application\nfailures=1'
expect_no_fixture

printf '%s artifact tooling assertions passed\n' "$assertions"
