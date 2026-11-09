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

source "$(dirname -- "${BASH_SOURCE[0]}")/formats.sh"

# package_filename FORMAT VERSION
package_filename() { format_render "${1-}" filename "${2-}"; }

# package_native_version FORMAT VERSION
package_native_version() { format_render "${1-}" native_version "${2-}"; }

package_info_main() {
    local format='' version='' native filename architecture
    local usage="usage: package-info.sh --format $(format_ids | paste -sd '|') --version <semver>"
    while [ $# -gt 0 ]; do
        case "$1" in
            -h|--help) printf '%s\n' "$usage"; return 0 ;;
            --format|--version)
                [ $# -ge 2 ] || { printf '%s\n' "$usage" >&2; return 2; }
                if [ "$1" = --format ]; then format=$2; else version=$2; fi
                shift 2 ;;
            *) printf '%s\n' "$usage" >&2; return 2 ;;
        esac
    done
    [ -n "$format" ] && [ -n "$version" ] || { printf '%s\n' "$usage" >&2; return 2; }
    format_field "$format" id >/dev/null || { printf '%s\n' "$usage" >&2; return 2; }
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
