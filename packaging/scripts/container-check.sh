#!/usr/bin/env bash
# Tests, builds and checks the branch-truce KDE DEB packages in throwaway
# containers, so the host's packages are never touched. CI and the release
# workflow run it. Every stage runs even after an earlier one failed; a summary of
# the stages follows, and the script exits non-zero when any stage failed or was skipped.
#
#   container-check.sh --version <version> --output <dir>
#                      [--format kubuntu-deb|tuxedo-deb|all]
#                      [--previous-version <version> | --previous-package <file>]

set -euo pipefail

usage() {
    echo "usage: $0 --version <version> --output <dir> [--format kubuntu-deb|tuxedo-deb|all]" \
        "[--previous-version <version> | --previous-package <file>]" >&2
    exit 2
}

version=""
output=""
formats="kubuntu-deb tuxedo-deb"
previous_version="0.0.0"
previous_package=""
previous_given=""
while [ $# -gt 0 ]; do
    case "$1" in
        --version) version="${2-}"; shift 2 || usage ;;
        --output) output="${2-}"; shift 2 || usage ;;
        --format)
            case "${2-}" in
                kubuntu-deb|tuxedo-deb) formats="$2" ;;
                all) formats="kubuntu-deb tuxedo-deb" ;;
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
    case "$formats" in kubuntu-deb|tuxedo-deb) ;; *) echo "error: --previous-package needs one format" >&2; exit 2 ;; esac
    case "$previous_package" in
        *.deb) ;;
        *) echo "error: --previous-package $previous_package is not a .deb" >&2; exit 2 ;;
    esac
    [ -f "$previous_package" ] || { echo "error: --previous-package $previous_package is not a file" >&2; exit 2; }
    [[ "$(basename "$previous_package")" =~ ^[A-Za-z0-9._~+-]+$ ]] \
        || { echo "error: --previous-package $previous_package has an unexpected file name" >&2; exit 2; }
fi

packaging_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
root_dir="$(cd "$packaging_dir/.." && pwd)"
native_version="$("$packaging_dir/scripts/native-version.sh" "$version")" || exit 2
native_previous="$("$packaging_dir/scripts/native-version.sh" "$previous_version")" || exit 2
if [ "$previous_version" = "$version" ]; then
    echo "error: --previous-version must be older than --version" >&2
    exit 2
fi

engine="${CONTAINER_ENGINE:-docker}"
image="${BRANCH_TRUCE_KDE_CI_IMAGE:-branch-truce-kde-ci}"
image_cache="${BRANCH_TRUCE_KDE_CI_IMAGE_CACHE:-}"
inputs_label=org.branch-truce.kde-ci.inputs
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
    "$engine" run --rm "$image:$format-build" cat /opt/branch-truce/baseline.txt
}

stage_test() {
    echo "== $format: build, qmllint and run CTest for $version"
    # shellcheck disable=SC2016
    "$engine" run --rm \
        -u "$test_uid:$test_gid" \
        -e HOME=/tmp/bt-home \
        -v "$root_dir:/src:ro" \
        "$image:$format-test" bash -euc '
            mkdir -p /tmp/bt-home
            work="$(mktemp -d /tmp/bt-ci.XXXXXX)"
            trap "rm -rf \"\$work\" /tmp/bt-home" EXIT
            cp -r /src "$work/repo"
            rm -rf "$work/repo/build" "$work/repo/node_modules" "$work/repo/dist"
            # In a linked worktree .git is a file pointing at a path that does not exist here.
            # Give the copy a repository of its own, so the tests that need one see the same tree.
            if [ -f "$work/repo/.git" ]; then
                rm -f "$work/repo/.git"
                git -C "$work/repo" init --quiet --initial-branch=main
                git -C "$work/repo" add -A
                git -C "$work/repo" -c user.name="Container Check" -c user.email="container-check@example.invalid" \
                    commit --quiet -m "Worktree snapshot"
            fi
            cmake -S "$work/repo" -B "$work/build" -G "Unix Makefiles" \
                -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
                -DBRANCH_TRUCE_BUILD_DESKTOP=ON -DBRANCH_TRUCE_WARNINGS_AS_ERRORS=ON \
                -DBRANCH_TRUCE_VERSION="$1"
            cmake --build "$work/build" --parallel 2
            cmake --build "$work/build" --target branch_truce_ui_qmllint
            ctest --test-dir "$work/build" --output-on-failure
        ' bash "$version"
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
    # shellcheck disable=SC2016
    "$engine" run --rm \
        -v "$root_dir:/src:ro" -v "$output:/out" \
        "${build_env[@]}" \
        "$image:$format-build" bash -euc '
            if [ -n "$2" ]; then
                /src/packaging/scripts/package-release.sh --version "$2" --format "$1" --output /out/previous
            fi
            /src/packaging/scripts/package-release.sh --version "$3" --format "$1" --output /out
            chown -R "$4" /out
        ' bash "$format" "$build_previous" "$version" "$host_uid:$host_gid"
}

stage_check() {
    echo "== $format: install, smoke-load, upgrade and remove in a clean target"
    # shellcheck disable=SC2016
    "$engine" run --rm \
        -v "$root_dir:/src:ro" -v "$output:/out:ro" \
        "$image:$format-target" bash -euc '
            /src/packaging/scripts/prepare-target.sh --format "$2" --refresh
            /src/packaging/scripts/check-package.sh --version "$1" --package '"$new"' --previous '"$old"'
        ' bash "$version" "$format"
}

for format in $formats; do
    if [ "$format" = kubuntu-deb ]; then
        dockerfile="$packaging_dir/ci/kubuntu.Dockerfile"
        suffix=-ubuntu26.04
    else
        dockerfile="$packaging_dir/ci/tuxedo.Dockerfile"
        suffix=-tuxedo24.04
    fi
    new="/out/branch-truce_${native_version}_amd64${suffix}.deb"
    old="/out/previous/${previous_name:-branch-truce_${native_previous}_amd64${suffix}.deb}"

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
