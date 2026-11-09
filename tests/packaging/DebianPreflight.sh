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
touch "$work/candidate.deb"
check() { PATH="$work/bin:$PATH" bash "$root/packaging/scripts/check-package.sh" --format kubuntu-deb "$@" > "$work/result" 2>&1; }
# reject MESSAGE ARGUMENTS...: the checker refuses before anything on the target changes.
reject() {
    local message=$1
    shift
    if check "$@"; then echo "FAIL: accepted $message" >&2; exit 1; fi
    grep -qF -- "$message" "$work/result" || { cat "$work/result"; exit 1; }
    if grep -qF 'before metadata rejection' "$work/result"; then cat "$work/result"; exit 1; fi
}
for field in Package Version Architecture; do
    export DEB_NAME=branch-truce DEB_VERSION=1.2.3 DEB_ARCH=amd64
    case "$field" in Package) DEB_NAME=other ;; Version) DEB_VERSION=99.0.0 ;; Architecture) DEB_ARCH=arm64 ;; esac
    reject "candidate identity $DEB_NAME|$DEB_VERSION|$DEB_ARCH differs from branch-truce|1.2.3|amd64" \
        --version 1.2.3 --package "$work/candidate.deb"
done
export DEB_NAME=branch-truce DEB_VERSION=1.2.3 DEB_ARCH=amd64
# The payload must be exactly the installed files, plus the Debian documentation.
reject 'payload differs from the expected files' --version 1.2.3 --package "$work/candidate.deb"
# Usage errors and unsupported versions exit 2.
for arguments in '--version 1.2.3' '--version 1.2.3-rc.0 --package candidate.deb' '--version 1.2.3 --package candidate.rpm'; do
    status=0
    # shellcheck disable=SC2086
    (cd "$work" && touch candidate.rpm && PATH="$work/bin:$PATH" bash "$root/packaging/scripts/check-package.sh" \
        --format kubuntu-deb $arguments) > "$work/result" 2>&1 || status=$?
    [ "$status" = 2 ] || { cat "$work/result"; echo "FAIL: '$arguments' exited $status, not 2" >&2; exit 1; }
done
echo 'all Debian preflight rejections passed without target mutation'
