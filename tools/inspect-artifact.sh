#!/usr/bin/env bash
# Inspects the runtime closure of a branch-truce DEB on a clean OS base, the
# checks packaging/scripts/check-package.sh does not make.
#
#   tools/inspect-artifact.sh --target <target> --package DEB [--output DIR]
#
# <target> is a row of packaging/validation-targets.tsv whose release format is
# a DEB, such as kubuntu or tuxedo. The script builds that target image, or
# reuses it from the engine's layer cache, and installs the DEB there with
# apt-get --no-install-recommends in a throwaway container, so the host's
# packages are never touched. The container fetches the target archive's
# package lists, so it needs network access. It then reports:
#
#   1. The runtime closure: every package the install added, with its version.
#   2. ELF closure: the RUNPATH and the highest required symbol versions, and
#      the package of each NEEDED library is named in Depends itself, not only
#      reached through another package.
#   3. QML closure: each module src/ui imports has a qmldir from a package
#      named in Depends.
#
# check-package.sh covers the payload, the metadata, the desktop files and
# icons, hardening, the private Qt ABI, runtime modules, the launch and the
# removal, on every release format.
#
# With --output, the report is also written to DIR/<package>.inspection.
# CONTAINER_ENGINE defaults to docker. Exit status: 0 every check passed, 1 a
# check failed, 2 usage error.

qml_root=/usr/lib/x86_64-linux-gnu/qt6/qml

usage() { echo "Usage: $0 --target <target> --package DEB [--output DIR]"; }

# ---------------------------------------------------------------- host side

