#!/usr/bin/env bash
# Native release identity of one package format, from packaging/formats.tsv.
#
#   package-info.sh --format <id> --version <semver>
#
# Prints one JSON object: format, version, nativeVersion (with the native
# release revision), filename and architecture. Usage errors and unsupported
# versions exit 2. Needs Bash and awk only, so build containers without Node
# can run it.
#
# Sourcing the file defines package_filename and package_native_version, which
# print their value on stdout, and runs nothing else.

source "$(dirname -- "${BASH_SOURCE[0]}")/cli.sh"
source "$(dirname -- "${BASH_SOURCE[0]}")/formats.sh"

# package_filename FORMAT VERSION
package_filename() { format_render "${1-}" filename "${2-}"; }

# package_native_version FORMAT VERSION
package_native_version() { format_render "${1-}" native_version "${2-}"; }

_package_info_usage() { echo "usage: package-info.sh --format $(format_ids | paste -sd '|') --version <semver>"; }

package_info_main() {
    local format version native filename architecture
    local -A options=()
    cli_parse options - 'format=! version=!' "$@" || { cli_usage $? _package_info_usage; return; }
    format=${options[format]} version=${options[version]}
    format_field "$format" id >/dev/null || { _package_info_usage >&2; return 2; }
    native=$(package_native_version "$format" "$version") || return 2
    filename=$(package_filename "$format" "$version") || return 2
    architecture=$(format_field "$format" architecture) || return 2
    # Every value is constrained by formats.tsv and native-version.sh; none needs JSON escaping.
    printf '{"format":"%s","version":"%s","nativeVersion":"%s","filename":"%s","architecture":"%s"}\n' \
        "$format" "$version" "$native" "$filename" "$architecture"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    set -euo pipefail
    package_info_main "$@"
fi
