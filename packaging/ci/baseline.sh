#!/usr/bin/env bash
# Prints the distribution, Qt 6, KDE Frameworks 6 and Git versions of a build
# environment and fails when they are older than the supported baseline.
#
#   baseline.sh --format kubuntu-deb|fedora-rpm|arch-pkg

set -euo pipefail

usage() {
    echo "usage: $0 --format kubuntu-deb|fedora-rpm|arch-pkg" >&2
    exit 2
}

format=""
while [ $# -gt 0 ]; do
    case "$1" in
        --format) format="${2-}"; shift 2 || usage ;;
        *) usage ;;
    esac
done

# shellcheck disable=SC1091
. /etc/os-release
failures=0
fail() { echo "error: $*" >&2; failures=$((failures + 1)); }

case "$format" in
    kubuntu-deb)
        [ "${ID-}" = ubuntu ] && [ "${VERSION_ID-}" = 26.04 ] \
            || fail "expected Ubuntu 26.04, found ${ID-} ${VERSION_ID-}"
        ;;
    fedora-rpm)
        [ "${ID-}" = fedora ] && [ "${VERSION_ID-}" = 44 ] || fail "expected Fedora 44"
        ;;
    arch-pkg)
        [ "${ID-}" = arch ] || fail "expected Arch Linux"
        ;;
    *) usage ;;
esac

if [ "$format" != kubuntu-deb ]; then
    echo "os ${PRETTY_NAME:-$ID}"
    if [ "$format" = fedora-rpm ]; then
        installed() { rpm -q --qf '%{VERSION}' "$1"; }
        at_least() { [ "$(OLD="$1" NEW="$2" rpm --eval '%{lua:print(rpm.vercmp(os.getenv("OLD"), os.getenv("NEW")))}')" != -1 ]; }
        packages="qt6-qtbase:6.6.0 kf6-kirigami:6.0.0 kf6-syntax-highlighting:6.0.0 git:2.43.0"
        rpm -q gcc-c++ cmake extra-cmake-modules qt6-qtdeclarative rpm rpm-build redhat-rpm-config
    else
        installed() { pacman -Q "$1" | awk '{print $2}'; }
        at_least() { [ "$(vercmp "$1" "$2")" -ge 0 ]; }
        packages="qt6-base:6.6.0 kirigami:6.0.0 syntax-highlighting:6.0.0 git:2.43.0"
        pacman -Q gcc cmake extra-cmake-modules qt6-declarative pacman
        makepkg --version
    fi
    for entry in $packages; do
        IFS=: read -r package minimum <<< "$entry"
        version="$(installed "$package")" || { fail "missing $package"; continue; }
        echo "library/tool $package $version"
        at_least "$version" "$minimum" || fail "$package $version is older than $minimum"
    done
    [ "$failures" -eq 0 ]
    exit
fi

dpkg-query -W g++ cmake extra-cmake-modules dpkg-dev debhelper
libraries="qt:libQt6Core.so:6.6.0 kf:libKirigami.so:6.0.0 kf:libKF6SyntaxHighlighting.so:6.0.0"

owner() { dpkg -S "*/$1.*" 2>/dev/null | head -n 1 | cut -d: -f1; }
installed() { dpkg-query -W -f '${Version}' "$1"; }
at_least() { dpkg --compare-versions "$1" ge "$2"; }

echo "os ${PRETTY_NAME:-${ID-} ${VERSION_ID-}}"
for entry in $libraries; do
    IFS=: read -r label library minimum <<<"$entry"
    package="$(owner "$library")" || package=""
    if [ -z "$package" ] || ! version="$(installed "$package" 2>/dev/null)" || [ -z "$version" ]; then
        fail "no installed package ships $library ($label)"
        continue
    fi
    echo "$label $package $version"
    upstream="${version#*:}"
    upstream="${upstream%%[-+~]*}"
    at_least "$upstream" "$minimum" || fail "$package $version is older than $minimum"
done

git_pkg_version="$(installed git 2>/dev/null || true)"
if [ -z "$git_pkg_version" ]; then
    fail "git is not installed"
else
    echo "git git $git_pkg_version"
    git_upstream="${git_pkg_version#*:}"
    git_upstream="${git_upstream%%[-+~]*}"
    at_least "$git_upstream" "2.43.0" || fail "git $git_pkg_version is older than 2.43.0"
fi

[ "$failures" -eq 0 ]
