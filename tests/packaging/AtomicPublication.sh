#!/usr/bin/env bash
# Private stub metadata, concurrent writers, failed copy and source permission preservation.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
mkdir "$work/scripts" "$work/out"
cat "$root/packaging/scripts/builder-common.sh" > "$work/scripts/builder-common.sh"
printf 'package_filename() { echo release.pkg; }\n' > "$work/scripts/package-info.sh"
printf '#!/bin/sh\nexit 0\n' > "$work/scripts/package-identity.sh"
chmod +x "$work/scripts/package-identity.sh"
source "$work/scripts/builder-common.sh"
printf first > "$work/first"
printf second > "$work/second"
chmod 640 "$work/first" "$work/second"
cp() {
    command cp "$@"
    touch "$work/ready-${2##*/}"
    local deadline=$((SECONDS + 5))
    until [[ -f "$work/ready-first" && -f "$work/ready-second" ]]; do
        ((SECONDS < deadline)) || return 1
        sleep 0.01
    done
}
builder_publish stub 1.2.3 "$work/first" "$work/out" > "$work/result-first" & first=$!
builder_publish stub 1.2.3 "$work/second" "$work/out" > "$work/result-second" & second=$!
wait "$first" && wait "$second" || fail 'concurrent publication failed'
content=$(cat "$work/out/release.pkg")
[[ "$content" == first || "$content" == second ]] || fail 'publication contains mixed or missing bytes'
[[ $(stat -c %a "$work/out/release.pkg") == 640 ]] || fail 'source mode was lost'
cp() { printf partial > "$3"; return 42; }
if builder_publish stub 1.2.3 "$work/first" "$work/out"; then fail 'failed copy was published'; fi
[[ $(cat "$work/out/release.pkg") == "$content" ]] || fail 'failed copy replaced the destination'
[[ $(ls -A "$work/out") == release.pkg ]] || fail 'publication leaked staging files'
echo 'concurrent publication and failed-copy cleanup passed'
