#!/usr/bin/env bash
# Exercise the host side of tools/build-artifact.sh and tools/inspect-artifact.sh
# through CONTAINER_ENGINE set to a stub engine. No Docker, network, root or
# installed package is used. The scripts get TMPDIR inside this test's temporary
# directory and write their output there; the directory is removed on exit.
#
# The in-container half of tools/inspect-artifact.sh (--in-container) is not run:
# it reads /inspect and writes /tmp at fixed paths, so it needs the target container.
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
# dpkg-buildpackage (a /work volume) or the inspection container (an /inspect volume).
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
        work_volume= inspect_volume= workdir=
        while (($# > 0)); do
            case "$1" in
                --volume)
                    case "$2" in
                        *:/work) work_volume=${2%:/work} ;;
                        *:/inspect:ro) inspect_volume=${2%:/inspect:ro} ;;
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
    [[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+\+$base$ ]] || fail "$target changelog version '$version' for $base/$suite"
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
[[ "$(engine_line 2)" == "$(quoted run --rm --volume "$work/debs/$deb_name.deb:/artifact/$deb_name.deb:ro" \
    --volume "$fixture:/inspect:ro" branch-truce-artifact:tuxedo-24.04-target \
    /bin/bash /inspect/inspect-artifact.sh --in-container tuxedo24.04 "/artifact/$deb_name.deb")" ]] \
    || fail "inspection container: $(engine_line 2)"
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
[[ "$(engine_line 2)" == *" branch-truce-artifact:kubuntu-26.04-target /bin/bash /inspect/inspect-artifact.sh --in-container kubuntu26.04 "* ]] \
    || fail "kubuntu inspection container: $(engine_line 2)"
pass
expect_no_fixture

printf '%s artifact tooling assertions passed\n' "$assertions"
