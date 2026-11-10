#!/usr/bin/env bash
# Copies the inputs of a native package build into a new source tree.
#
#   stage-source.sh [--tracked] --from <root> --to <dir> [PATH...]
#
# The inputs are the top-level CMake project, src/, the license, the desktop
# integration files under packaging/ and the branding pack's Linux icons and
# SVG exports, plus every extra PATH (relative to <root>). With --tracked,
# only files Git tracks, or would track (untracked but not ignored), are
# copied, so build outputs and other ignored files never reach a package;
# <root> must then be a Git work tree. Without it, the paths are copied as
# they are, for a <root> that is already such a copy. <dir> must not exist.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/cli.sh"

usage() { echo "usage: $0 [--tracked] --from <root> --to <dir> [PATH...]"; }
inputs=(CMakeLists.txt cmake src LICENSE
    packaging/CMakeLists.txt packaging/com.canpicasoft.branch-truce.desktop
    packaging/com.canpicasoft.branch-truce.metainfo.xml packaging/installed-files.txt
    assets/branding/platform/linux assets/branding/svg)
declare -A options=()
declare -a extra=()
cli_parse options extra 'tracked from=! to=!' "$@" || { cli_usage $? usage; exit; }
inputs+=("${extra[@]}")
from=${options[from]} to=${options[to]} tracked=false
[ -z "${options[tracked]-}" ] || tracked=true
[ -d "$from" ] || { echo "error: $from is not a directory" >&2; exit 2; }
[ ! -e "$to" ] || { echo "error: $to already exists" >&2; exit 2; }
mkdir -p -- "$to"
if $tracked; then
    (cd -- "$from" && git ls-files -z --cached --others --exclude-standard -- "${inputs[@]}") |
        while IFS= read -r -d '' file; do
            # A tracked file deleted in the work tree is not an input.
            if [ -e "$from/$file" ]; then printf '%s\0' "$file"; fi
        done |
        (cd -- "$from" && tar --null --files-from=- --create --file=-) |
        tar --extract --file=- --directory="$to"
else
    (cd -- "$from" && tar --create --file=- -- "${inputs[@]}") | tar --extract --file=- --directory="$to"
fi
