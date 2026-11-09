# Reads packaging/formats.tsv, the table of native release package formats.
# Sourcing this file defines the functions below; it sets no shell options and
# runs nothing. Each function prints its result on stdout and returns 0, or
# prints a message on stderr and returns 2 for an unknown format, field or
# version.
#
#   format_ids                       every format id, one per line
#   format_field ID FIELD            one column of the row of ID
#   format_render ID FIELD VERSION   a template column with {version} filled in
#                                    for the release VERSION, in the row's style
#
# Fields: id family extension version_style filename native_version
# architecture dockerfile label.

formats_table() { printf '%s\n' "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)/formats.tsv"; }

format_ids() {
    local table
    table=$(formats_table) || return 2
    awk -F '\t' '!/^#/ && NF { print $1 }' "$table"
}

format_field() {
    local id=${1-} field=${2-} table column
    case "$field" in
        id) column=1 ;; family) column=2 ;; extension) column=3 ;; version_style) column=4 ;;
        filename) column=5 ;; native_version) column=6 ;; architecture) column=7 ;;
        dockerfile) column=8 ;; label) column=9 ;;
        *) echo "error: unknown package format field '$field'" >&2; return 2 ;;
    esac
    table=$(formats_table) || return 2
    awk -F '\t' -v id="$id" -v column="$column" '
        !/^#/ && $1 == id { print $column; found = 1 }
        END { exit found ? 0 : 1 }' "$table" && return 0
    echo "error: unknown package format '$id'; use one of: $(format_ids | tr '\n' ' ')" >&2
    return 2
}

format_render() {
    local id=${1-} field=${2-} version=${3-} template style native
    template=$(format_field "$id" "$field") || return 2
    style=$(format_field "$id" version_style) || return 2
    native=$("$(dirname -- "${BASH_SOURCE[0]}")/native-version.sh" --style "$style" "$version") || return 2
    printf '%s\n' "${template//\{version\}/$native}"
}
