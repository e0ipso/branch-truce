#!/usr/bin/env bash
# Prints the OS base, toolchain and Qt/KDE packages of a build or target
# container. tools/build-artifact.sh and tools/inspect-artifact.sh run it
# inside the container and keep its output with the artifact evidence.
set -euo pipefail
. /etc/os-release
printf 'os\t%s\t%s\n' "$PRETTY_NAME" "${VERSION_CODENAME:-}"
if command -v g++ >/dev/null; then
    printf 'compiler\t%s\n' "$(g++ --version | head -1)"
fi
packages=(libc6 libstdc++6 g++ cmake debhelper dpkg-dev git extra-cmake-modules
    qt6-base-dev qt6-declarative-dev libkirigami-dev libkf6syntaxhighlighting-dev
    libqt6core6t64 libqt6gui6 libqt6gui6t64 libqt6qml6 libqt6quick6
    qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-layouts
    qml6-module-org-kde-kirigami qml6-module-org-kde-desktop kde-style-breeze
    kf6-breeze-icon-theme qt6-wayland libkf6syntaxhighlighting6
    libkf6syntaxhighlighting-data plasma-integration)
for package in "${packages[@]}"; do
    version=$(dpkg-query -W -f='${db:Status-Status} ${Version}' "$package" 2>/dev/null || true)
    case "$version" in
        installed\ *) printf 'package\t%s\t%s\n' "$package" "${version#installed }" ;;
        *)
            # Not installed: show the archive candidate, or nothing when the
            # base has no package of that name.
            version=$(apt-cache policy "$package" 2>/dev/null | sed -n 's/^  Candidate: //p')
            [[ -z "$version" || "$version" == '(none)' ]] \
                || printf 'package\t%s\tcandidate %s\n' "$package" "$version" ;;
    esac
done
