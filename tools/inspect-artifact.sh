#!/usr/bin/env bash
# Inspects a branch-truce .deb from tools/build-artifact.sh on its own OS base.
#
#   tools/inspect-artifact.sh --target kubuntu|tuxedo --package DEB [--output DIR]
#
# It runs in a throwaway container from branch-truce-artifact:<base>-target,
# the clean OS base that tools/build-artifact.sh leaves, so the host's packages
# are never touched. The container fetches the target archive's package lists,
# so it needs network access.
#
# Steps:
#   1. Package metadata and contents: name, version suffix for the base,
#      architecture, exactly the installed files listed in docs/build.md plus
#      the Debian copyright and changelog, and no maintainer scripts.
#   2. Clean install with apt and --no-install-recommends. The packages the
#      install adds are the runtime closure; they are printed with versions.
#   3. ELF closure: NEEDED libraries, RUNPATH, highest required symbol
#      versions, every library resolves, the package of each NEEDED
#      library is in Depends, and Qt_6_PRIVATE_API symbols, if any, come with
#      an exact Qt private ABI pin in Depends.
#   4. QML closure: each module src/ui imports has a qmldir whose package is
#      in Depends.
#   5. desktop-file-validate and appstreamcli validate --no-net.
#   6. The installed branch-truce runs --version and --smoke-test on a
#      disposable merge conflict, offscreen. This loads the embedded QML,
#      Kirigami and Qt Quick Controls from the installed closure.
#   7. The installed application resolves the Breeze icon document-save, read
#      by tests/fixtures/AppearanceProbe.cpp preloaded into the real binary.
#      Without the SVG icon engine in the closure the icon is null. The run
#      names no desktop, so it also covers the app naming its own icon theme.
#   8. Removal leaves none of the package's files.
# binutils, desktop-file-utils and appstream are installed after step 2 for
# steps 3 and 5 only; they are not part of the closure. The probe is built
# from the sources in the matching -build image, which must exist.
#
# With --output, the report is also written to DIR/<package>.inspection.
# CONTAINER_ENGINE defaults to docker.
set -euo pipefail

app_id=local.branch_truce.BranchTruce
qml_root=/usr/lib/x86_64-linux-gnu/qt6/qml

usage() { echo "Usage: $0 --target kubuntu|tuxedo --package DEB [--output DIR]" >&2; exit 2; }

base_for() {
    case "$1" in
        kubuntu) echo kubuntu26.04 ;;
        tuxedo) echo tuxedo24.04 ;;
        *) usage ;;
    esac
}

