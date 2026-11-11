#!/usr/bin/env bash
# Prints the identity of the CI images of a format: the SHA-256 of its
# Dockerfile (from packaging/formats.tsv), every file it COPYs, and
# BRANCH_TRUCE_KDE_CI_REFRESH.
#
#   image-id.sh --format <id>
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/../scripts/cli.sh"
source "$(dirname -- "${BASH_SOURCE[0]}")/../scripts/formats.sh"

usage() { echo "usage: $0 --format $(format_ids | paste -sd '|' -)"; }
declare -A options=()
cli_parse options - 'format=!' "$@" || { cli_usage $? usage; exit; }
format=${options[format]}
dockerfile=$(format_field "$format" dockerfile) || { usage >&2; exit 2; }

cd "$(dirname "${BASH_SOURCE[0]}")/.."

copied=$(awk '
    toupper($1) == "COPY" {
        for (i = 2; i < NF; i++) print $i
    }' "$dockerfile") || exit 1
sources=()
[ -z "$copied" ] || mapfile -t sources <<<"$copied"
for source in "${sources[@]}"; do
    case "$source" in
        -*|*'*'*|*'?'*|*'['*)
            echo "error: $dockerfile: COPY $source is not a plain file; extend $0" >&2
            exit 1 ;;
    esac
    [ -f "$source" ] || { echo "error: $dockerfile copies $source, which is not a file" >&2; exit 1; }
done

{
    printf 'refresh %s\n' "${BRANCH_TRUCE_KDE_CI_REFRESH:-}"
    for file in "$dockerfile" "${sources[@]}"; do
        printf 'file %s %s\n' "$file" "$(sha256sum < "$file" | cut -d' ' -f1)"
    done
} | sha256sum | cut -d' ' -f1
