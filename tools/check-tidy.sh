#!/usr/bin/env bash
# Run the pinned clang-tidy over every project source in a configured and built tree.
# Usage: tools/check-tidy.sh <build-directory> [jobs]
set -euo pipefail
root=$(cd -- "${BASH_SOURCE[0]%/*}/.." && pwd -P)
required=23.1.3
case "${1:-}" in -h|--help) echo "Usage: $0 <build-directory-with-compile_commands.json> [jobs]"; exit 0 ;; esac
build=${1:-}
jobs=${2:-2}
[[ -n "$build" && -f "$build/compile_commands.json" ]] || {
    echo "Usage: $0 <build-directory-with-compile_commands.json> [jobs]" >&2
    exit 2
}
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "Jobs must be a positive integer." >&2; exit 2; }
command -v clang-tidy >/dev/null || {
    echo "clang-tidy $required is required: python3 -m pip install --require-hashes -r tools/clang-tools.txt" >&2
    exit 1
}
version=$(clang-tidy --version | sed -n 's/.*LLVM version \([0-9.]*\).*/\1/p')
[[ "$version" == "$required" ]] || {
    echo "clang-tidy $required is required, found ${version:-an unknown version}." >&2
    exit 1
}
manifest=$(mktemp)
trap 'rm -f -- "$manifest"' EXIT
python3 -I - "$build/compile_commands.json" "$root" > "$manifest" <<'PY' || exit $?
import json, os, sys
entries = json.load(open(sys.argv[1]))
root = sys.argv[2].rstrip("/") + "/"
seen = set()
for entry in entries:
    path = os.path.normpath(os.path.join(entry["directory"], entry["file"]))
    relative = path[len(root):] if path.startswith(root) else ""
    if relative.split("/", 1)[0] in ("src", "tests", "tools") and path not in seen:
        seen.add(path)
        sys.stdout.write(path + "\0")
PY
mapfile -d '' files < "$manifest"
((${#files[@]} > 0)) || { echo "No project sources in $build/compile_commands.json." >&2; exit 1; }
printf '%s\0' "${files[@]}" | xargs -0 -P "$jobs" -n 1 clang-tidy -p "$build" --quiet
printf '%s files passed clang-tidy %s\n' "${#files[@]}" "$required"