host_main() {
    local root engine target package output image name status fixture
    root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
    source "$root/packaging/scripts/cli.sh"
    source "$root/packaging/scripts/formats.sh"
    source "$root/tools/lib/targets.sh"
    local -A options=()
    cli_parse options - 'target=! package=! output=' "$@" || { cli_usage $? usage; return; }
    target=${options[target]} package=${options[package]} output=${options[output]-}
    [[ -f "$package" ]] || { echo "error: $package is not a file" >&2; usage >&2; return 2; }
    deb_target "$target" || { usage >&2; return 2; }
    engine=${CONTAINER_ENGINE:-docker}
    package=$(cd -- "$(dirname -- "$package")" && pwd -P)/$(basename -- "$package")
    name=$(basename -- "$package" .deb)
    image=$(target_image "$engine" "$root" "$target" "${BRANCH_TRUCE_KDE_CI_REFRESH:-}") || return 1

    fixture=$(mktemp -d "${TMPDIR:-/tmp}/bt-inspect.XXXXXXXX")
    local cleanup
    printf -v cleanup 'rm -rf -- %q' "$fixture"
    # shellcheck disable=SC2064
    trap "$cleanup" EXIT
    # The container sees only this script and the QML modules the application
    # imports, from the sources it was built from.
    mkdir -- "$fixture/inspect"
    sed -n 's/^import \([A-Za-z][A-Za-z0-9.]*\).*/\1/p' "$root"/src/ui/*.qml | LC_ALL=C sort -u > "$fixture/inspect/imports.txt"
    cp -- "$root/tools/inspect-artifact.sh" "$fixture/inspect/"

    status=0
    "$engine" run --rm --volume "$package:/artifact/$name.deb:ro" --volume "$fixture/inspect:/inspect:ro" \
        "$image" /bin/bash /inspect/inspect-artifact.sh --in-container "/artifact/$name.deb" \
        > "$fixture/report" 2>&1 || status=$?
    cat -- "$fixture/report"
    if [[ -n "$output" ]]; then
        mkdir -p -- "$output"
        # A temporary name first, so an interrupted copy leaves no truncated report.
        cp -- "$fixture/report" "$output/.$name.inspection.partial"
        mv -- "$output/.$name.inspection.partial" "$output/$name.inspection"
        echo "Report: $(cd -- "$output" && pwd -P)/$name.inspection"
    fi
    echo "Inspection exit status: $status"
    return "$status"
}

# deb_target TARGET: TARGET is a validation target that takes a DEB.
deb_target() {
    local format family
    format=$(target_field "$1" format) && family=$(format_field "$format" family) || return 2
    [[ "$family" == deb ]] || { echo "error: $1 takes the $format package, not a DEB" >&2; return 2; }
}

# ------------------------------------------------------ inside the container

failures=0
step() { printf '\n== %s\n' "$*"; }
pass() { printf 'PASS %s\n' "$*"; }
fail() { printf 'FAIL %s\n' "$*"; failures=$((failures + 1)); }
installed_set() { dpkg-query -W -f='${db:Status-Status}\t${Package}\t${Version}\n' | awk -F '\t' '$1 == "installed" {print $2 "\t" $3}' | LC_ALL=C sort; }
# Package names in a Depends field, without versions or alternatives' order.
depends_names() { tr ',|' '\n\n' <<<"$1" | sed 's/(.*//; s/:any//; s/[[:space:]]//g' | sed '/^$/d' | LC_ALL=C sort -u; }
# owner_of PATH: the package that ships PATH, or nothing. A path no package
# ships is an answer, not an error, so the merged-/usr fallbacks can run.
owner_of() { { dpkg-query -S "$1" 2>/dev/null || true; } | head -1 | cut -d: -f1; }
# The symbol version tags in `objdump -T` output on stdin, sorted and unique.
# Output without any tag gives an empty list rather than grep's status 1.
symbol_tags() {
    { grep -oE '\b(GLIBC|GLIBCXX|CXXABI|GCC|Qt)_[0-9][0-9A-Z_.]*' || (($? == 1)); } | LC_ALL=C sort -u
}

# check_elf_closure BINARY DEPENDS: every NEEDED library of BINARY resolves and
# comes from a package DEPENDS names. dpkg may register a library under /lib or
# /usr/lib on merged-/usr systems, so each form is looked up.
check_elf_closure() {
    local binary=$1 names needed runpath lib path real owner before=$failures
    names=$(depends_names "$2")
    needed=$(readelf -d "$binary" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
    runpath=$(readelf -d "$binary" | sed -n 's/.*(\(RPATH\|RUNPATH\)).*\[\(.*\)\]/\2/p')
    echo "RUNPATH: ${runpath:-none}"
    for lib in $needed; do
        path=$(ldd "$binary" | awk -v lib="$lib" '$1 == lib {print $3}')
        [[ -n "$path" && "$path" != 'not' ]] || { fail "$lib does not resolve"; continue; }
        real=$(readlink -f "$path")
        owner=$(owner_of "$real")
        [[ -n "$owner" ]] || owner=$(owner_of "$path")
        [[ -n "$owner" ]] || owner=$(owner_of "${real/#\/usr\/lib\///lib/}")
        printf '%s\t%s\t%s\n' "$lib" "$path" "${owner:-no package}"
        grep -qx -- "$owner" <<<"$names" || fail "$lib comes from ${owner:-no package}, which Depends does not name"
    done
    if ((failures == before)); then pass "$(wc -w <<<"$needed") NEEDED libraries come from packages named in Depends"; fi
    echo "Highest required symbol versions:"
    local tags namespace
    tags=$(objdump -T "$binary" | symbol_tags)
    for namespace in GLIBC GLIBCXX CXXABI GCC; do
        grep "^${namespace}_[0-9.]*$" <<<"$tags" | sort -V | tail -1 | sed 's/^/  /' || true
    done
    echo "Qt symbol version tags: $(grep '^Qt_' <<<"$tags" | tr '\n' ' ' || true)"
}

# check_qml_closure IMPORTS DEPENDS: each module listed in the file IMPORTS has
# a qmldir from a package DEPENDS names.
check_qml_closure() {
    local imports=$1 names module dir owner before=$failures
    names=$(depends_names "$2")
    while IFS= read -r module; do
        dir=$qml_root/${module//.//}
        if [[ -f "$dir/qmldir" ]]; then
            owner=$(owner_of "$dir/qmldir")
            printf '%s\t%s\t%s\n' "$module" "$dir/qmldir" "${owner:-no package}"
            grep -qx -- "$owner" <<<"$names" || fail "QML $module comes from ${owner:-no package}, which Depends does not name"
        else
            fail "QML $module has no $dir/qmldir"
        fi
    done < "$imports"
    if ((failures == before)); then pass "$(wc -l < "$imports") imported QML modules come from packages named in Depends"; fi
}

container_main() {
    local deb=$1 binary=/usr/bin/branch-truce package version depends before after closure
    export DEBIAN_FRONTEND=noninteractive LC_ALL=C.UTF-8
    step "Package"
    package=$(dpkg-deb -f "$deb" Package) && version=$(dpkg-deb -f "$deb" Version) \
        && depends=$(dpkg-deb -f "$deb" Depends) || { fail "cannot read $deb"; return 1; }
    printf 'Package: %s\nVersion: %s\nDepends: %s\n' "$package" "$version" "$depends"

    step "Clean install of the runtime closure"
    before=$(installed_set)
    apt-get update -q >/dev/null
    apt-get install -y -q --no-install-recommends "$deb" >/tmp/install.log 2>&1 \
        || { cat /tmp/install.log; fail "apt install"; return 1; }
    after=$(installed_set)
    closure=$(LC_ALL=C comm -13 <(printf '%s\n' "$before") <(printf '%s\n' "$after"))
    printf '%s\n' "$closure"
    pass "apt installed $(wc -l <<<"$closure") packages, branch-truce included"
    # The inspection tools come after the closure is recorded; they are not part of it.
    apt-get install -y -q --no-install-recommends binutils >/dev/null 2>&1 \
        || { fail "inspection tools did not install"; return 1; }

    step "ELF closure of $binary"
    check_elf_closure "$binary" "$depends"
    step "QML closure"
    check_qml_closure /inspect/imports.txt "$depends"

    step "Result"
    if ((failures > 0)); then echo "FAILED: $failures checks"; return 1; fi
    echo "All inspection checks passed for $package $version"
}

# Sourcing the file defines its functions but runs neither half, and sets no
# shell options, so tests can call the functions on their own.
[[ "${BASH_SOURCE[0]}" == "$0" ]] || return 0
set -euo pipefail
if [[ "${1:-}" == --in-container ]]; then
    [[ $# -eq 2 ]] || { usage >&2; exit 2; }
    container_main "$2"
else
    host_main "$@"
fi
