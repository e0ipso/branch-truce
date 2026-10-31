#!/usr/bin/env bash
# Tests, builds and checks the branch-truce native KDE packages in throwaway
# containers, so the host's packages are never touched. CI and the release
# workflow run it. Every stage runs even after an earlier one failed; a summary of
# the stages follows, and the script exits non-zero when any stage failed or was skipped.
#
#   container-check.sh --version <version> --output <dir>
#                      [--format kubuntu-deb|fedora-rpm|arch-pkg|all]
#                      [--previous-version <version> | --previous-package <file>]

set -euo pipefail

usage() {
    echo "usage: $0 --version <version> --output <dir> [--format kubuntu-deb|fedora-rpm|arch-pkg|all]" \
        "[--previous-version <version> | --previous-package <file>]" >&2
    exit 2
}

version=""
output=""
formats="kubuntu-deb fedora-rpm arch-pkg"
previous_version="0.0.0"
previous_package=""
previous_given=""
while [ $# -gt 0 ]; do
    case "$1" in
        --version) version="${2-}"; shift 2 || usage ;;
        --output) output="${2-}"; shift 2 || usage ;;
        --format)
            case "${2-}" in
                kubuntu-deb|fedora-rpm|arch-pkg) formats="$2" ;;
                all) formats="kubuntu-deb fedora-rpm arch-pkg" ;;
                *) usage ;;
            esac
            shift 2 ;;
        --previous-version) previous_version="${2-}"; previous_given=true; shift 2 || usage ;;
        --previous-package) previous_package="${2-}"; shift 2 || usage ;;
        *) usage ;;
    esac
done
[ -n "$version" ] && [ -n "$output" ] || usage
if [ -n "$previous_package" ]; then
    [ -z "$previous_given" ] || { echo "error: give --previous-version or --previous-package, not both" >&2; exit 2; }
    case "$formats" in kubuntu-deb|fedora-rpm|arch-pkg) ;; *) echo "error: --previous-package needs one format" >&2; exit 2 ;; esac
    case "$previous_package" in
        *.deb) [ "$formats" = kubuntu-deb ] || { echo 'error: predecessor extension does not match format' >&2; exit 2; } ;;
        *.rpm) [ "$formats" = fedora-rpm ] || { echo 'error: predecessor extension does not match format' >&2; exit 2; } ;;
        *.pkg.tar.zst) [ "$formats" = arch-pkg ] || { echo 'error: predecessor extension does not match format' >&2; exit 2; } ;;
        *) echo "error: --previous-package $previous_package has an unsupported extension" >&2; exit 2 ;;
    esac
    [ -f "$previous_package" ] || { echo "error: --previous-package $previous_package is not a file" >&2; exit 2; }
    [[ "$(basename "$previous_package")" =~ ^[A-Za-z0-9._~+-]+$ ]] \
        || { echo "error: --previous-package $previous_package has an unexpected file name" >&2; exit 2; }
fi

packaging_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
root_dir="$(cd "$packaging_dir/.." && pwd)"
# package_info assigns these outputs for its Bash callers.
package_filename=''
source "$packaging_dir/scripts/package-info.sh"
"$packaging_dir/scripts/native-version.sh" "$version" >/dev/null || exit 2
"$packaging_dir/scripts/native-version.sh" "$previous_version" >/dev/null || exit 2
if [ "$previous_version" = "$version" ]; then
    echo "error: --previous-version must be older than --version" >&2
    exit 2
fi

engine="${CONTAINER_ENGINE:-docker}"
image="${BRANCH_TRUCE_KDE_CI_IMAGE:-branch-truce-kde-ci}"
image_cache="${BRANCH_TRUCE_KDE_CI_IMAGE_CACHE:-}"
inputs_label=org.branch-truce.kde-ci.inputs
# Optional persistent compiler results. Workflow restore/save owns branch scope;
# the source revision belongs in save keys, never the compatible restore prefix.
compiler_cache="${BRANCH_TRUCE_COMPILER_CACHE:-}"
cache_env=()
mkdir -p "$output/previous"
output="$(cd "$output" && pwd)"
if [ -n "$previous_package" ]; then
    cp "$previous_package" "$output/previous/"
    previous_name="$(basename "$previous_package")"
fi
build_env=()
[ -z "${SOURCE_DATE_EPOCH:-}" ] || build_env=(-e "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH")

host_uid="$(id -u)"
host_gid="$(id -g)"
test_uid="$host_uid"
test_gid="$host_gid"
if [ "$test_uid" -eq 0 ]; then
    test_uid=65534
    test_gid=65534
fi

summary=()
failures=0