host_main() {
    local target= package= output=
    while (($# > 0)); do
        case "$1" in
            --target) [[ $# -ge 2 ]] || usage; target=$2; shift 2 ;;
            --package) [[ $# -ge 2 ]] || usage; package=$2; shift 2 ;;
            --output) [[ $# -ge 2 ]] || usage; output=$2; shift 2 ;;
            *) usage ;;
        esac
    done
    [[ -n "$target" && -f "$package" ]] || usage
    local root engine base image name status
    root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
    engine=${CONTAINER_ENGINE:-docker}
    base=$(base_for "$target")
    image=branch-truce-artifact:${target}-${base#"$target"}-target
    package=$(cd -- "$(dirname -- "$package")" && pwd -P)/$(basename -- "$package")
    name=$(basename -- "$package" .deb)
    "$engine" image inspect "$image" >/dev/null 2>&1 \
        || { echo "Missing image $image; run tools/build-artifact.sh --target $target first" >&2; exit 1; }

    fixture=$(mktemp -d "${TMPDIR:-/tmp}/bt-inspect.XXXXXXXX")
    trap 'rm -rf -- "$fixture"' EXIT
    # The QML modules the application imports, from the sources it was built from.
    sed -n 's/^import \([A-Za-z][A-Za-z0-9.]*\).*/\1/p' "$root"/src/ui/*.qml | LC_ALL=C sort -u > "$fixture/imports.txt"
    cp -- "$root/tools/inspect-artifact.sh" "$root/tools/artifact-baseline.sh" "$fixture/"
    build_appearance_probe "$engine" "${image%-target}-build" "$root" "$fixture/probe" \
        || { echo "Could not build the appearance probe in ${image%-target}-build; run tools/build-artifact.sh --target $target first" >&2; exit 1; }

    status=0
    "$engine" run --rm --volume "$package:/artifact/$name.deb:ro" --volume "$fixture:/inspect:ro" \
        "$image" /bin/bash /inspect/inspect-artifact.sh --in-container "$base" "/artifact/$name.deb" \
        > "$fixture/report" 2>&1 || status=$?
    cat -- "$fixture/report"
    if [[ -n "$output" ]]; then
        mkdir -p -- "$output"
        cp -- "$fixture/report" "$output/$name.inspection"
        echo "Report: $(cd -- "$output" && pwd -P)/$name.inspection"
    fi
    echo "Inspection exit status: $status"
    return "$status"
}

# Builds tests/fixtures/AppearanceProbe.cpp into DIR/lib/libappearance_probe.so
# in the -build image of the same base, so it links the Qt the package runs on.
# The container has no network and sees the source read-only.
build_appearance_probe() {
    local engine=$1 image=$2 root=$3 dir=$4 log
    mkdir -p -- "$dir/src" "$dir/lib"
    cp -- "$root/tests/fixtures/AppearanceProbe.cpp" "$dir/src/"
    cat > "$dir/src/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.22)
project(appearance_probe CXX)
find_package(Qt6 REQUIRED COMPONENTS Gui QuickControls2)
add_library(appearance_probe MODULE AppearanceProbe.cpp)
target_link_libraries(appearance_probe PRIVATE Qt6::Gui Qt6::QuickControls2)
CMAKE
    log=$dir/build.log
    "$engine" run --rm --network none \
        --volume "$dir/src:/probe/src:ro" --volume "$dir/lib:/probe/lib" "$image" /bin/bash -c \
        'cmake -S /probe/src -B /tmp/probe-build && cmake --build /tmp/probe-build \
            && cp /tmp/probe-build/libappearance_probe.so /probe/lib/' \
        > "$log" 2>&1 || { tail -n 20 -- "$log" >&2; return 1; }
    [[ -f "$dir/lib/libappearance_probe.so" ]]
}

# The icon decision: the appearance record the preloaded probe printed must say
# the Breeze icon document-save resolved to an icon. An unset record fails too.
check_icon_resolves() {
    local record=$1
    if [[ -z "$record" ]]; then
        fail "no BRANCH_TRUCE_APPEARANCE record from the installed application"
    elif [[ "$record" == *'"documentSave":true'* ]]; then
        pass "QIcon::fromTheme(document-save) resolves in the installed application"
    else
        fail "QIcon::fromTheme(document-save) is null in the installed application; does Depends name the Qt SVG icon engine package?"
    fi
}

# Everything below runs as root inside the disposable target container.
failures=0
step() { printf '\n== %s\n' "$*"; }
pass() { printf 'PASS %s\n' "$*"; }
fail() { printf 'FAIL %s\n' "$*"; failures=$((failures + 1)); }
installed_set() { dpkg-query -W -f='${db:Status-Status}\t${Package}\t${Version}\n' | awk -F '\t' '$1 == "installed" {print $2 "\t" $3}' | LC_ALL=C sort; }
# Package names in a Depends field, without versions or alternatives' order.
depends_names() { tr ',|' '\n\n' <<<"$1" | sed 's/(.*//; s/:any//; s/[[:space:]]//g' | sed '/^$/d' | LC_ALL=C sort -u; }
owner_of() { dpkg-query -S "$1" 2>/dev/null | head -1 | cut -d: -f1; }
# The symbol version tags in `objdump -T` output on stdin, sorted and unique.
# Output without any tag gives an empty list rather than grep's status 1.
symbol_tags() {
    { grep -oE '\b(GLIBC|GLIBCXX|CXXABI|GCC|Qt)_[0-9][0-9A-Z_.]*' || (($? == 1)); } | LC_ALL=C sort -u
}
# Qt's private API is only stable within one Qt release, so a binary that
# uses it must refuse any other release through an exact Depends pin.
check_private_abi() {
    local tags=$1 depends=$2
    if grep -qx 'Qt_6_PRIVATE_API' <<<"$tags"; then
        if grep -qE 'qt6-[a-z]+-private-abi \(= [^)]+\)' <<<"$depends"; then
            pass "Qt_6_PRIVATE_API symbols with an exact Qt private ABI pin in Depends"
        else
            fail "Qt_6_PRIVATE_API symbols without an exact Qt private ABI pin in Depends"
        fi
    else
        pass "no Qt_6_PRIVATE_API symbols"
    fi
}

container_main() {
    local base=$1 deb=$2 binary=/usr/bin/branch-truce
    export DEBIAN_FRONTEND=noninteractive LC_ALL=C.UTF-8

    step "Package metadata"
    local package version arch depends recommends
    package=$(dpkg-deb -f "$deb" Package)
    version=$(dpkg-deb -f "$deb" Version)
    arch=$(dpkg-deb -f "$deb" Architecture)
    depends=$(dpkg-deb -f "$deb" Depends)
    recommends=$(dpkg-deb -f "$deb" Recommends)
    printf 'Package: %s\nVersion: %s\nArchitecture: %s\nDepends: %s\nRecommends: %s\n' \
        "$package" "$version" "$arch" "$depends" "$recommends"
    [[ "$package" == branch-truce ]] && pass "package name" || fail "package name $package"
    [[ "$version" == *"+$base" ]] && pass "version $version names $base" || fail "version $version lacks +$base"
    [[ "$arch" == amd64 ]] && pass "architecture amd64" || fail "architecture $arch"

    step "Package contents"
    local files expected controls
    files=$(dpkg-deb --fsys-tarfile "$deb" | tar -t | grep -v '/$' | LC_ALL=C sort)
    expected=$(printf '%s\n' ./usr/bin/branch-truce \
        "./usr/share/applications/$app_id.desktop" \
        "./usr/share/icons/hicolor/scalable/apps/$app_id.svg" \
        "./usr/share/metainfo/$app_id.metainfo.xml" \
        ./usr/share/doc/branch-truce/changelog.gz ./usr/share/doc/branch-truce/copyright | LC_ALL=C sort)
    printf '%s\n' "$files"
    [[ "$files" == "$expected" ]] && pass "exactly $(wc -l <<<"$expected") files" || fail "file list differs"
    controls=$(dpkg-deb --ctrl-tarfile "$deb" | tar -t | grep -v '/$' | LC_ALL=C sort | tr '\n' ' ')
    echo "Control members: $controls"
    [[ "$controls" == './control ./md5sums ' ]] && pass "no maintainer scripts" || fail "unexpected control members"

    step "Clean install of the runtime closure"
    local before after closure
    before=$(installed_set)
    apt-get update -q >/dev/null
    apt-get install -y -q --no-install-recommends "$deb" >/tmp/install.log 2>&1 \
        || { cat /tmp/install.log; fail "apt install"; return 1; }
    after=$(installed_set)
    closure=$(LC_ALL=C comm -13 <(printf '%s\n' "$before") <(printf '%s\n' "$after"))
    printf '%s\n' "$closure"
    pass "apt installed $(wc -l <<<"$closure") packages, branch-truce included"
    printf '%s\n' "$closure" | grep -qx "branch-truce"$'\t'"$version" || fail "branch-truce $version not installed"

    step "Target runtime baseline"
    /bin/bash /inspect/artifact-baseline.sh

    apt-get install -y -q --no-install-recommends binutils desktop-file-utils appstream >/dev/null 2>&1 \
        || { fail "inspection tools did not install"; return 1; }

    step "ELF closure of $binary"
    local needed runpath lib path real owner names before_libs
    names=$(depends_names "$depends")
    needed=$(readelf -d "$binary" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
    runpath=$(readelf -d "$binary" | sed -n 's/.*(\(RPATH\|RUNPATH\)).*\[\(.*\)\]/\2/p')
    echo "RUNPATH: ${runpath:-none}"
    if ldd "$binary" | grep -q 'not found'; then
        ldd "$binary"; fail "unresolved libraries"
    else
        pass "all $(ldd "$binary" | grep -c '=>') linked libraries resolve"
    fi
    before_libs=$failures
    for lib in $needed; do
        path=$(ldd "$binary" | awk -v lib="$lib" '$1 == lib {print $3}')
        real=$(readlink -f "$path")
        # dpkg may register a library under /lib or /usr/lib on merged-/usr systems.
        owner=$(owner_of "$real")
        [[ -n "$owner" ]] || owner=$(owner_of "$path")
        [[ -n "$owner" ]] || owner=$(owner_of "${real/#\/usr\/lib\///lib/}")
        printf '%s\t%s\t%s\n' "$lib" "$path" "$owner"
        grep -qx -- "$owner" <<<"$names" || fail "$lib comes from $owner, which Depends does not name"
    done
    ((failures == before_libs)) && pass "$(wc -w <<<"$needed") NEEDED libraries come from packages named in Depends"
    echo "Highest required symbol versions:"
    local tags namespace
    tags=$(objdump -T "$binary" | symbol_tags)
    for namespace in GLIBC GLIBCXX CXXABI GCC; do
        grep "^${namespace}_[0-9.]*$" <<<"$tags" | sort -V | tail -1 | sed 's/^/  /' || true
    done
    echo "Qt symbol version tags: $(grep '^Qt_' <<<"$tags" | tr '\n' ' ')"
    check_private_abi "$tags" "$depends"

    step "QML closure"
    local module dir before_qml=$failures
    while IFS= read -r module; do
        dir=$qml_root/${module//.//}
        if [[ -f "$dir/qmldir" ]]; then
            owner=$(owner_of "$dir/qmldir")
            printf '%s\t%s\t%s\n' "$module" "$dir/qmldir" "$owner"
            grep -qx -- "$owner" <<<"$names" || fail "QML $module comes from $owner, which Depends does not name"
        else
            fail "QML $module has no $dir/qmldir"
        fi
    done < /inspect/imports.txt
    ((failures == before_qml)) && pass "$(wc -l < /inspect/imports.txt) imported QML modules come from packages named in Depends"

    step "Desktop metadata"
    desktop-file-validate "/usr/share/applications/$app_id.desktop" && pass "desktop entry" || fail "desktop entry"
    appstreamcli validate --no-net --override=url-homepage-missing=info \
        "/usr/share/metainfo/$app_id.metainfo.xml" && pass "metainfo" || fail "metainfo"

    step "Installed application on a disposable merge conflict"
    local home=/tmp/bt-home repo=/tmp/bt-repo smoke record
    mkdir -p "$home/data" "$home/config" "$home/cache" "$home/tmp" "$home/runtime" "$repo"
    chmod 700 "$home/runtime"
    local environment=(
        "PATH=/usr/bin:/bin" "HOME=$home" "TMPDIR=$home/tmp" "LC_ALL=C.UTF-8"
        "XDG_DATA_HOME=$home/data" "XDG_CONFIG_HOME=$home/config" "XDG_CACHE_HOME=$home/cache"
        "XDG_RUNTIME_DIR=$home/runtime" "GIT_CONFIG_NOSYSTEM=1" "GIT_CONFIG_GLOBAL=/dev/null"
        "GIT_TERMINAL_PROMPT=0" "QT_QPA_PLATFORM=offscreen" "QT_QUICK_BACKEND=software"
    )
    isolated() { (cd "$repo" && env -i "${environment[@]}" "$@"); }
    isolated git init --quiet --initial-branch=main
    isolated git config user.name 'Branch Truce Artifact Check'
    isolated git config user.email 'artifact@example.invalid'
    printf 'base\n' > "$repo/tracked.txt"
    isolated git add tracked.txt && isolated git commit --quiet -m base
    isolated git checkout --quiet -b side
    printf 'side\n' > "$repo/tracked.txt"
    isolated git commit --quiet -am side && isolated git checkout --quiet main
    printf 'main\n' > "$repo/tracked.txt"
    isolated git commit --quiet -am main
    if isolated git merge --no-edit side >/dev/null 2>&1; then fail "fixture merge did not conflict"; fi
    echo "git: $(isolated git --version)"
    isolated branch-truce --version && pass "branch-truce --version" || fail "branch-truce --version"
    smoke=$(isolated branch-truce --smoke-test "$repo" 2>&1) || fail "smoke test exited $?"
    printf '%s\n' "$smoke"
    record=$(printf '%s\n' "$smoke" | sed -n 's/^BRANCH_TRUCE_SESSION=//p')
    if [[ "$record" == *'"state":"ready"'* && "$record" == *'"operation":"Merge"'* \
        && "$record" == *'"path":"tracked.txt"'* ]]; then
        pass "installed window loaded and opened the merge session"
    else
        fail "smoke test did not open the merge session"
    fi

    step "Icon theme in the installed application"
    local appearance
    # The isolated environment names no desktop, as on a bare session. The
    # app has to name its own icon theme there, or Kirigami swaps in one that
    # does not resolve and the icon is null.
    appearance=$(isolated env LD_PRELOAD=/inspect/probe/lib/libappearance_probe.so \
        branch-truce --smoke-test "$repo" 2>&1) || fail "smoke test with the probe exited $?"
    appearance=$(printf '%s\n' "$appearance" | sed -n 's/^BRANCH_TRUCE_APPEARANCE=//p')
    echo "Appearance: ${appearance:-none}"
    echo "SVG icon engine: $(find /usr/lib -path '*/qt6/plugins/iconengines/*svg*' | head -1)"
    check_icon_resolves "$appearance"

    step "Removal"
    apt-get remove -y -q branch-truce >/dev/null 2>&1 || fail "apt remove"
    local left=0 file
    while IFS= read -r file; do
        [[ "$file" == ./usr/share/doc/* || ! -e "${file#.}" ]] || { echo "left: ${file#.}"; left=$((left + 1)); }
    done <<<"$files"
    ((left == 0)) && pass "no installed program files remain" || fail "$left files remain"

    step "Result"
    if ((failures > 0)); then echo "FAILED: $failures checks"; return 1; fi
    echo "All inspection checks passed for $package $version"
}

# Sourcing the file defines its functions and sets the shell options above but
# runs neither half, so tests can call the functions on their own.
[[ "${BASH_SOURCE[0]}" == "$0" ]] || return 0
if [[ "${1:-}" == --in-container ]]; then
    [[ $# -eq 3 ]] || usage
    container_main "$2" "$3"
else
    host_main "$@"
fi
