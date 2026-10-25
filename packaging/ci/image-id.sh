#!/usr/bin/env bash
# Prints the identity of the CI images for a format: the SHA-256 of its
# Dockerfile, every file it COPYs, and BRANCH_TRUCE_KDE_CI_REFRESH.

set -euo pipefail

usage() {
    echo "usage: $0 --format kubuntu-deb|tuxedo-deb" >&2
    exit 2
}

format=""
while [ $# -gt 0 ]; do
    case "$1" in
        --format) format="${2-}"; shift 2 || usage ;;
        *) usage ;;
    esac
done
case "$format" in
    kubuntu-deb) dockerfile=ci/kubuntu.Dockerfile ;;
    tuxedo-deb) dockerfile=ci/tuxedo.Dockerfile ;;
    *) usage ;;
esac

cd "$(dirname "${BASH_SOURCE[0]}")/.."

mapfile -t sources < <(awk '
    toupper($1) == "COPY" {
        for (i = 2; i < NF; i++) print $i
    }' "$dockerfile")
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
