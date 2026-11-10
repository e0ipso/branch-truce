#!/usr/bin/env bash
# Reuses a verified published native package for every validation event.
# Missing tools/releases/assets and failed downloads visibly select the real
# synthetic upgrade check. Downloaded bytes with bad size/digest are fatal.
#
#   previous-package.sh --format <id> --version <semver> --output <dir>
#
# packaging/formats.tsv owns the file names and release asset labels.
# The supported version subset has the same order in dpkg, RPM and vercmp,
# and in sort -V on its canonical tilde form.
# Clean native runtime checks independently verify actual package metadata and
# strictly older ordering with the target's own package manager before install.

set -euo pipefail

source "$(dirname -- "${BASH_SOURCE[0]}")/../scripts/cli.sh"
source "$(dirname -- "${BASH_SOURCE[0]}")/../scripts/package-info.sh"
usage() { echo "usage: $0 --format $(format_ids | paste -sd '|' -) --version <semver> --output <dir>"; }

declare -A options=()
cli_parse options - 'format=! version=! output=!' "$@" || { cli_usage $? usage; exit; }
format=${options[format]} version=${options[version]} output=${options[output]}
label=$(format_field "$format" label) || { usage >&2; exit 2; }
packaging_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
native_version() { "$packaging_dir/scripts/native-version.sh" "$1"; }
next="$(native_version "$version")" || exit 2

fallback() {
    echo "previous package: $*; the upgrade check uses a build of this source as 0.0.0 instead" >&2
    exit 0
}
for tool in gh jq sha256sum timeout; do
    command -v "$tool" > /dev/null || fallback "$tool is not installed"
done

if ! tags="$(timeout --kill-after=5s 30s gh release list --exclude-drafts --exclude-pre-releases --limit 1000 \
    --json tagName --jq '.[].tagName' 2>&1)"; then
    fallback "cannot list the releases ($(head -n 1 <<<"$tags"))"
fi

# Releases strictly older than this version, newest first. The supported
# versions sort the same with sort -V as with dpkg, RPM and vercmp.
candidates=()
while read -r tag; do
    [ -n "$tag" ] && [ "${tag#v}" != "$tag" ] || continue
    native="$(native_version "${tag#v}" 2> /dev/null)" || continue
    [ "$native" != "$next" ] && [ "$(printf '%s\n' "$native" "$next" | sort -V | head -n 1)" = "$native" ] || continue
    candidates+=("$native $tag")
done <<<"$tags"
ordered=$(printf '%s\n' "${candidates[@]}" | sed '/^$/d' | sort -r -V -k1,1) || exit 1
candidates=()
[ -z "$ordered" ] || mapfile -t candidates <<<"$ordered"

# asset_of TAG: the release's single uploaded asset with this format's file
# name and label and a SHA-256 digest, as JSON, or nothing.
asset_of() {
    local assets name
    if ! assets="$(timeout --kill-after=5s 30s gh release view "$1" --json assets 2>&1)"; then
        head -n 1 <<<"$assets" >&2
        return 1
    fi
    name=$(package_filename "$format" "${1#v}") || return 1
    jq -c --arg name "$name" --arg label "$label" '
        [.assets[]
         | select(.label == $label and .state == "uploaded")
         | select((.digest // "") | test("^sha256:[0-9a-f]{64}$"))
         | select(.name == $name)]
        | if length == 1 then .[0] else empty end' <<<"$assets"
}

chosen=""
for candidate in "${candidates[@]}"; do
    native="${candidate%% *}"
    tag="${candidate#* }"
    asset="$(asset_of "$tag")" || fallback "cannot read the assets of $tag"
    if [ -n "$asset" ]; then
        chosen="$tag"
        break
    fi
    echo "previous package: $tag has no single \"$label\" asset for $native; trying older releases" >&2
done
[ -n "$chosen" ] || fallback "no published release older than $version has a \"$label\" asset"

name="$(jq -r .name <<<"$asset")"
size="$(jq -r .size <<<"$asset")"
digest="$(jq -r '.digest | ltrimstr("sha256:")' <<<"$asset")"
mkdir -p "$output"
file="$output/$name"
download="$(mktemp -d "$output/.download.XXXXXX")"
trap 'rm -rf "$download"' EXIT
if ! timeout --kill-after=5s 120s gh release download "$chosen" --pattern "$name" --dir "$download" >&2; then
    fallback "downloading $name from $chosen failed"
fi
if [ ! -f "$download/$name" ] || [ "$(stat -c %s "$download/$name")" != "$size" ] \
    || [ "$(sha256sum "$download/$name" | cut -d' ' -f1)" != "$digest" ]; then
    echo "error: $name from $chosen does not match its published size $size and SHA-256 $digest" >&2
    exit 1
fi
mv -T -- "$download/$name" "$file"
echo "previous package: $name from $chosen, SHA-256 $digest, for the upgrade to $version" >&2
echo "$file"
