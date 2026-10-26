#!/usr/bin/env bash
# Prints the distribution, Qt 6, KDE Frameworks 6 and Git versions of a build
# environment and fails when they are older than the supported baseline.
#
#   baseline.sh --format kubuntu-deb|tuxedo-deb

set -euo pipefail

usage() {
    echo "usage: $0 --format kubuntu-deb|tuxedo-deb" >&2
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
    tuxedo-deb)
        { [ "${ID-}" = tuxedo ] || [ "${ID-}" = ubuntu ]; } && [ "${VERSION_ID-}" = 24.04 ] \
            || fail "expected Tuxedo/Ubuntu 24.04 (noble), found ${ID-} ${VERSION_ID-}"
        ;;
    *) usage ;;
esac

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
