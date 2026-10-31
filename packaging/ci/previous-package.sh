#!/usr/bin/env bash
# Reuses a verified published native package for every validation event.
# Missing tools/releases/assets and failed downloads visibly select the real
# synthetic upgrade check. Downloaded bytes with bad size/digest are fatal.
#
# package-info owns target filenames, architectures and native revisions.
# The supported version subset has the same order in dpkg, RPM and vercmp;
# compare its canonical tilde form with dpkg on the Ubuntu workflow host.
# Clean native runtime checks independently verify actual package metadata and
# strictly older ordering with the target's own package manager before install.

set -euo pipefail

usage() {
    echo "usage: $0 --format kubuntu-deb|fedora-rpm|arch-pkg --version <version> --output <dir>" >&2
    exit 2
}

format=""
version=""
output=""
while [ $# -gt 0 ]; do
    case "$1" in
        --format) format="${2-}"; shift 2 || usage ;;
        --version) version="${2-}"; shift 2 || usage ;;
        --output) output="${2-}"; shift 2 || usage ;;
        *) usage ;;
    esac
done
case "$format" in kubuntu-deb|fedora-rpm|arch-pkg) ;; *) usage ;; esac
[ -n "$version" ] && [ -n "$output" ] || usage

packaging_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
native_version() { "$packaging_dir/scripts/native-version.sh" "$1"; }
next="$(native_version "$version")" || exit 2
source "$packaging_dir/scripts/package-info.sh"

fallback() {
    echo "previous package: $*; the upgrade check uses a build of this source as 0.0.0 instead" >&2
    exit 0
}
for tool in gh jq dpkg sha256sum; do
    command -v "$tool" > /dev/null || fallback "$tool is not installed"
done

case "$format" in
    kubuntu-deb) asset_path='dist/kde/branch-truce_*_amd64-ubuntu26.04.deb' ;;
    fedora-rpm) asset_path='dist/kde/branch-truce-*-1.fc44.x86_64.rpm' ;;
    arch-pkg) asset_path='dist/kde/branch-truce-*-1-x86_64.pkg.tar.zst' ;;
esac
label="$(jq -r --arg path "$asset_path" '
    .plugins[] | select(type == "array" and .[0] == "@semantic-release/github")
    | .[1].assets[] | select(.path == $path) | .label' "$packaging_dir/../.releaserc.json")"
if [ -z "$label" ] || [ "$(wc -l <<<"$label")" != 1 ]; then
    echo "error: .releaserc.json does not give one label for the $format asset" >&2
    exit 1
fi

if ! tags="$(gh release list --exclude-drafts --exclude-pre-releases --limit 1000 \
    --json tagName --jq '.[].tagName' 2>&1)"; then
    fallback "cannot list the releases ($(head -n 1 <<<"$tags"))"
fi

candidates=()
while read -r tag; do
    [ -n "$tag" ] && [ "${tag#v}" != "$tag" ] || continue
    native="$(native_version "${tag#v}" 2> /dev/null)" || continue
    dpkg --compare-versions "$native" lt "$next" || continue
    candidates+=("$native $tag")
done <<<"$tags"

asset_of() {
    local assets
    if ! assets="$(gh release view "$2" --json assets 2>&1)"; then
        head -n 1 <<<"$assets" >&2
        return 1
    fi
    package_info "$format" "${2#v}"
    jq -c --arg name "$package_filename" --arg label "$label" '
        [.assets[]
         | select(.label == $label and .state == "uploaded")
         | select((.digest // "") | test("^sha256:[0-9a-f]{64}$"))
         | select(.name == $name)]
        | if length == 1 then .[0] else empty end' <<<"$assets"
}

chosen=""
while [ "${#candidates[@]}" -gt 0 ]; do
    best=0
    for i in "${!candidates[@]}"; do
        if dpkg --compare-versions "${candidates[$i]%% *}" gt "${candidates[$best]%% *}"; then
            best=$i
        fi
    done
    native="${candidates[$best]%% *}"
    tag="${candidates[$best]#* }"
    unset 'candidates[best]'
    candidates=("${candidates[@]}")
    asset="$(asset_of "$native" "$tag")" || fallback "cannot read the assets of $tag"
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
rm -f "$file"
download="$(mktemp -d "$output/.download.XXXXXX")"
trap 'rm -rf "$download"' EXIT
if ! gh release download "$chosen" --pattern "$name" --dir "$download" >&2; then
    fallback "downloading $name from $chosen failed"
fi
if [ ! -f "$download/$name" ] || [ "$(stat -c %s "$download/$name")" != "$size" ] \
    || [ "$(sha256sum "$download/$name" | cut -d' ' -f1)" != "$digest" ]; then
    echo "error: $name from $chosen does not match its published size $size and SHA-256 $digest" >&2
    exit 1
fi
mv "$download/$name" "$file"
echo "previous package: $name from $chosen, SHA-256 $digest, for the upgrade to $version" >&2
echo "$file"
