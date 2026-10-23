#!/usr/bin/env bash
# Check project scripts, including additions, without executing their commands.
set -euo pipefail
root=$(cd -- "${BASH_SOURCE[0]%/*}/.." && pwd -P)
shopt -s globstar nullglob dotglob
scripts=("$root"/tools/**/*.sh "$root"/tests/**/*.sh)
((${#scripts[@]} > 0)) || { echo 'No project shell scripts found.' >&2; exit 1; }
checked=0
for script in "${scripts[@]}"; do
    [[ -f "$script" ]] || continue
    bash -n "$script"
    checked=$((checked + 1))
    printf 'Syntax checked: %s\n' "${script#"$root/"}"
done
((checked > 0)) || { echo 'No project shell files found.' >&2; exit 1; }
printf '%s project shell scripts passed syntax checks\n' "$checked"
