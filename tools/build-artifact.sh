#!/usr/bin/env bash
# Builds the branch-truce .deb for one OS base in a container.
#
#   tools/build-artifact.sh --target kubuntu|tuxedo --output DIR
#
# kubuntu: Kubuntu 26.04 LTS (Ubuntu resolute archive), packaging/kubuntu/Dockerfile
# tuxedo:  Tuxedo OS 24.04 (noble with Tuxedo's Qt/KDE), packaging/tuxedo/Dockerfile
#
# The image build needs network access to the target archive. The package
# build itself runs with --network none in a throwaway container. Nothing is
# installed on the host. The checkout is only read: its tracked and untracked
# non-ignored CMakeLists.txt, cmake/, src/ and packaging/ files are copied to a
# fixture under ${TMPDIR:-/tmp}, which is removed on exit.
#
# DIR receives:
#   branch-truce_<version>_amd64.deb   <version> is <project version>+<base>
#   branch-truce_<version>_amd64.buildinfo
#   branch-truce_<version>_amd64.manifest   source digest, image and toolchain
#
# The images stay for reuse and for tools/inspect-artifact.sh:
#   branch-truce-artifact:<base>-build    toolchain and build dependencies
#   branch-truce-artifact:<base>-target   the clean OS base
# Set BRANCH_TRUCE_REFRESH to a new value to fetch the archive again instead
# of reusing cached layers. CONTAINER_ENGINE defaults to docker.
set -euo pipefail

usage() { echo "Usage: $0 --target kubuntu|tuxedo --output DIR" >&2; exit 2; }
target=
output=
while (($# > 0)); do
    case "$1" in
        --target) [[ $# -ge 2 ]] || usage; target=$2; shift 2 ;;
        --output) [[ $# -ge 2 ]] || usage; output=$2; shift 2 ;;
        *) usage ;;
    esac
done
[[ -n "$target" && -n "$output" ]] || usage

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
engine=${CONTAINER_ENGINE:-docker}
case "$target" in
    kubuntu) base=kubuntu26.04 suite=resolute ;;
    tuxedo) base=tuxedo24.04 suite=noble ;;
    *) usage ;;
esac
image=branch-truce-artifact:${target}-${base#"$target"}
for tool in "$engine" git tar sha256sum; do
    command -v "$tool" >/dev/null || { echo "Missing prerequisite: $tool" >&2; exit 1; }
done

project_version=$(sed -n 's/^project(BranchTruce VERSION \([0-9][0-9.]*\) .*/\1/p' "$root/CMakeLists.txt")
[[ "$project_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "No project version in CMakeLists.txt" >&2; exit 1; }
version=$project_version+$base
name=branch-truce_${version}_amd64
mkdir -p -- "$output"
output=$(cd -- "$output" && pwd -P)

fixture=$(mktemp -d "${TMPDIR:-/tmp}/bt-artifact.XXXXXXXX")
cleanup() {
    local status=$?
    trap - EXIT
    rm -rf -- "$fixture"
    [[ ! -e "$fixture" ]] || { echo "Cleanup failed; retained fixture: $fixture" >&2; exit 1; }
    exit "$status"
}
trap cleanup EXIT
run() { printf '+'; printf ' %q' "$@"; printf '\n'; "$@"; }

echo "== Build environment $image-build"
for stage in target build; do
    run "$engine" build --quiet --target "$stage" --tag "$image-$stage" \
        --build-arg "BRANCH_TRUCE_REFRESH=${BRANCH_TRUCE_REFRESH:-}" \
        --file "$root/packaging/$target/Dockerfile" "$root/packaging"
done

echo "== Source snapshot"
tree=$fixture/work/branch-truce-$version
mkdir -p -- "$tree"
(cd -- "$root" && git ls-files -z --cached --others --exclude-standard -- \
    CMakeLists.txt cmake src packaging) |
    (cd -- "$root" && tar --null --files-from=- --create --file=-) |
    tar --extract --file=- --directory="$tree"
cp -a -- "$tree/packaging/debian" "$tree/debian"
# The changelog date fixes SOURCE_DATE_EPOCH for dpkg-buildpackage. It is the
# author time of the last commit that changed the copied paths, so commits
# elsewhere, or amending one, leave the artifact unchanged.
epoch=$(git -C "$root" log -1 --format=%at -- CMakeLists.txt cmake src packaging)
cat > "$tree/debian/changelog" <<EOF
branch-truce ($version) $suite; urgency=medium

  * Branch Truce $project_version for $base.

 -- Branch Truce contributors <contributors@example.invalid>  $(LC_ALL=C TZ=UTC date -R -d "@$epoch")
EOF
source_digest=$(tar --sort=name --mtime="@$epoch" --owner=0 --group=0 --numeric-owner \
    --format=gnu -C "$fixture/work" -cf - "branch-truce-$version" | sha256sum | cut -d' ' -f1)
dirty=$(git -C "$root" status --porcelain -- CMakeLists.txt cmake src packaging | wc -l)
echo "source sha256 $source_digest; HEAD $(git -C "$root" rev-parse HEAD); changed paths $dirty"

echo "== Package build (no network)"
run "$engine" run --rm --network none \
    --volume "$fixture/work:/work" --volume "$root/tools/artifact-baseline.sh:/opt/branch-truce/baseline.sh:ro" \
    --workdir "/work/branch-truce-$version" \
    --env DEB_BUILD_OPTIONS="parallel=2 nocheck noddebs" \
    "$image-build" /bin/bash -c \
    '/opt/branch-truce/baseline.sh > ../baseline.txt && dpkg-buildpackage -b --no-sign'

deb=$fixture/work/$name.deb
[[ -f "$deb" ]] || { echo "dpkg-buildpackage produced no $name.deb" >&2; exit 1; }
cp -- "$deb" "$fixture/work/$name.buildinfo" "$output/"
{
    printf 'artifact\t%s.deb\n' "$name"
    printf 'sha256\t%s\n' "$(sha256sum < "$deb" | cut -d' ' -f1)"
    printf 'target\t%s\n' "$target"
    printf 'base_image\t%s\n' "$(sed -n 's/^FROM \([^ ]*\) AS target$/\1/p' "$root/packaging/$target/Dockerfile")"
    # The image ID can change on every rebuild from cache; the layer list
    # identifies the build environment's contents.
    printf 'build_image\t%s\t%s\n' "$image-build" "$("$engine" image inspect --format '{{.Id}}' "$image-build")"
    printf 'build_layers_sha256\t%s\n' \
        "$("$engine" image inspect --format '{{json .RootFS.Layers}}' "$image-build" | sha256sum | cut -d' ' -f1)"
    printf 'source_sha256\t%s\n' "$source_digest"
    printf 'source_head\t%s\n' "$(git -C "$root" rev-parse HEAD)"
    printf 'source_changed_paths\t%s\n' "$dirty"
    printf 'source_date_epoch\t%s\n' "$epoch"
    cat "$fixture/work/baseline.txt"
} > "$output/$name.manifest"
cat "$output/$name.manifest"
echo "$output/$name.deb"
