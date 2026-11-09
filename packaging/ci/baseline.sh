#!/usr/bin/env bash
# Prints the distribution, toolchain, Qt 6, KDE Frameworks 6 and Git versions
# of a CI build image and fails when one is older than the supported baseline
# in ../minimum-versions.tsv (packaging/minimum-versions.tsv).
#
#   baseline.sh --format kubuntu-deb|fedora-rpm|arch-pkg
#
# Every failure is reported with the version found. Exit status: 0 the image
# meets the baseline, 1 it does not, 2 usage error.
set -euo pipefail

usage() { echo "usage: $0 --format kubuntu-deb|fedora-rpm|arch-pkg"; }
format=''
while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --format) [ $# -ge 2 ] || { usage >&2; exit 2; }; format=$2; shift 2 ;;
        *) usage >&2; exit 2 ;;
    esac
done

# shellcheck disable=SC1091
. /etc/os-release
failures=0
fail() { echo "error: $*" >&2; failures=$((failures + 1)); }
found="${ID-} ${VERSION_ID-}"

case "$format" in
    kubuntu-deb)
        column=3
        [ "$found" = 'ubuntu 26.04' ] || fail "expected Ubuntu 26.04, found $found"
        toolchain() { dpkg-query -W g++ cmake extra-cmake-modules dpkg-dev debhelper; }
        # A library is resolved to the package that ships it.
        package_of() {
            case "$1" in
                *.so) dpkg -S "*/$1.*" 2>/dev/null | head -n 1 | cut -d: -f1 ;;
                *) printf '%s\n' "$1" ;;
            esac
        }
        installed() { dpkg-query -W -f '${Version}' "$1" 2>/dev/null; }
        at_least() {
            local upstream=${1#*:}
            dpkg --compare-versions "${upstream%%[-+~]*}" ge "$2"
        } ;;
    fedora-rpm)
        column=4
        [ "$found" = 'fedora 44' ] || fail "expected Fedora 44, found $found"
        toolchain() { rpm -q gcc-c++ cmake extra-cmake-modules qt6-qtdeclarative rpm rpm-build redhat-rpm-config; }
        package_of() { printf '%s\n' "$1"; }
        installed() { rpm -q --qf '%{VERSION}' "$1" 2>/dev/null; }
        at_least() { [ "$(OLD="$1" NEW="$2" rpm --eval '%{lua:print(rpm.vercmp(os.getenv("OLD"), os.getenv("NEW")))}')" != -1 ]; } ;;
    arch-pkg)
        column=5
        [ "${ID-}" = arch ] || fail "expected Arch Linux, found $found"
        toolchain() { pacman -Q gcc cmake extra-cmake-modules qt6-declarative pacman && makepkg --version; }
        package_of() { printf '%s\n' "$1"; }
        installed() { pacman -Q "$1" 2>/dev/null | awk '{ print $2 }'; }
        at_least() { [ "$(vercmp "$1" "$2")" -ge 0 ]; } ;;
    *) usage >&2; exit 2 ;;
esac

echo "os ${PRETTY_NAME:-$found}"
toolchain || fail 'the toolchain is incomplete'
while IFS=$'\t' read -r component minimum name; do
    package=$(package_of "$name") || package=''
    version=''
    [ -z "$package" ] || version=$(installed "$package") || version=''
    if [ -z "$version" ]; then
        fail "no installed package provides $component ($name)"
        continue
    fi
    echo "$component $package $version"
    at_least "$version" "$minimum" || fail "$component: $package $version is older than $minimum"
done < <(awk -F '\t' -v column="$column" '!/^#/ && NF { print $1 "\t" $2 "\t" $column }' \
    "$(dirname -- "${BASH_SOURCE[0]}")/../minimum-versions.tsv")
[ "$failures" -eq 0 ]
