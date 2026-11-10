# shellcheck shell=bash
# Shared steps of the native package builders package-deb.sh, package-rpm.sh
# and package-arch.sh, which package-release.sh runs. Sourcing defines the
# functions below (and those of package-info.sh) and runs nothing.
#
#   builder_args BUILDER ARGS...            fails with the builders' usage
#                                           unless ARGS are FORMAT VERSION DIR
#   builder_publish FORMAT VERSION FILE DIR checks FILE's own metadata with
#                                           package-identity.sh, copies it to
#                                           DIR under the release file name
#                                           and prints the copy's path

source "$(dirname -- "${BASH_SOURCE[0]}")/package-info.sh"

builder_args() {
    local builder=$1
    shift
    [ $# -eq 3 ] && [ -n "$1" ] && [ -n "$2" ] && [ -d "$3" ] && [[ "$3" == /* ]] && return 0
    echo "usage: $builder FORMAT VERSION ABSOLUTE-OUTPUT-DIR (run through package-release.sh)" >&2
    return 2
}

builder_publish() {
    local format=$1 version=$2 file=$3 output=$4 name
    "$(dirname -- "${BASH_SOURCE[0]}")/package-identity.sh" --format "$format" --version "$version" "$file" >/dev/null \
        || return 1
    name=$(package_filename "$format" "$version") || return 1
    # Copy under a temporary name, so an interrupted copy never leaves a
    # truncated file with the release name.
    cp -- "$file" "$output/.$name.partial" && mv -- "$output/.$name.partial" "$output/$name" || return 1
    printf '%s\n' "$output/$name"
}
