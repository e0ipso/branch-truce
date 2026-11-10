# shellcheck shell=bash
# Reads the tab-separated tables under packaging/ (formats.tsv,
# minimum-versions.tsv, validation-targets.tsv). Sourcing defines the
# functions below; it sets no shell options and runs nothing.
#
# A table has comment lines starting with #, blank lines, one header line that
# names the columns, and then one row per key, the key in the first column.
# Columns are found by their header name, never by position.
#
#   tsv_keys TABLE              the key of every row, one per line
#   tsv_field TABLE KEY COLUMN  that column of the row of KEY
#   tsv_column TABLE COLUMN     KEY<TAB>VALUE for every row
#
# Each prints on stdout and returns 0, or prints one "error: ..." line on
# stderr and returns 2 for an unreadable table, an unknown key or an unknown
# column.

tsv_keys() {
    [[ -r "${1-}" ]] || { echo "error: cannot read the table '${1-}'" >&2; return 2; }
    awk -F '\t' '/^#/ || !NF { next } !header { header = 1; next } { print $1 }' "$1"
}

tsv_field() {
    local table=${1-} key=${2-} column=${3-} status=0
    [[ -r "$table" ]] || { echo "error: cannot read the table '$table'" >&2; return 2; }
    awk -F '\t' -v key="$key" -v name="$column" '
        /^#/ || !NF { next }
        !header { header = 1; for (i = 1; i <= NF; i++) if ($i == name) want = i; if (!want) exit 3; next }
        $1 == key { print $want; found = 1; exit }
        END { if (!want) exit 3; if (!found) exit 4 }' "$table" || status=$?
    case "$status" in
        0) return 0 ;;
        3) echo "error: ${table##*/} has no column '$column'" >&2 ;;
        4) echo "error: ${table##*/} has no row '$key'; use one of: $(tsv_keys "$table" | paste -sd ' ' -)" >&2 ;;
        *) echo "error: cannot read the table '$table'" >&2 ;;
    esac
    return 2
}

tsv_column() {
    local table=${1-} column=${2-} status=0
    [[ -r "$table" ]] || { echo "error: cannot read the table '$table'" >&2; return 2; }
    awk -F '\t' -v name="$column" '
        /^#/ || !NF { next }
        !header { header = 1; for (i = 1; i <= NF; i++) if ($i == name) want = i; if (!want) exit 3; next }
        { print $1 "\t" $want }' "$table" || status=$?
    ((status == 0)) && return 0
    echo "error: ${table##*/} has no column '$column'" >&2
    return 2
}
