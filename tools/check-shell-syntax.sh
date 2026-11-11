#!/usr/bin/env bash
# Checks the syntax of every project shell script, including additions,
# without executing their commands, and rejects lines that start with a bare
# `! command`: Bash ignores errexit for a negated command, so such a line
# never fails a script, even under set -e. Write `! command || fail ...` or
# `if command; then fail ...; fi` instead.
#
#   tools/check-shell-syntax.sh
#
# Exit status: 0 every script passed, 1 a script failed, 2 usage error.
set -euo pipefail
case "${1-}" in
    '') ;;
    -h|--help) echo "Usage: $0"; exit 0 ;;
    *) echo "Usage: $0" >&2; exit 2 ;;
esac
root=$(cd -- "${BASH_SOURCE[0]%/*}/.." && pwd -P)
shopt -s globstar nullglob dotglob
scripts=("$root"/tools/**/*.sh "$root"/tests/**/*.sh "$root"/packaging/**/*.sh)
((${#scripts[@]} > 0)) || { echo 'No project shell scripts found.' >&2; exit 1; }
checked=0
failures=0
for script in "${scripts[@]}"; do
    [[ -f "$script" ]] || continue
    # A syntax error fails this script but never stops the check of the others.
    if ! bash -n "$script"; then
        printf '%s: syntax error\n' "${script#"$root/"}" >&2
        failures=$((failures + 1))
    fi
    # A here-document can hold such a line as data; those scripts are not exempt,
    # so keep generated scripts free of it too.
    if negated=$(grep -nE '^[[:space:]]*![[:space:]]' "$script" | grep -v '||'); then
        printf '%s:%s: a bare negated command never fails the script\n' "${script#"$root/"}" "${negated%%:*}" >&2
        failures=$((failures + 1))
    fi
    checked=$((checked + 1))
    printf 'Syntax checked: %s\n' "${script#"$root/"}"
done
((checked > 0)) || { echo 'No project shell files found.' >&2; exit 1; }
((failures == 0)) || exit 1
printf '%s project shell scripts passed syntax checks\n' "$checked"
