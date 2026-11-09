#!/usr/bin/env bash
# Check the formatting of tracked C++ sources with the pinned clang-format. --fix rewrites them.
set -euo pipefail
root=$(cd -- "${BASH_SOURCE[0]%/*}/.." && pwd -P)
required=23.1.3
case "${1:-}" in
    '') mode=(--dry-run -Werror) done=checked ;;
    --fix) mode=(-i) done=formatted ;;
    -h|--help) echo "Usage: $0 [--fix]"; exit 0 ;;
    *) echo "Usage: $0 [--fix]" >&2; exit 2 ;;
esac
command -v clang-format >/dev/null || {
    echo "clang-format $required is required: python3 -m pip install --require-hashes -r tools/clang-tools.txt" >&2
    exit 1
}
version=$(clang-format --version | sed -n 's/.*clang-format version \([0-9.]*\).*/\1/p')
[[ "$version" == "$required" ]] || {
    echo "clang-format $required is required, found ${version:-an unknown version}." >&2
    exit 1
}
mapfile -d '' files < <(git -C "$root" ls-files -z -- '*.cpp' '*.h')
((${#files[@]} > 0)) || { echo 'No C++ sources found.' >&2; exit 1; }
cd "$root"
clang-format "${mode[@]}" -- "${files[@]}"
printf '%s C++ files %s with clang-format %s\n' "${#files[@]}" "$done" "$required"
