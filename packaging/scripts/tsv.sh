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

_tsv_read() {
    local table=${1-} mode=$2 key=$3 column=$4 status=0
    [[ -f "$table" && -r "$table" ]] || { echo "error: cannot read the table '$table'" >&2; return 2; }
    awk -F '\t' -v mode="$mode" -v key="$key" -v name="$column" '
        /^#/ || !NF { next }
        !header { header = NF; for (i = 1; i <= NF; i++) {
            if (!$i || columns[$i]++) invalid = 1; if ($i == name) want = i
        } next }
        NF != header || seen[$1]++ { invalid = 1; next }
        { for (i = 1; i <= NF; i++) if ($i == "") invalid = 1; keys[++count] = $1; values[$1] = $want }
        END {
            if (!header || !count || invalid) exit 5
            if (mode != "keys" && !want) exit 3
            if (mode == "field") { if (!(key in values)) exit 4; print values[key] }
            else for (i = 1; i <= count; i++)
                print keys[i] (mode == "column" ? "\t" values[keys[i]] : "")
        }' "$table" || status=$?
    case "$status" in
        0) return 0 ;;
        3) echo "error: ${table##*/} has no column '$column'" >&2 ;;
        4) echo "error: ${table##*/} has no row '$key'; use one of: $(tsv_keys "$table" | paste -sd ' ' -)" >&2 ;;
        *) echo "error: cannot read the table '$table': malformed or empty" >&2 ;;
    esac
    return 2
}

tsv_keys() { _tsv_read "${1-}" keys '' ''; }
tsv_field() { _tsv_read "${1-}" field "${2-}" "${3-}"; }
tsv_column() { _tsv_read "${1-}" column '' "${2-}"; }
