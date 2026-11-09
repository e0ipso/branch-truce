#!/usr/bin/env bash
# Inspects a branch-truce native package, then installs, exercises, upgrades
# and removes it with the target's own package manager.
#
#   check-package.sh --format <id> --version <semver> --package <file> [--previous <file>]
#
# Run as root in a disposable target container of the format's OS, with its
# repositories configured and no source or build tree. Never run it on a
# desktop. The directory two levels up must hold packaging/ (formats.tsv,
# installed-files.txt, runtime-modules.txt, the recipe of the format and these
# scripts), tools/lib/fixture.sh and the branding pack's hicolor icons;
# packaging/scripts/container-check.sh assembles that tree.
#
# Every format runs the same checks. The package manager calls live in
# pkg-<family>.sh, which defines:
#   pkg_identity FILE           name|native version|architecture
#   pkg_check_metadata FILE     format-specific metadata, such as licenses
#   pkg_files FILE              payload files, absolute and sorted
#   pkg_extra_files             payload files beyond installed-files.txt
#   pkg_has_scripts FILE        true when the package has scripts, triggers
#                               or configuration files
#   pkg_depends FILE            declared dependency names
#   pkg_recipe_depends ROOT     dependency names the recipe declares
#   pkg_older OLD NEW           native version order
#   pkg_target_ready            the container is the format's target
#   pkg_refresh                 refresh the package lists
#   pkg_install FILE            install or upgrade from a local file
#   pkg_remove                  remove completely
#   pkg_installed_version       the installed native version, or nothing
#   pkg_verify_installed        the package manager's file integrity check
#   pkg_runtime_versions        print the target's Qt, KDE and Git packages
#   pkg_qt_root                 the Qt 6 directory holding qml/ and plugins/
#   pkg_leftover_suffixes       suffixes of saved configuration files
#
# The first failed check stops the run. Exit status: 0 every check passed,
# 1 a check failed, 2 usage error or unsupported version. Sourcing the file
# defines its functions and runs nothing, so tests can call them.

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)
source "$here/package-info.sh"
source "$root/tools/lib/fixture.sh"
app_binary=/usr/bin/branch-truce

usage() {
    echo "usage: $0 --format $(format_ids | paste -sd '|' -) --version <semver> --package <file> [--previous <file>]"
}
die() { echo "error: $*" >&2; exit 1; }
ok() { echo "ok: $*"; }
step() { printf '\n== %s\n' "$*"; }

# seed_user_home HOME: a private home with files the user already has. The
# installed binary runs with it, so the removal checks also cover what the
# application itself wrote there.
seed_user_home() {
    fixture_home "$1" && fixture_gitconfig "$1" || return 1
    mkdir -p "$1/data/Branch Truce/recovery/unrelated"
    printf '[General]\nwrapLines=true\n' > "$1/config/branch-trucerc"
    head -c 256 /dev/urandom > "$1/data/Branch Truce/recovery/unrelated/journal"
}

# run_app HOME DIRECTORY ARGUMENT...: the installed binary in DIRECTORY with
# only HOME's private environment, offscreen, with a deadline so a hang fails.
run_app() {
    local home=$1 directory=$2
    shift 2
    local -a environment
    mapfile -d '' environment < <(fixture_env "$home")
    (cd -- "$directory" && env -i "${environment[@]}" QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
        timeout --kill-after=10 60 "$app_binary" "$@")
}

# user_files HOME: the checksum of every file in HOME but its runtime directory.
user_files() { (cd -- "$1" && find . -path ./runtime -prune -o -type f -print0 | LC_ALL=C sort -z | xargs -0 -r sha256sum); }

# check_metadata FILE NATIVE ARCHITECTURE PAYLOAD: everything the package says
# about itself, before anything on the target changes. PAYLOAD lists the
# expected files, sorted.
check_metadata() {
    local file=$1 native=$2 architecture=$3 expected=$4 got reason declared dependency
    got=$(pkg_identity "$file") || die "cannot read the identity of $file"
    [ "$got" = "branch-truce|$native|$architecture" ] \
        || die "candidate identity $got differs from branch-truce|$native|$architecture"
    reason=$(pkg_check_metadata "$file" 2>&1) || die "candidate metadata: $reason"
    ok "candidate is branch-truce $native for $architecture"
    got=$(pkg_files "$file")
    [ "$got" = "$expected" ] || die "payload differs from the expected files:
$(diff <(printf '%s\n' "$expected") <(printf '%s\n' "$got") || true)"
    ok "payload is exactly the $(wc -l <<<"$expected") expected files"
    ! pkg_has_scripts "$file" || die 'the package has scripts, triggers or configuration files'
    ok 'no package scripts, triggers or configuration files'
    declared=$(pkg_depends "$file")
    while IFS= read -r dependency; do
        grep -qxF -- "$dependency" <<<"$declared" || die "the package does not declare the recipe dependency $dependency"
    done < <(pkg_recipe_depends "$root")
    ok 'the package declares every dependency of its recipe'
}

