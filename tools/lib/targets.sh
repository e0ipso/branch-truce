# shellcheck shell=bash
# The validation targets of packaging/validation-targets.tsv: clean OS
# containers that tools/validate-installed-artifact.sh installs a release
# package into. Sourcing defines the functions below; it sets
# no shell options and runs nothing. Each prints its result on stdout and
# returns 0, or prints one "error: ..." line on stderr and returns non-zero.
#
#   target_ids                         every target, one per line
#   target_field TARGET FIELD          one column of the row of TARGET
#   target_image ENGINE ROOT TARGET REFRESH
#                                      builds, or reuses from the engine's layer
#                                      cache, the target stage of TARGET's
#                                      Dockerfile under ROOT/packaging, and
#                                      prints the image name; status 1 when the
#                                      build fails. A new REFRESH value fetches
#                                      the package archives again instead of
#                                      reusing cached layers, as
#                                      BRANCH_TRUCE_KDE_CI_REFRESH does for the
#                                      CI images.

source "$(dirname -- "${BASH_SOURCE[0]}")/../../packaging/scripts/tsv.sh"

_targets_table() { printf '%s\n' "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)/packaging/validation-targets.tsv"; }

target_ids() { tsv_keys "$(_targets_table)"; }

target_field() { tsv_field "$(_targets_table)" "${1-}" "${2-}"; }

target_image() {
    local engine=$1 root=$2 target=$3 refresh=$4 dockerfile image
    dockerfile=$(target_field "$target" dockerfile) || return 2
    image=branch-truce-validate:$target
    "$engine" build --quiet --target target --tag "$image" \
        --build-arg "BRANCH_TRUCE_KDE_CI_REFRESH=$refresh" \
        --file "$root/packaging/$dockerfile" "$root/packaging" >/dev/null \
        || { echo "error: the $target image did not build from packaging/$dockerfile" >&2; return 1; }
    printf '%s\n' "$image"
}
