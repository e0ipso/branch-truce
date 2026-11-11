#!/usr/bin/env bash
# Run version checks anywhere; native vercmp checks also run in the Arch image.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
arch_native_version() { "$root/packaging/scripts/native-version.sh" --style plain "$@"; }
expect_version() {
    local got
    got="$(arch_native_version "$1")"
    [ "$got" = "$2" ] || { echo "FAIL: $1 mapped to $got, expected $2" >&2; exit 1; }
}
expect_version 1.2.3 1.2.3
expect_version 0.0.0 0.0.0
expect_version 1.2.3-alpha 1.2.3alpha
expect_version 1.2.3-alpha.2 1.2.3alpha2
expect_version 1.2.3-rc.10 1.2.3rc10
for bad in '' 01.2.3 1.02.3 1.2.03 1.2 1.2.3-rc.0 1.2.3-rc.01 \
    1.2.3-rc1 1.2.3-rc.more 1.2.3-rc.1.2 1.2.3+build '1.2.3;true'; do
    if arch_native_version "$bad" >/dev/null 2>&1; then
        echo "FAIL: unsupported version '$bad' accepted" >&2; exit 1
    fi
done
if command -v vercmp >/dev/null; then
    versions=(1.2.3-alpha 1.2.3-alpha.1 1.2.3-alpha.2 1.2.3-alpha.10 1.2.3-beta 1.2.3-rc 1.2.3-rc.1 1.2.3 1.2.4-alpha 2.0.0)
    for ((i=1; i<${#versions[@]}; i++)); do
        a="$(arch_native_version "${versions[i-1]}")-1"
        b="$(arch_native_version "${versions[i]}")-1"
        [ "$(vercmp "$a" "$b")" -lt 0 ] || { echo "FAIL: $a is not older than $b" >&2; exit 1; }
    done
    echo 'ok: native vercmp prerelease, stable and cross-release ordering'
fi
echo 'ok: Arch version conversion and unsupported version rejection'
