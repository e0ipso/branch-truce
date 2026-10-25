#!/usr/bin/env bash
# Prepares a disposable container as a packaging target for Branch Truce.
#
#   prepare-target.sh --format kubuntu-deb|tuxedo-deb [--build-deps] [--refresh]
#
# kubuntu-deb: an ubuntu:26.04 (Kubuntu 26.04 LTS) system needs only its
#              standard package lists.
# tuxedo-deb:  an ubuntu:24.04 (noble) system configured with the Tuxedo OS
#              24.04 vendor repositories and archive keyring so its Qt 6.10+
#              and KDE Frameworks 6.24+ packages are used instead of stock
#              noble's Qt 6.4.
# --build-deps also installs the build dependencies from packaging/debian/control.
# Without it the system stays clean apart from the package lists, ready for
# packaging/scripts/check-package.sh. --refresh fetches the package lists again.
#
# Run as root inside a disposable container only.

set -euo pipefail

usage() {
    echo "usage: $0 --format kubuntu-deb|tuxedo-deb [--build-deps] [--refresh]" >&2
    exit 2
}

format=""
build_deps=false
refresh=false
while [ $# -gt 0 ]; do
    case "$1" in
        --format) format="${2-}"; shift 2 || usage ;;
        --build-deps) build_deps=true; shift ;;
        --refresh) refresh=true; shift ;;
        *) usage ;;
    esac
done

case "$format" in
    kubuntu-deb|tuxedo-deb) ;;
    *) usage ;;
esac

packaging_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export DEBIAN_FRONTEND=noninteractive

if [ "$format" = tuxedo-deb ] && [ ! -f /etc/apt/sources.list.d/tuxedo-os.sources ]; then
    apt-get update -q
    apt-get install -y -q --no-install-recommends ca-certificates
    install -d -m 0755 /usr/share/keyrings /etc/apt/sources.list.d
    install -m 0644 "$packaging_dir/ci/tuxedo-archive-keyring.gpg" /usr/share/keyrings/tuxedo-archive-keyring.gpg
    cat > /etc/apt/sources.list.d/tuxedo-os.sources <<'EOF'
Types: deb
URIs: https://deb.tuxedocomputers.com/ubuntu
Suites: noble
Components: main
Signed-By: /usr/share/keyrings/tuxedo-archive-keyring.gpg

Types: deb
URIs: https://txos.tuxedocomputers.com/ubuntu
Suites: noble
Components: main
Signed-By: /usr/share/keyrings/tuxedo-archive-keyring.gpg

Types: deb
URIs: https://txos.tuxedocomputers.com/ubuntu-plasma
Suites: noble noble-updates
Components: main
Signed-By: /usr/share/keyrings/tuxedo-archive-keyring.gpg

Types: deb
URIs: https://txos-extra.tuxedocomputers.com/ubuntu
Suites: noble
Components: main
Signed-By: /usr/share/keyrings/tuxedo-archive-keyring.gpg
EOF
    refresh=true
fi

if $refresh || ! compgen -G "/var/lib/apt/lists/*_Packages*" > /dev/null; then
    apt-get update -q
fi

if $build_deps; then
    apt-get install -y -q --no-install-recommends build-essential dpkg-dev
    apt-get build-dep -y -q "$packaging_dir"
fi
