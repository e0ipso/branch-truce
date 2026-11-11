#!/usr/bin/env bash
# Prints the native package version for a Branch Truce release version.
#
#   native-version.sh [--style tilde|plain] <version>
#
# Packages accept a subset of semantic versions, chosen so that dpkg and
# SemVer order every pair of them the same way:
#
#   MAJOR.MINOR.PATCH                     1.2.0       -> 1.2.0
#   MAJOR.MINOR.PATCH-<letters>           1.2.0-rc    -> 1.2.0~rc
#   MAJOR.MINOR.PATCH-<letters>.<number>  1.2.0-rc.1  -> 1.2.0~rc1
#
# <letters> is one or more ASCII letters and <number> has no leading zero and
# is not 0. "~" sorts a pre-release before its release in dpkg.
# The dot is dropped because dpkg sorts "." after letters, so "rc.1" would
# sort after "rcx". Other forms (a "-" or digits in the pre-release, more
# identifiers, build metadata) exit with status 2.
#
# The default tilde style is the DEB and RPM form. The plain style drops the
# "~" (1.2.0rc1), which Arch's vercmp also sorts before the release.
#
# The other packaging scripts call this; the CMake build accepts any semantic
# version, so a development build can use versions that cannot be packaged.

set -euo pipefail

usage() { echo "usage: $0 [--style tilde|plain] <version>"; }
style=tilde
case "${1-}" in
    -h|--help) usage; exit 0 ;;
    --style)
        case "${2-}" in tilde|plain) style=$2 ;; *) usage >&2; exit 2 ;; esac
        shift 2 ;;
esac
if [ $# -ne 1 ]; then
    usage >&2
    exit 2
fi

pattern='^((0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*))(-([A-Za-z]+)(\.([1-9][0-9]*))?)?$'
if ! [[ "$1" =~ $pattern ]]; then
    echo "error: \"$1\" cannot be packaged. Use MAJOR.MINOR.PATCH, optionally with" \
        "-<letters> or -<letters>.<number>, for example 1.2.0 or 1.2.0-rc.1." >&2
    exit 2
fi

core="${BASH_REMATCH[1]}"
letters="${BASH_REMATCH[6]}"
number="${BASH_REMATCH[8]}"
if [ -z "$letters" ]; then
    echo "$core"
elif [ "$style" = plain ]; then
    echo "$core$letters$number"
else
    echo "$core~$letters$number"
fi
