#!/usr/bin/env bash
# Native release identity. nativeVersion includes the native release revision.
# This helper requires Bash only, including inside build containers without Node.
set -euo pipefail
# Public outputs of package_info for Bash callers.
package_filename=''
package_native_version=''
package_architecture=''
package_info() {
    local format="$1" version="$2" native
    native="$("$(dirname "${BASH_SOURCE[0]}")/native-version.sh" "$version")" || return 2
    package_architecture=x86_64
    case "$format" in
        kubuntu-deb)
            package_architecture=amd64
            package_filename="branch-truce_${native}_amd64-ubuntu26.04.deb"
            package_native_version="$native" ;;
        fedora-rpm)
            package_filename="branch-truce-$native-1.fc44.x86_64.rpm"
            package_native_version="$native-1.fc44" ;;
        arch-pkg)
            native="${native/\~/}"
            package_filename="branch-truce-$native-1-x86_64.pkg.tar.zst"
            package_native_version="$native-1" ;;
        *) echo 'usage: package-info.sh --format kubuntu-deb|fedora-rpm|arch-pkg --version <semver>' >&2; return 2 ;;
    esac
}
package_info_main() {
    local format='' version=''
    while [ $# -gt 0 ]; do
        [ $# -ge 2 ] || return 2
        case "$1" in --format) format="$2" ;; --version) version="$2" ;; *) return 2 ;; esac
        shift 2
    done
    package_info "$format" "$version" || return "$?"
    # All values are constrained above; no arbitrary text needs JSON escaping.
    printf '{"format":"%s","version":"%s","nativeVersion":"%s","filename":"%s","architecture":"%s"}\n' \
        "$format" "$version" "$package_native_version" "$package_filename" "$package_architecture"
}
if [ "${BASH_SOURCE[0]}" = "$0" ]; then package_info_main "$@"; fi
