#!/usr/bin/env bash
# Machine provisioning only. Never call from per-worktree setup.
set -euo pipefail
mode=${1:---check}
case "$mode" in --check|--dry-run|--install) ;; *) echo "Usage: $0 [--check|--dry-run|--install]" >&2; exit 2 ;; esac
. /etc/os-release
case "${ID:-}:${VERSION_ID:-}" in
    tuxedo:24.04|ubuntu:26.04) ;;
    *) echo 'Supported provisioning targets: Tuxedo OS 24.04 and Kubuntu 26.04.' >&2; exit 2 ;;
esac
# Tuxedo needs its vendor Qt/KDE repository enabled. Stock noble has Qt 6.4.
# Every runtime Depends of the package belongs here too (tests/packaging/ProvisioningDepends.sh).
# app.startup-appearance also exercises plasma-integration's KDE platform theme plugin.
packages=(g++ cmake make git extra-cmake-modules qt6-base-dev qt6-declarative-dev
    libkirigami-dev libkf6syntaxhighlighting-dev qml6-module-org-kde-kirigami qml6-module-org-kde-desktop
    qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-layouts qml6-module-qtquick-shapes
    qml6-module-qtquick-templates qml6-module-qtquick-window qml6-module-qtqml-models qml6-module-qtqml-workerscript
    kf6-breeze-icon-theme kde-style-breeze hicolor-icon-theme plasma-integration qt6-svg-plugins qt6-wayland libasan8 libubsan1)
missing=()
for package in "${packages[@]}"; do
    status=$(dpkg-query -W -f='${db:Status-Status}' "$package" 2>/dev/null || true)
    [[ "$status" == installed ]] || missing+=("$package")
done
if ((${#missing[@]} == 0)); then
    echo 'All provisioning packages are already installed; no changes.'
    exit 0
fi
printf 'Missing packages:'; printf ' %s' "${missing[@]}"; printf '\n'
case "$mode" in
    --check) exit 1 ;;
    --dry-run) apt-get --simulate install --no-install-recommends "${missing[@]}" ;;
    --install)
        if ((EUID != 0)); then echo 'Run --install as root after reviewing --dry-run.' >&2; exit 2; fi
        apt-get update
        apt-get install -y --no-install-recommends "${missing[@]}"
        ;;
esac
