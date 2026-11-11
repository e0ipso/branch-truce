#!/usr/bin/env bash
# Builds the branch-truce native release package of one format in a throwaway
# source tree, inside that format's build image.
#
#   package-release.sh --format <id> --version <semver> --output <dir>
#
# packaging/formats.tsv names the format's family; build-<family>.sh does the
# build, and the package's own metadata is checked before it is published to
# <dir> under its release file name. Prints that file's absolute path on the
# last line. Exit status: 0 built, 1 build or check failed, 2 usage error or
# unsupported version.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$here/cli.sh"
source "$here/package-info.sh"
usage() { echo "usage: $0 --format $(format_ids | paste -sd '|' -) --version <semver> --output <dir>"; }

declare -A options=()
cli_parse options - 'format=! version=! output=!' "$@" || { cli_usage $? usage; exit; }
format=${options[format]} version=${options[version]} output=${options[output]}
family=$(format_field "$format" family) || { usage >&2; exit 2; }
package_filename "$format" "$version" >/dev/null || exit 2
mkdir -p -- "$output"
output=$(cd -- "$output" && pwd)
exec "$here/build-$family.sh" "$format" "$version" "$output"