# Runs one stage of a format, so a failure never hides the stages after it. The stage's
# output stays as it was; its result goes to the summary printed at the end.
run_stage() {
    local format="$1" stage="$2" status=0
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

# Detached containers survive a lost shell connection. Persist the native exit
# status and logs; the client connection's status is never the test result.
run_container() {
    local container_id status logger log_dir
    log_dir="$output/containers/$format"
    mkdir -p "$log_dir"
    container_id="$("$engine" run -d "$@")" || return 1
    echo "container: $container_id"
    "$engine" logs --follow "$container_id" > "$log_dir/$container_id.log" 2>&1 &
    logger=$!
    if ! status="$("$engine" wait "$container_id")"; then
        wait "$logger" || true
        echo "error: container wait disconnected; inspect retained container $container_id" >&2
        return 1
    fi
    wait "$logger" || true
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

images_ready() {
    local stage
    for stage in target build test; do
        [ "$("$engine" image inspect --format "{{ index .Config.Labels \"$inputs_label\" }}" \
            "$image:$format-$stage" 2>/dev/null)" = "$inputs" ] || return 1
    done
}

stage_images() {
    echo "== $format: images from $(basename "$dockerfile")"
    local images_started=$SECONDS stage archive
    inputs="$("$packaging_dir/ci/image-id.sh" --format "$format")" || return 1
    archive="${image_cache:+$image_cache/$format-$inputs.tar}"
    if images_ready; then
        echo "image hit: $image:$format-{target,build,test} already have inputs $inputs"
    elif [ -n "$archive" ] && [ -f "$archive" ] && "$engine" load -q -i "$archive" && images_ready; then
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
    echo "== $format: images ready in $((SECONDS - images_started))s"
    run_container "$image:$format-build" cat /opt/branch-truce/baseline.txt
}

prepare_compiler_cache() {
    local stage="$1" directory image_identity
    cache_env=()
    [ -n "$compiler_cache" ] || return 0
    directory="$compiler_cache/$format-$(uname -m)-$inputs"
    if ! mkdir -p "$directory"; then
        echo "compiler cache unavailable on host; compiling directly: $directory" >&2
        return 0
    fi
    if ! directory="$(cd "$directory" && pwd)"; then
        echo 'compiler cache path unavailable on host; compiling directly' >&2
        return 0
    fi
    image_identity="$("$engine" image inspect --format '{{.Id}}' "$image:$format-$stage")" || return 1
    cache_env=(-v "$directory:/compiler-cache"
        -e BRANCH_TRUCE_COMPILER_CACHE_DIR=/compiler-cache
        -e "BRANCH_TRUCE_COMPILER_CACHE_NAMESPACE=v1-$format-$(uname -m)-$inputs-$image_identity")
}

stage_test() {
    echo "== $format: build and bounded CTest (including ui.qmllint) for $version"
    mkdir -p "$output/tests/$format"
    prepare_compiler_cache test || return 1
    run_container \
        -v "$root_dir:/src:ro" -v "$output/tests/$format:/reports" \
        "${cache_env[@]}" -e BRANCH_TRUCE_COMPILER_CACHE_REPORT_DIR=/reports/cache \
        "$image:$format-test" /src/packaging/scripts/test-desktop.sh "$version" "$test_uid" "$test_gid"
}

stage_package() {
    local build_previous
    if [ -n "$previous_package" ]; then
        echo "== $format: previous package $previous_name, given; package $version"
        build_previous=""
    else
        echo "== $format: previous package $previous_version, a build of this source; package $previous_version and $version"
        build_previous="$previous_version"
    fi
    prepare_compiler_cache build || return 1
    # shellcheck disable=SC2016
    run_container \
        -v "$root_dir:/src:ro" -v "$output:/out" \
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

stage_check() {
    echo "== $format: install, smoke-load, upgrade and remove in a clean target"
    run_container \
        -v "$runtime_fixture:/fixture:ro" -v "$output/$new_name:/packages/$new_name:ro" \
        -v "$output/previous/$old_name:/previous/$old_name:ro" \
        "$image:$format-target" bash -euc '
            /fixture/packaging/scripts/prepare-target.sh --format "$1" --refresh
            /fixture/packaging/scripts/"$2" --version "$3" --package "/packages/$4" --previous "/previous/$5"
        ' bash "$format" "$checker" "$version" "$new_name" "$old_name"
}

# Only helpers and expected icon bytes enter the installed-runtime target.
runtime_fixture="$(mktemp -d /tmp/bt-runtime-fixture.XXXXXX)"
trap 'rm -rf "$runtime_fixture"' EXIT
mkdir -p "$runtime_fixture/packaging/scripts" "$runtime_fixture/assets/branding/platform/linux"
for helper in prepare-target native-version check-package check-fedora check-arch package-arch; do
    cp "$packaging_dir/scripts/$helper.sh" "$runtime_fixture/packaging/scripts/"
done
cp -r "$root_dir/assets/branding/platform/linux/hicolor" "$runtime_fixture/assets/branding/platform/linux/"

for format in $formats; do
    case "$format" in
        kubuntu-deb) dockerfile="$packaging_dir/ci/kubuntu.Dockerfile"; checker=check-package.sh ;;
        fedora-rpm) dockerfile="$packaging_dir/ci/fedora.Dockerfile"; checker=check-fedora.sh ;;
        arch-pkg) dockerfile="$packaging_dir/ci/arch.Dockerfile"; checker=check-arch.sh ;;
    esac
    package_info "$format" "$version"
    new_name="$package_filename"
    package_info "$format" "$previous_version"
    old_name="${previous_name:-$package_filename}"

    if run_stage "$format" images stage_images; then
        run_stage "$format" test stage_test || true
        # The check installs the packages this run built, so it needs the package stage to have worked.
        if run_stage "$format" package stage_package; then
            run_stage "$format" check stage_check || true
        else
            skip_stage "$format" check "no package was built"
        fi
    else
        for stage in test package check; do
            skip_stage "$format" "$stage" "no images"
        done
    fi
done

echo "== summary"
printf '%s\n' "${summary[@]}"
[ "$failures" -eq 0 ] || exit 1
