#!/usr/bin/env bash
# Tests, builds and checks the branch-truce native packages in throwaway
# containers, so the host's packages are never touched. CI and the release
# workflow run it.
#
#   container-check.sh --version <semver> --output <dir>
#                      [--format <id>|all]
#                      [--previous-version <semver> | --previous-package <file>]
#
# packaging/formats.tsv lists the formats; the default is all of them. Per
# format there are four stages: images (build or load the target, build and
# test images), test (the desktop suite), package (this version, and a
# previous version built from this source unless --previous-package gives
# one) and check (install, upgrade and remove in a clean target). Every stage
# runs even after an earlier one failed, unless it needs that stage's result;
# a summary follows. Exit status: 0 every stage passed, 1 a stage failed or
# was skipped, 2 usage error. Interrupting the script stops its running
# container.
#
# Environment: CONTAINER_ENGINE (docker), BRANCH_TRUCE_KDE_CI_IMAGE (image
# name), BRANCH_TRUCE_KDE_CI_IMAGE_CACHE (directory of saved images),
# BRANCH_TRUCE_COMPILER_CACHE (host directory of compiler caches),
# BRANCH_TRUCE_KDE_CI_REFRESH (image refresh key), SOURCE_DATE_EPOCH.

set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
packaging_dir=$(cd -- "$here/.." && pwd)
root_dir=$(cd -- "$packaging_dir/.." && pwd)
source "$here/cli.sh"
source "$here/package-info.sh"

