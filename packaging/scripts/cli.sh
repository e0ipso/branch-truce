# shellcheck shell=bash
# Command-line parsing shared by the packaging scripts and the tools under
# tools/. Sourcing defines the functions below; it sets no shell options and
# runs nothing.
#
#   cli_parse OPTIONS OPERANDS SPEC [ARG...]
#       Reads ARG... into the associative array named OPTIONS and the indexed
#       array named OPERANDS, or rejects every operand when OPERANDS is -.
#       SPEC lists the long options, separated by spaces: NAME= takes a value,
#       NAME alone is a flag that is set to 1, and a trailing ! marks an
#       option that must be given a non-empty value. -h and --help set
#       OPTIONS[help] and end the parse; -- ends the options. A later value
#       replaces an earlier one. Returns 0 when parsed, 1 when help was asked
#       for, and 2 on a usage error, with one "error: ..." line on stderr.
#   cli_usage STATUS USAGE-COMMAND
#       Reports a cli_parse status other than 0: USAGE-COMMAND's text on
#       stdout and status 0 for help, or on stderr and status 2 otherwise.
#
# A script calls them as
#   declare -A options=()
#   cli_parse options - 'format=! version=!' "$@" || { cli_usage $? usage; exit; }
# Names that start with _cli_ are reserved for this file.

cli_parse() {
    local -n _cli_options=$1
    local _cli_operands_name=$2 _cli_spec=$3 _cli_word _cli_name
    local -a _cli_required=()
    local -A _cli_kind=()
    shift 3
    for _cli_word in $_cli_spec; do
        _cli_name=${_cli_word%!}
        if [[ "$_cli_word" == *! ]]; then _cli_required+=("${_cli_name%=}"); fi
        if [[ "$_cli_name" == *= ]]; then _cli_kind[${_cli_name%=}]=value; else _cli_kind[$_cli_name]=flag; fi
    done
    if [[ "$_cli_operands_name" != - ]]; then local -n _cli_operands=$_cli_operands_name; fi
    while (($# > 0)); do
        case "$1" in
            -h|--help) _cli_options['help']=1; return 1 ;;
            --) shift; break ;;
            --?*)
                _cli_name=${1#--}
                case "${_cli_kind[$_cli_name]-}" in
                    flag) _cli_options[$_cli_name]=1; shift ;;
                    value)
                        (($# >= 2)) || { echo "error: $1 needs a value" >&2; return 2; }
                        _cli_options[$_cli_name]=$2
                        shift 2 ;;
                    *) echo "error: unknown option $1" >&2; return 2 ;;
                esac ;;
            -?*) echo "error: unknown option $1" >&2; return 2 ;;
            *)
                [[ "$_cli_operands_name" != - ]] || { echo "error: unexpected argument $1" >&2; return 2; }
                _cli_operands+=("$1")
                shift ;;
        esac
    done
    if (($# > 0)); then
        [[ "$_cli_operands_name" != - ]] || { echo "error: unexpected argument $1" >&2; return 2; }
        _cli_operands+=("$@")
    fi
    for _cli_name in "${_cli_required[@]}"; do
        [[ -n "${_cli_options[$_cli_name]-}" ]] || { echo "error: --$_cli_name is required" >&2; return 2; }
    done
}

cli_usage() {
    if [[ "$1" == 1 ]]; then
        "$2"
        return 0
    fi
    "$2" >&2
    return 2
}