# check_previous FILE NATIVE ARCHITECTURE: prints the previous package's native
# version after checking that it is an older branch-truce for ARCHITECTURE.
check_previous() {
    local name version previous_architecture got
    got=$(pkg_identity "$1") || die "cannot read the identity of $1"
    IFS='|' read -r name version previous_architecture <<<"$got"
    [ "$name" = branch-truce ] && [ "$previous_architecture" = "$3" ] \
        || die "previous package is $name for $previous_architecture, not branch-truce for $3"
    pkg_older "$version" "$2" || die "previous version $version is not older than $2"
    printf '%s\n' "$version"
}

# check_installed WANT VERSION HOME WORK: the installed package and the
# application it installs, run on a fresh conflict in WORK/repo.
check_installed() {
    local want=$1 version=$2 home=$3 work=$4 path relative size got module kind qt_root
    local branding=$root/assets/branding/platform/linux/hicolor
    [ "$(pkg_installed_version)" = "$want" ] || die "installed version is '$(pkg_installed_version)', expected $want"
    while IFS= read -r path; do [ -f "$path" ] || die "missing installed file $path"; done < "$work/payload"
    pkg_verify_installed || die 'the package manager reports changed installed files'
    while IFS= read -r relative; do
        path=/usr/share/icons/hicolor/$relative
        cmp -s -- "$branding/$relative" "$path" || die "icon $relative differs from the branding pack"
        got=$(icon_lookup_ok /usr/share/icons/hicolor/index.theme "$relative") \
            || die "index.theme does not declare ${relative%/*} at the icon's size (${got:-not listed})"
        if [[ "$relative" != scalable/* ]]; then
            size=${relative%%/*}
            got=$(png_size "$path") || got='not a PNG'
            [ "$got" = "$size" ] || die "icon $relative is $got, expected $size"
        fi
    done < <(installed_files "$root/packaging/installed-files.txt" '' | sed -n 's|^/share/icons/hicolor/||p')
    desktop-file-validate /usr/share/applications/com.canpicasoft.branch-truce.desktop
    appstreamcli validate --no-net /usr/share/metainfo/com.canpicasoft.branch-truce.metainfo.xml
    ldd "$app_binary" > "$work/ldd" 2>&1 || die "ldd failed on $app_binary: $(cat "$work/ldd")"
    ! grep -q 'not found' "$work/ldd" || die "unresolved libraries: $(grep 'not found' "$work/ldd" | tr '\n' ' ')"
    # Write the tool output to files first, so a failing objdump or readelf fails the check.
    objdump -T "$app_binary" > "$work/symbols"
    readelf -hW "$app_binary" > "$work/header"
    readelf -lW "$app_binary" > "$work/segments"
    readelf -dW "$app_binary" > "$work/dynamic"
    ! grep -q Qt_6_PRIVATE_API "$work/symbols" || die 'the binary uses the private Qt ABI'
    grep -q 'Type:.*DYN' "$work/header" || die 'the binary is not PIE'
    grep -q GNU_RELRO "$work/segments" || die 'the binary lacks RELRO'
    grep -q BIND_NOW "$work/dynamic" || die 'the binary lacks immediate binding'
    grep GNU_STACK "$work/segments" | grep -q 'RW ' || die 'the binary has an executable or no stack policy'
    qt_root=$(pkg_qt_root)
    while IFS=$'\t' read -r kind module; do
        case "$kind" in
            qml) path=$qt_root/qml/${module//.//}/qmldir ;;
            plugin) path=$qt_root/plugins/$module ;;
            *) continue ;;
        esac
        [ -f "$path" ] || die "runtime $kind $module is not installed: no $path"
    done < "$root/packaging/runtime-modules.txt"
    got=$(run_app "$home" / --version) || die "installed --version exited $?"
    [ "$got" = "Branch Truce $version" ] || die "installed --version reports '$got'"
    fixture_merge_repo "$home" "$work/repo" tracked.txt
    got=$(run_app "$home" "$work/repo" --smoke-test "$work/repo") || die "installed --smoke-test exited $?: $got"
    smoke_record_ready "$got" || die "installed --smoke-test did not open the merge session: $got"
    ok "installed $want: files, integrity, icons, metadata, libraries, hardening, runtime modules, --version and --smoke-test"
}

# remove_and_check HOME PAYLOAD-FILE: removes the package completely; nothing
# it installed and nothing in HOME may remain changed.
remove_and_check() {
    local home=$1 payload=$2 before path suffix
    before=$(user_files "$home")
    step "remove $(pkg_installed_version)"
    pkg_remove
    [ -z "$(pkg_installed_version)" ] || die 'branch-truce is still installed'
    while IFS= read -r path; do
        [ ! -e "$path" ] && [ ! -L "$path" ] || die "removal left $path"
        for suffix in $(pkg_leftover_suffixes); do
            [ ! -e "$path$suffix" ] || die "removal left $path$suffix"
        done
    done < "$payload"
    while IFS= read -r path; do
        [ ! -e "${path%/*}" ] || die "removal left ${path%/*}"
    done < <(pkg_extra_files)
    [ "$(user_files "$home")" = "$before" ] || die 'removal changed files in the user home'
    ok 'complete removal; the user home is unchanged'
}

check_main() {
    local format='' version='' package='' previous='' family extension architecture native file work previous_native=''
    while [ $# -gt 0 ]; do
        case "$1" in
            -h|--help) usage; return 0 ;;
            --format|--version|--package|--previous)
                [ $# -ge 2 ] || { usage >&2; return 2; }
                case "$1" in
                    --format) format=$2 ;; --version) version=$2 ;;
                    --package) package=$2 ;; --previous) previous=$2 ;;
                esac
                shift 2 ;;
            *) usage >&2; return 2 ;;
        esac
    done
    [ -n "$format" ] && [ -n "$version" ] && [ -n "$package" ] || { usage >&2; return 2; }
    family=$(format_field "$format" family) || return 2
    extension=$(format_field "$format" extension)
    architecture=$(format_field "$format" architecture)
    native=$(package_native_version "$format" "$version") || return 2
    source "$here/pkg-$family.sh"
    for file in "$package" ${previous:+"$previous"}; do
        [ -f "$file" ] || { echo "error: $file is not a file" >&2; return 2; }
        [[ "$file" == *".$extension" ]] || { echo "error: $file is not a .$extension package" >&2; return 2; }
    done
    package=$(realpath -e -- "$package")
    [ -z "$previous" ] || previous=$(realpath -e -- "$previous")

    step "inspect $(basename "$package")"
    local expected
    expected=$({ installed_files "$root/packaging/installed-files.txt" /usr; pkg_extra_files; } | LC_ALL=C sort)
    check_metadata "$package" "$native" "$architecture" "$expected"
    if [ -n "$previous" ]; then
        previous_native=$(check_previous "$previous" "$native" "$architecture")
        ok "previous package branch-truce $previous_native is older than $native"
    fi

    [ "$(id -u)" -eq 0 ] || die 'run as root in a disposable target container'
    pkg_target_ready
    [ -z "$(pkg_installed_version)" ] || die 'branch-truce is already installed; use a clean target'
    work=$(mktemp -d /tmp/bt-package-check.XXXXXX)
    local cleanup
    printf -v cleanup 'rm -rf -- %q' "$work"
    # shellcheck disable=SC2064
    trap "$cleanup" EXIT
    printf '%s\n' "$expected" > "$work/payload"
    seed_user_home "$work/home"
    step 'refresh the package lists'
    pkg_refresh

    step "clean install of $native"
    pkg_install "$package"
    pkg_runtime_versions
    check_installed "$native" "$version" "$work/home" "$work"
    remove_and_check "$work/home" "$work/payload"
    if [ -n "$previous" ]; then
        step "install $previous_native, then upgrade to $native"
        pkg_install "$previous"
        [ "$(pkg_installed_version)" = "$previous_native" ] || die "the previous package installed '$(pkg_installed_version)'"
        pkg_install "$package"
        check_installed "$native" "$version" "$work/home" "$work"
        remove_and_check "$work/home" "$work/payload"
    fi
    echo
    echo "all checks passed for $(basename "$package")"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    set -euo pipefail
    check_main "$@"
fi
