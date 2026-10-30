#!/usr/bin/env bash
# DEB metadata adapter with mutation sentinels. No native tools or installation.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/tree/DEBIAN" "$work/tree/usr/bin" "$work/bin"
printf fixture > "$work/tree/usr/bin/branch-truce"
cat > "$work/bin/apt-get" <<'STUB'
#!/usr/bin/env bash
echo 'FAIL: package manager reached before rejection' >&2
exit 91
STUB
cat > "$work/bin/mktemp" <<'STUB'
#!/usr/bin/env bash
echo 'FAIL: target changed before metadata rejection' >&2
exit 92
STUB
cat > "$work/bin/dpkg-query" <<'STUB'
#!/usr/bin/env bash
exit 0
STUB
chmod +x "$work/bin/"*
cat > "$work/bin/dpkg-deb" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail
case "$1" in
    -f)
        case "$3" in Package) echo "$DEB_NAME" ;; Version) echo "$DEB_VERSION" ;; Architecture) echo "$DEB_ARCH" ;; *) echo '' ;; esac ;;
    --fsys-tarfile) tar -C "$DEB_TREE" -cf - ./usr ;;
    --ctrl-tarfile) tar -C "$DEB_TREE/DEBIAN" -cf - . ;;
    *) exit 9 ;;
esac
STUB
chmod +x "$work/bin/dpkg-deb"
export DEB_TREE="$work/tree"
for field in Package Version Architecture; do
    export DEB_NAME=branch-truce DEB_VERSION=1.2.3 DEB_ARCH=amd64
    case "$field" in Package) DEB_NAME=other ;; Version) DEB_VERSION=99.0.0 ;; Architecture) DEB_ARCH=arm64 ;; esac
    touch "$work/candidate.deb"
    if PATH="$work/bin:$PATH" bash "$root/packaging/scripts/check-package.sh" --version 1.2.3 --package "$work/candidate.deb" > "$work/result" 2>&1; then
        echo "FAIL: accepted invalid $field" >&2; exit 1
    fi
    grep -qF 'invalid candidate metadata; target unchanged' "$work/result" || { cat "$work/result"; exit 1; }
    if grep -qF 'before metadata rejection' "$work/result"; then cat "$work/result"; exit 1; fi
done
echo 'all Debian preflight rejections passed without target mutation'
