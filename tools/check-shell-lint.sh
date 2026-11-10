#!/usr/bin/env bash
# Lints the shell scripts of the packaging and tooling layers with the pinned
# ShellCheck at warning severity, following sourced files.
#
#   tools/check-shell-lint.sh
#
# Exit status: 0 no warning, 1 warnings or a missing or wrong ShellCheck, 2 usage error.
set -euo pipefail
case "${1-}" in
    '') ;;
    -h|--help) echo "Usage: $0"; exit 0 ;;
    *) echo "Usage: $0" >&2; exit 2 ;;
esac
root=$(cd -- "${BASH_SOURCE[0]%/*}/.." && pwd -P)
required=0.10.0
command -v shellcheck >/dev/null || {
    echo "shellcheck $required is required: python3 -m pip install --require-hashes -r tools/shellcheck.txt" >&2
    exit 1
}
version=$(shellcheck --version | sed -n 's/^version: //p')
[[ "$version" == "$required" ]] || { echo "shellcheck $required is required, found ${version:-an unknown version}." >&2; exit 1; }
cd -- "$root"
shopt -s globstar nullglob
scripts=(tools/**/*.sh packaging/**/*.sh tests/packaging/*.sh tests/tools/*.sh)
shellcheck --external-sources --severity=warning --format=gcc -- "${scripts[@]}"
printf '%s shell scripts passed shellcheck %s\n' "${#scripts[@]}" "$required"
