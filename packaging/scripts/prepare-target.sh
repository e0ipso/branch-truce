#!/usr/bin/env bash
# Prepare only disposable native release containers. Developer artifact tooling
# uses its separate provisioning path under tools/ and packaging/<base>/.
set -euo pipefail

usage() {
    echo "usage: $0 --format kubuntu-deb|fedora-rpm|arch-pkg [--build-deps] [--refresh]" >&2
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
    kubuntu-deb|fedora-rpm|arch-pkg) ;;
    *) usage ;;
esac

packaging_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export DEBIAN_FRONTEND=noninteractive

case "$format" in
    fedora-rpm)
        $refresh && dnf makecache --refresh -y
        if $build_deps; then
            dnf install -y --setopt=install_weak_deps=False rpm-build
            dnf builddep -y --setopt=install_weak_deps=False -D 'branch_truce_version 0.0.0' \
                -D 'branch_truce_native_version 0.0.0' "$packaging_dir/rpm/branch-truce.spec"
        fi
        exit 0 ;;
    arch-pkg)
        # Refresh the entire rolling base; never perform a partial upgrade.
        $refresh && pacman -Syu --noconfirm
        if $build_deps; then
            pacman -S --needed --noconfirm base-devel cmake ninja extra-cmake-modules git \
                qt6-base qt6-declarative qt6-svg qt6-wayland kirigami syntax-highlighting \
                qqc2-desktop-style breeze breeze-icons hicolor-icon-theme
        fi
        exit 0 ;;
esac

if $refresh || ! compgen -G "/var/lib/apt/lists/*_Packages*" > /dev/null; then
    apt-get update -q
fi

if $build_deps; then
    apt-get install -y -q --no-install-recommends build-essential dpkg-dev
    apt-get build-dep -y -q "$packaging_dir"
fi