usage() {
    echo "usage: $0 --version <semver> --output <dir> [--format $(format_ids | paste -sd '|' -)|all]" \
        "[--previous-version <semver> | --previous-package <file>]"
}
usage_error() { [ $# -eq 0 ] || echo "error: $*" >&2; usage >&2; exit 2; }

# parse_args ARG...: sets the run's configuration, version, native, output,
# formats, previous_version and previous_package, or exits 2 on a usage error.
parse_args() {
    local -A options=()
    local extension
    cli_parse options - 'version=! output=! format= previous-version= previous-package=' "$@" || { cli_usage $? usage; exit; }
    version=${options[version]} output=${options[output]}
    previous_version=${options[previous-version]-} previous_package=${options[previous-package]-}
    if [ "${options[format]:-all}" = all ]; then
        mapfile -t formats < <(format_ids)
    else
        format_field "${options[format]}" id >/dev/null || usage_error
        formats=("${options[format]}")
    fi
    [ -z "$previous_version" ] || [ -z "$previous_package" ] \
        || usage_error 'give --previous-version or --previous-package, not both'
    native=$("$here/native-version.sh" "$version") || exit 2
    if [ -z "$previous_package" ]; then
        previous_version=${previous_version:-0.0.0}
        local previous_native
        # The supported versions sort the same with sort -V as with dpkg, RPM and vercmp.
        previous_native=$("$here/native-version.sh" "$previous_version") || exit 2
        [ "$previous_native" != "$native" ] \
            && [ "$(printf '%s\n' "$previous_native" "$native" | sort -V | head -n 1)" = "$previous_native" ] \
            || usage_error "--previous-version $previous_version must be older than --version $version"
    else
        [ "${#formats[@]}" -eq 1 ] || usage_error '--previous-package needs one --format'
        extension=$(format_field "${formats[0]}" extension)
        [[ "$previous_package" == *".$extension" ]] \
            || usage_error "--previous-package $previous_package has the wrong extension for ${formats[0]} (.$extension)"
        [ -f "$previous_package" ] || usage_error "--previous-package $previous_package is not a file"
        [[ "$(basename -- "$previous_package")" =~ ^[A-Za-z0-9._~+-]+$ ]] \
            || usage_error "--previous-package $previous_package has an unexpected file name"
    fi
}

# assemble_runtime_fixture DIR: the tree the check stage mounts, with only the
# checker, its data and the expected icon bytes; no source or build tree.
assemble_runtime_fixture() {
    local fixture=$1 recipe
    mkdir -p "$fixture/packaging/scripts" "$fixture/tools/lib" "$fixture/assets/branding/platform/linux"
    cp -- "$here"/{check-package,cli,tsv,package-info,formats,native-version}.sh "$here"/pkg-*.sh \
        "$fixture/packaging/scripts/"
    cp -- "$packaging_dir"/{formats.tsv,installed-files.txt,runtime-modules.txt} "$fixture/packaging/"
    for recipe in debian/control rpm/branch-truce.spec arch/PKGBUILD; do
        mkdir -p "$fixture/packaging/${recipe%/*}"
        cp -- "$packaging_dir/$recipe" "$fixture/packaging/$recipe"
    done
    cp -- "$root_dir/tools/lib/fixture.sh" "$root_dir/tools/lib/report.sh" "$fixture/tools/lib/"
    cp -r -- "$root_dir/assets/branding/platform/linux/hicolor" "$fixture/assets/branding/platform/linux/"
}

# --- Interruption

active_container=''
active_children=()
# Detached containers survive a lost shell connection, but an interruption of
# this script stops the running one and the engine clients that follow it.
interrupted() {
    trap - INT TERM
    if [ -n "$active_container" ]; then
        echo "interrupted; stopping container $active_container" >&2
        "$engine" stop "$active_container" >/dev/null 2>&1 || true
    fi
    [ "${#active_children[@]}" -eq 0 ] || kill "${active_children[@]}" 2>/dev/null || true
    exit "$1"
}

# --- Stages

summary=()
failures=0

# run_stage FORMAT STAGE COMMAND...: runs one stage, so a failure never hides
# the stages after it. The stage's output stays as it is; its result goes to
# the summary printed at the end.
run_stage() {
    local format=$1 stage=$2 status=0
    shift 2
    "$@" || status=$?
    if [ "$status" -eq 0 ]; then
        summary+=("$format $stage: ok")
    else
        summary+=("$format $stage: FAILED (exit $status)")
        failures=$((failures + 1))
    fi
    return "$status"
}

skip_stage() {
    summary+=("$1 $2: skipped ($3)")
    failures=$((failures + 1))
}

# run_container FORMAT ENGINE-RUN-ARGUMENTS...: runs a detached container and
# returns its exit status. The native status and logs are persisted under
# OUTPUT/containers/FORMAT; the client connection's status is never the result.
run_container() {
    local format=$1 container_id status log_dir logger waiter
    shift
    log_dir=$output/containers/$format
    mkdir -p "$log_dir"
    container_id=$("$engine" run -d "$@") || return 1
    active_container=$container_id
    echo "container: $container_id"
    "$engine" logs --follow "$container_id" > "$log_dir/$container_id.log" 2>&1 &
    logger=$!
    # The wait builtin, unlike a foreground command, lets a signal trap run at once.
    "$engine" wait "$container_id" > "$log_dir/$container_id.wait" &
    waiter=$!
    active_children=("$logger" "$waiter")
    if ! wait "$waiter"; then
        wait "$logger" || true
        active_container='' active_children=()
        echo "error: container wait disconnected; inspect retained container $container_id" >&2
        return 1
    fi
    wait "$logger" || true
    active_container='' active_children=()
    status=$(cat "$log_dir/$container_id.wait")
    rm -f "$log_dir/$container_id.wait"
    "$engine" inspect "$container_id" > "$log_dir/$container_id.json" || return 1
    [[ "$status" =~ ^[0-9]+$ ]] && [ "$status" -le 255 ] || return 1
    printf '%s\n' "$status" > "$log_dir/$container_id.exit"
    # A follow connection can end before the container. Fetch the complete log
    # after wait, retaining the container if that final read cannot be proved.
    if "$engine" logs "$container_id" > "$log_dir/$container_id.log.complete" 2>&1; then
        mv "$log_dir/$container_id.log.complete" "$log_dir/$container_id.log" || return 1
    else
        cat "$log_dir/$container_id.log.complete" >&2
        return 1
    fi
    cat "$log_dir/$container_id.log"
    # Retain failed containers for diagnosis; successful state is in the report.
    if [ "$status" -eq 0 ]; then "$engine" rm "$container_id" >/dev/null || return 1; fi
    return "$status"
}

# images_ready FORMAT INPUTS: the three images exist with the identity INPUTS.
images_ready() {
    local stage
    for stage in target build test; do
        [ "$("$engine" image inspect --format "{{ index .Config.Labels \"$inputs_label\" }}" \
            "$image:$1-$stage" 2>/dev/null)" = "$2" ] || return 1
    done
}

# stage_images FORMAT INPUTS
stage_images() {
    local format=$1 inputs=$2 dockerfile started=$SECONDS stage archive
    dockerfile=$packaging_dir/$(format_field "$format" dockerfile)
    echo "== $format: images from $(basename "$dockerfile")"
    archive=${image_cache:+$image_cache/$format-$inputs.tar}
    if images_ready "$format" "$inputs"; then
        echo "image hit: $image:$format-{target,build,test} already have inputs $inputs"
    elif [ -n "$archive" ] && [ -f "$archive" ] && "$engine" load -q -i "$archive" && images_ready "$format" "$inputs"; then
        echo "image hit: loaded $image:$format-{target,build,test} for inputs $inputs from $archive"
    else
        echo "image miss: building $image:$format-{target,build,test} for inputs $inputs"
        for stage in target build test; do
            "$engine" build -q -f "$dockerfile" --target "$stage" -t "$image:$format-$stage" \
                --label "$inputs_label=$inputs" \
                --build-arg "BRANCH_TRUCE_KDE_CI_REFRESH=${BRANCH_TRUCE_KDE_CI_REFRESH:-}" "$packaging_dir" || return 1
        done
        if [ -n "$archive" ]; then
            mkdir -p "$image_cache" || return 1
            "$engine" save -o "$archive.partial" "$image:$format-target" "$image:$format-build" "$image:$format-test" || return 1
            mv "$archive.partial" "$archive" || return 1
            echo "saved the images to $archive"
        fi
    fi
    echo "== $format: images ready in $((SECONDS - started))s"
    run_container "$format" "$image:$format-build" cat /opt/branch-truce/baseline.txt
}

# compiler_cache_mounts FORMAT INPUTS STAGE: the container arguments that mount
# this format's compiler cache, one per line, or nothing without a usable cache.
compiler_cache_mounts() {
    local format=$1 inputs=$2 stage=$3 directory image_identity
    [ -n "$compiler_cache" ] || return 0
    directory=$compiler_cache/$format-$(uname -m)-$inputs
    if ! mkdir -p "$directory" || ! directory=$(cd "$directory" && pwd); then
        echo "compiler cache unavailable on host; compiling directly: $directory" >&2
        return 0
    fi
    image_identity=$("$engine" image inspect --format '{{.Id}}' "$image:$format-$stage") || return 1
    printf '%s\n' -v "$directory:/compiler-cache" -e BRANCH_TRUCE_COMPILER_CACHE_DIR=/compiler-cache \
        -e "BRANCH_TRUCE_COMPILER_CACHE_NAMESPACE=v1-$format-$(uname -m)-$inputs-$image_identity"
}

# stage_test FORMAT INPUTS
stage_test() {
    local format=$1 inputs=$2 mounts
    local -a cache_env=()
    echo "== $format: build and bounded CTest (including ui.qmllint) for $version"
    mkdir -p "$output/tests/$format"
    mounts=$(compiler_cache_mounts "$format" "$inputs" test) || return 1
    [ -z "$mounts" ] || mapfile -t cache_env <<<"$mounts"
    run_container "$format" \
        -v "$root_dir:/src:ro" -v "$output/tests/$format:/reports" \
        "${cache_env[@]}" -e BRANCH_TRUCE_COMPILER_CACHE_REPORT_DIR=/reports/cache \
        -e BRANCH_TRUCE_CI_RUN_URL -e BRANCH_TRUCE_CI_RUN_ATTEMPT -e BRANCH_TRUCE_CI_JOB \
        -e BRANCH_TRUCE_CI_SOURCE_REVISION -e "BRANCH_TRUCE_CI_FORMAT=$format" \
        -e BRANCH_TRUCE_NATIVE_FIDELITY \
        "$image:$format-test" /src/packaging/scripts/test-desktop.sh "$version" "$test_uid" "$test_gid"
}

# stage_package FORMAT INPUTS PREVIOUS-VERSION: builds this version, and
# PREVIOUS-VERSION too unless it is empty.
stage_package() {
    local format=$1 inputs=$2 build_previous=$3 mounts
    local -a cache_env=()
    if [ -z "$build_previous" ]; then
        echo "== $format: previous package $previous_name, given; package $version"
    else
        echo "== $format: previous package $build_previous, a build of this source; package $build_previous and $version"
    fi
    mounts=$(compiler_cache_mounts "$format" "$inputs" build) || return 1
    [ -z "$mounts" ] || mapfile -t cache_env <<<"$mounts"
    # shellcheck disable=SC2016
    run_container "$format" \
        -v "$work/source:/src:ro" -v "$output:/out" \
        "${build_env[@]}" "${cache_env[@]}" \
        -e "BRANCH_TRUCE_COMPILER_CACHE_REPORT_DIR=/out/cache/$format" \
        "$image:$format-build" bash -euc '
            if [ -n "$2" ]; then
                /src/packaging/scripts/package-release.sh --version "$2" --format "$1" --output /out/previous
            fi
            /src/packaging/scripts/package-release.sh --version "$3" --format "$1" --output /out
            # Match the mounted directory owner, which is container root under
            # rootless Docker and the host uid under rootful Docker.
            find /out -maxdepth 1 -type f -exec chown --reference=/out {} +
            chown -R --reference=/out /out/previous
        ' bash "$format" "$build_previous" "$version"
}

# stage_check FORMAT NEW-PACKAGE-NAME OLD-PACKAGE-NAME
stage_check() {
    local format=$1 new_name=$2 old_name=$3
    echo "== $format: install, smoke-load, upgrade and remove in a clean target"
    run_container "$format" \
        -v "$runtime_fixture:/fixture:ro" -v "$output/$new_name:/packages/$new_name:ro" \
        -v "$output/previous/$old_name:/previous/$old_name:ro" \
        "$image:$format-target" /fixture/packaging/scripts/check-package.sh --format "$format" \
        --version "$version" --package "/packages/$new_name" --previous "/previous/$old_name"
}

# check_format FORMAT BUILD-PREVIOUS: the four stages of FORMAT. BUILD-PREVIOUS
# is the previous version the package stage builds, or empty for a given one.
check_format() {
    local format=$1 build_previous=$2 new_name old_name inputs stage
    new_name=$(package_filename "$format" "$version")
    old_name=${previous_name:-$(package_filename "$format" "$previous_version")}
    inputs=$("$packaging_dir/ci/image-id.sh" --format "$format") || inputs=''
    if [ -z "$inputs" ]; then
        summary+=("$format images: FAILED (no image identity)")
        failures=$((failures + 1))
        for stage in test package check; do skip_stage "$format" "$stage" "no images"; done
    elif run_stage "$format" images stage_images "$format" "$inputs"; then
        run_stage "$format" test stage_test "$format" "$inputs" || true
        # The check installs the packages this run built, so it needs the package stage to have worked.
        if run_stage "$format" package stage_package "$format" "$inputs" "$build_previous"; then
            run_stage "$format" check stage_check "$format" "$new_name" "$old_name" || true
        else
            skip_stage "$format" check "no package was built"
        fi
    else
        for stage in test package check; do skip_stage "$format" "$stage" "no images"; done
    fi
}

main() {
    local format build_previous
    parse_args "$@"
    # The rest of the run's configuration. It is set here once; the stages only read it.
    engine=${CONTAINER_ENGINE:-docker}
    image=${BRANCH_TRUCE_KDE_CI_IMAGE:-branch-truce-kde-ci}
    image_cache=${BRANCH_TRUCE_KDE_CI_IMAGE_CACHE:-}
    inputs_label=org.branch-truce.kde-ci.inputs
    # Optional persistent compiler results. Workflow restore/save owns branch scope;
    # the source revision belongs in save keys, never the compatible restore prefix.
    compiler_cache=${BRANCH_TRUCE_COMPILER_CACHE:-}
    mkdir -p "$output/previous"
    output=$(cd -- "$output" && pwd)
    previous_name=''
    if [ -n "$previous_package" ]; then
        cp -- "$previous_package" "$output/previous/"
        previous_name=$(basename -- "$previous_package")
    fi
    build_env=()
    [ -z "${SOURCE_DATE_EPOCH:-}" ] || build_env=(-e "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH")
    test_uid=$(id -u) test_gid=$(id -g)
    if [ "$test_uid" -eq 0 ]; then test_uid=65534 test_gid=65534; fi

    work=$(mktemp -d /tmp/bt-container-check.XXXXXX)
    local cleanup
    printf -v cleanup 'rm -rf -- %q' "$work"
    # shellcheck disable=SC2064
    trap "$cleanup" EXIT
    trap 'interrupted 130' INT
    trap 'interrupted 143' TERM
    # The package stage builds from the files Git tracks, never from ignored ones.
    "$here/stage-source.sh" --tracked --from "$root_dir" --to "$work/source" packaging
    runtime_fixture=$work/runtime
    assemble_runtime_fixture "$runtime_fixture"

    # Without a given previous package, the package stage also builds the previous version.
    build_previous=$previous_version
    [ -z "$previous_name" ] || build_previous=''
    for format in "${formats[@]}"; do
        check_format "$format" "$build_previous"
    done
    echo "== summary"
    printf '%s\n' "${summary[@]}"
    [ "$failures" -eq 0 ] || exit 1
}

main "$@"
