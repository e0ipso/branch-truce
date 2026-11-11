#!/usr/bin/env bash
# Checks that a built native package is the release package of a format.
#
#   package-identity.sh --format <id> --version <semver> <file>
#
# Reads the package's own metadata with its package manager's tools, through
# pkg-<family>.sh, and compares name, native version and architecture with
# formats.tsv, plus the format's own metadata such as licenses. A file name is
# never trusted. Prints the identity on stdout and exits 0 when it matches,
# exits 1 when it does not and 2 on a usage error or unsupported version.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$here/cli.sh"
source "$here/package-info.sh"
usage() { echo "usage: $0 --format $(format_ids | paste -sd '|' -) --version <semver> <file>"; }
declare -A options=()
declare -a files=()
cli_parse options files 'format=! version=!' "$@" || { cli_usage $? usage; exit; }
[ "${#files[@]}" -eq 1 ] && [ -n "${files[0]}" ] || { usage >&2; exit 2; }
format=${options[format]} version=${options[version]} file=${files[0]}
family=$(format_field "$format" family) || exit 2
native=$(package_native_version "$format" "$version") || exit 2
expected="branch-truce|$native|$(format_field "$format" architecture)"
# shellcheck source=pkg-deb.sh
source "$here/pkg-$family.sh"
[ -f "$file" ] || { echo "error: $file is not a file" >&2; exit 1; }
got=$(pkg_identity "$file") || { echo "error: cannot read the identity of $file" >&2; exit 1; }
[ "$got" = "$expected" ] || { echo "error: $file is $got, expected $expected" >&2; exit 1; }
reason=$(pkg_check_metadata "$file" 2>&1) || { echo "error: $file metadata: $reason" >&2; exit 1; }
printf '%s\n' "$got"
