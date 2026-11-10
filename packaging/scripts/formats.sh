#!/usr/bin/env bash
# Reads packaging/formats.tsv, the table of native release package formats,
# through tsv.sh. It is the one reader of that table: the other shell scripts
# source it, and the workflow and packaging/ci/*.js run it.
#
#   formats.sh ids                    every format id, one per line
#   formats.sh field ID FIELD         one column of the row of ID
#   formats.sh matrix                 the CI matrix as JSON: {"include": [{"format",
#                                     "target", "native_fidelity"}, ...]}
#
# Exit status: 0, or 2 with one "error: ..." line on stderr for a usage error,
# an unknown format or an unknown field.
#
# Sourcing defines these functions and runs nothing; it sets no shell options.
# Each prints its result on stdout and returns 0, or returns 2 as above.
#
#   format_ids                        every format id, one per line
#   format_field ID FIELD             one column of the row of ID
#   format_render ID FIELD VERSION    a template column with {version} filled in
#                                     for the release VERSION, in the row's style
#   format_matrix                     the CI matrix above
#
# The fields are the names in the table's header line.

source "$(dirname -- "${BASH_SOURCE[0]}")/tsv.sh"

formats_table() { printf '%s\n' "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)/formats.tsv"; }

format_ids() { tsv_keys "$(formats_table)"; }

format_field() { tsv_field "$(formats_table)" "${1-}" "${2-}"; }

format_render() {
    local id=${1-} field=${2-} version=${3-} template style native
    template=$(format_field "$id" "$field") || return 2
    style=$(format_field "$id" version_style) || return 2
    native=$("$(dirname -- "${BASH_SOURCE[0]}")/native-version.sh" --style "$style" "$version") || return 2
    printf '%s\n' "${template//\{version\}/$native}"
}

# _format_json VALUE: VALUE as a JSON string.
_format_json() { local value=${1//\\/\\\\}; printf '"%s"' "${value//\"/\\\"}"; }

# The values come from the table itself; quotes and backslashes are escaped anyway.
format_matrix() {
    local id target fidelity ids separator=''
    ids=$(format_ids) || return 2
    printf '{"include":['
    while IFS= read -r id; do
        target=$(format_field "$id" target) && fidelity=$(format_field "$id" native_fidelity) || return 2
        printf '%s{"format":%s,"target":%s,"native_fidelity":%s}' "$separator" \
            "$(_format_json "$id")" "$(_format_json "$target")" "$(_format_json "$fidelity")"
        separator=,
    done <<<"$ids"
    printf ']}\n'
}

_formats_main() {
    case "${1-}:$#" in
        ids:1) format_ids ;;
        field:3) format_field "$2" "$3" ;;
        matrix:1) format_matrix ;;
        -h:1|--help:1) _formats_usage ;;
        *) _formats_usage >&2; return 2 ;;
    esac
}
_formats_usage() { echo "usage: formats.sh ids | field ID FIELD | matrix"; }

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    set -euo pipefail
    _formats_main "$@"
fi
