#!/usr/bin/env bash
# Prints the distribution, toolchain, Qt 6, KDE Frameworks 6 and Git versions
# of a CI build image and fails when one is older than the supported baseline
# in ../minimum-versions.tsv (packaging/minimum-versions.tsv).
#
#   baseline.sh --format <id>
#
# The format's row in ../formats.tsv names the expected os and the package
# family; minimum-versions.tsv has one package column per family. The image
# holds both tables and ../scripts/{cli,tsv,formats}.sh at the same relative
# paths as packaging/ does. Every failure is reported with the version found.
# Exit status: 0 the image meets the baseline, 1 it does not, 2 usage error.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$here/../scripts/cli.sh"
source "$here/../scripts/formats.sh"

usage() { echo "usage: $0 --format $(format_ids | paste -sd '|' -)"; }
failures=0
fail() { echo "error: $*" >&2; failures=$((failures + 1)); }

# Per family: the toolchain report, the package that provides a minimum-versions
# entry, the installed version of a package and the version order.
deb_toolchain() { dpkg-query -W g++ cmake extra-cmake-modules dpkg-dev debhelper; }
# A library is resolved to the package that ships it, because its package name changes with the ABI.
deb_package_of() {
    case "$1" in
        *.so) { dpkg -S "*/$1.*" 2>/dev/null || true; } | head -n 1 | cut -d: -f1 ;;
        *) printf '%s\n' "$1" ;;
    esac
}
deb_installed() { dpkg-query -W -f '${Version}' "$1" 2>/dev/null; }
deb_at_least() {
    local upstream=${1#*:}
    dpkg --compare-versions "${upstream%%[-+~]*}" ge "$2"
}
rpm_toolchain() { rpm -q gcc-c++ cmake extra-cmake-modules qt6-qtdeclarative rpm rpm-build redhat-rpm-config; }
rpm_package_of() { printf '%s\n' "$1"; }
rpm_installed() { rpm -q --qf '%{VERSION}' "$1" 2>/dev/null; }
rpm_at_least() { [ "$(OLD="$1" NEW="$2" rpm --eval '%{lua:print(rpm.vercmp(os.getenv("OLD"), os.getenv("NEW")))}')" != -1 ]; }
arch_toolchain() { pacman -Q gcc cmake extra-cmake-modules qt6-declarative pacman && makepkg --version; }
arch_package_of() { printf '%s\n' "$1"; }
arch_installed() { { pacman -Q "$1" 2>/dev/null || true; } | awk '{ print $2 }'; }
arch_at_least() { [ "$(vercmp "$1" "$2")" -ge 0 ]; }

# check_component FAMILY COMPONENT MINIMUM NAME: prints the installed version of
# the package NAME names and records a failure when it is missing or older.
check_component() {
    local family=$1 component=$2 minimum=$3 name=$4 package version=''
    package=$("${family}_package_of" "$name") || package=''
    [ -z "$package" ] || version=$("${family}_installed" "$package") || version=''
    if [ -z "$version" ]; then
        fail "no installed package provides $component ($name)"
        return 0
    fi
    echo "$component $package $version"
    "${family}_at_least" "$version" "$minimum" || fail "$component: $package $version is older than $minimum"
}

baseline_main() {
    local -A options=()
    cli_parse options - 'format=!' "$@" || { cli_usage $? usage; return; }
    local format=${options[format]} family os found component minimum name
    family=$(format_field "$format" family) && os=$(format_field "$format" os) || { usage >&2; return 2; }
    # shellcheck disable=SC1091
    . /etc/os-release
    found="${ID-}${VERSION_ID:+ $VERSION_ID}"
    [ "$found" = "$os" ] || fail "expected $os, found $found"
    echo "os ${PRETTY_NAME:-$found}"
    "${family}_toolchain" || fail 'the toolchain is incomplete'
    while IFS=$'\t' read -r component minimum; do
        name=$(tsv_field "$here/../minimum-versions.tsv" "$component" "$family") || return 2
        check_component "$family" "$component" "$minimum" "$name"
    done < <(tsv_column "$here/../minimum-versions.tsv" minimum)
    [ "$failures" -eq 0 ] || return 1
}

baseline_main "$@"
