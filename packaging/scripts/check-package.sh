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
# scripts), tools/lib/fixture.sh, tools/lib/report.sh and the branding pack's
# hicolor icons;
# packaging/scripts/container-check.sh assembles that tree.
#
# Every format runs the same checks. The package manager calls live in
# pkg-<family>.sh, whose contract pkg-adapters.md documents.
#
# The first failed check stops the run. Exit status: 0 every check passed,
# 1 a check failed, 2 usage error or unsupported version. Sourcing the file
# defines its functions and runs nothing, so tests can call them.

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)
source "$here/cli.sh"
source "$here/package-info.sh"
source "$root/tools/lib/fixture.sh"
source "$root/tools/lib/report.sh"
app_binary=/usr/bin/branch-truce

usage() {
    echo "usage: $0 --format $(format_ids | paste -sd '|' -) --version <semver> --package <file> [--previous <file>]"
}

# seed_user_home ROOT HOME: a private home with files the user already has. The
# installed binary runs with it, so the removal checks also cover what the
# application itself wrote there.
seed_user_home() {
    fixture_home "$1" "$2" && fixture_gitconfig "$1" "$2" || return 1
    mkdir -p "$2/data/Branch Truce/recovery/unrelated"
    printf '[General]\nwrapLines=true\n' > "$2/config/branch-trucerc"
    head -c 256 /dev/urandom > "$2/data/Branch Truce/recovery/unrelated/journal"
}

# run_app HOME DIRECTORY ARGUMENT...: the installed binary in DIRECTORY with
# only HOME's private environment, offscreen, with a deadline so a hang fails.
run_app() {
    fixture_run "$1" "$2" QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
        timeout --kill-after=10 60 "$app_binary" "${@:3}"
}

# user_files HOME: the checksum of every file in HOME but its runtime directory.
user_files() { (cd -- "$1" && find . -path ./runtime -prune -o -type f -print0 | LC_ALL=C sort -z | xargs -0 -r sha256sum); }

# check_metadata FILE NATIVE ARCHITECTURE PAYLOAD: everything the package says
# about itself, before anything on the target changes. PAYLOAD lists the
# expected files, sorted.
check_metadata() {
    local file=$1 native=$2 architecture=$3 expected=$4 got reason declared dependency status=0 recipe
    got=$(pkg_identity "$file") || die "cannot read the identity of $file"
    [ "$got" = "branch-truce|$native|$architecture" ] \
        || die "candidate identity $got differs from branch-truce|$native|$architecture"
    reason=$(pkg_check_metadata "$file" 2>&1) || die "candidate metadata: $reason"
    pass "candidate is branch-truce $native for $architecture"
    got=$(pkg_files "$file") || die "cannot read payload of $file"
    [ "$got" = "$expected" ] || die "payload differs from the expected files:
$(diff <(printf '%s\n' "$expected") <(printf '%s\n' "$got") || true)"
    pass "payload is exactly the $(wc -l <<<"$expected") expected files"
    pkg_has_scripts "$file" || status=$?
    case "$status" in
        0) die 'the package has scripts, triggers or configuration files' ;;
        1) ;;
        *) die "cannot inspect package scripts in $file (exit $status)" ;;
    esac
    pass 'no package scripts, triggers or configuration files'
    declared=$(pkg_depends "$file") || die "cannot read dependencies of $file"
    recipe=$(pkg_recipe_depends "$root") || die "cannot read recipe dependencies"
    while IFS= read -r dependency; do
        grep -qxF -- "$dependency" <<<"$declared" || die "the package does not declare the recipe dependency $dependency"
    done <<<"$recipe"
    pass 'the package declares every dependency of its recipe'
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

# check_installed_files WANT PAYLOAD-FILE: the package manager reports
# version WANT installed, every payload file exists and none changed.
check_installed_files() {
    local want=$1 payload=$2 path
    [ "$(pkg_installed_version)" = "$want" ] || die "installed version is '$(pkg_installed_version)', expected $want"
    while IFS= read -r path; do [ -f "$path" ] || die "missing installed file $path"; done < "$payload"
    pkg_verify_installed || die 'the package manager reports changed installed files'
}

# check_icons: each installed icon is the branding pack's file, its directory
# is declared at its size in hicolor's index.theme, and a PNG has that size.
check_icons() {
    local branding=$root/assets/branding/platform/linux/hicolor relative path size got icons
    icons=$(installed_files "$root/packaging/installed-files.txt" '' | sed -n 's|^/share/icons/hicolor/||p') || die "cannot read icon list"
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
    done <<<"$icons"
}

# check_desktop_files: the desktop entry and the AppStream metainfo validate.
check_desktop_files() {
    desktop-file-validate /usr/share/applications/com.canpicasoft.branch-truce.desktop \
        || die 'the desktop entry does not validate'
    appstreamcli validate --no-net /usr/share/metainfo/com.canpicasoft.branch-truce.metainfo.xml \
        || die 'the AppStream metainfo does not validate'
}

# check_binary WORK: every linked library resolves, the binary uses no private
# Qt ABI and has the hardening the distributions build with. Tool output goes
# to files in WORK first, so a failing objdump or readelf fails the check.
check_binary() {
    local work=$1
    ldd "$app_binary" > "$work/ldd" 2>&1 || die "ldd failed on $app_binary: $(cat "$work/ldd")"
    ! grep -q 'not found' "$work/ldd" || die "unresolved libraries: $(grep 'not found' "$work/ldd" | tr '\n' ' ')"
    { objdump -T "$app_binary" > "$work/symbols" && readelf -hW "$app_binary" > "$work/header" \
        && readelf -lW "$app_binary" > "$work/segments" && readelf -dW "$app_binary" > "$work/dynamic"; } \
        || die "cannot read the ELF headers of $app_binary"
    ! grep -q Qt_6_PRIVATE_API "$work/symbols" || die 'the binary uses the private Qt ABI'
    grep -q 'Type:.*DYN' "$work/header" || die 'the binary is not PIE'
    grep -q GNU_RELRO "$work/segments" || die 'the binary lacks RELRO'
    grep -q BIND_NOW "$work/dynamic" || die 'the binary lacks immediate binding'
    grep GNU_STACK "$work/segments" | grep -q 'RW ' || die 'the binary has an executable or no stack policy'
}

# check_runtime_modules QT-ROOT: every QML module and Qt plugin of
# runtime-modules.txt is installed under QT-ROOT.
check_runtime_modules() {
    local qt_root=$1 kind module path
    while IFS=$'\t' read -r kind module; do
        case "$kind" in
            qml) path=$qt_root/qml/${module//.//}/qmldir ;;
            plugin) path=$qt_root/plugins/$module ;;
            *) continue ;;
        esac
        [ -f "$path" ] || die "runtime $kind $module is not installed: no $path"
    done < "$root/packaging/runtime-modules.txt"
}

# check_launch VERSION HOME WORK: the installed binary reports VERSION and opens
# a fresh conflict in WORK/repo with --smoke-test, with HOME's environment.
check_launch() {
    local version=$1 home=$2 work=$3 got
    got=$(run_app "$home" / --version) || die "installed --version exited $?"
    [ "$got" = "Branch Truce $version" ] || die "installed --version reports '$got'"
    fixture_merge_repo "$work" "$home" "$work/repo" tracked.txt || die 'the fixture merge did not conflict'
    got=$(run_app "$home" "$work/repo" --smoke-test "$work/repo") || die "installed --smoke-test exited $?: $got"
    smoke_record_ready "$got" || die "installed --smoke-test did not open the merge session: $got"
}

# check_installed WANT VERSION HOME WORK: the installed package and the
# application it installs, run on a fresh conflict in WORK/repo.
check_installed() {
    local want=$1 version=$2 home=$3 work=$4
    check_installed_files "$want" "$work/payload"
    check_icons
    check_desktop_files
    check_binary "$work"
    check_runtime_modules "$(pkg_qt_root)"
    check_launch "$version" "$home" "$work"
    pass "installed $want: files, integrity, icons, metadata, libraries, hardening, runtime modules, --version and --smoke-test"
}

# remove_and_check HOME PAYLOAD-FILE: removes the package completely; nothing
# it installed and nothing in HOME may remain changed.
remove_and_check() {
    local home=$1 payload=$2 before path suffix extras
    before=$(user_files "$home")
    step "remove $(pkg_installed_version)"
    pkg_remove || die 'the package manager did not remove branch-truce'
    [ -z "$(pkg_installed_version)" ] || die 'branch-truce is still installed'
    while IFS= read -r path; do
        [ ! -e "$path" ] && [ ! -L "$path" ] || die "removal left $path"
        for suffix in $(pkg_leftover_suffixes); do
            [ ! -e "$path$suffix" ] || die "removal left $path$suffix"
        done
    done < "$payload"
    extras=$(pkg_extra_files) || die "cannot read extra package files"
    while IFS= read -r path; do
        [ -n "$path" ] || continue
        [ ! -e "${path%/*}" ] || die "removal left ${path%/*}"
    done <<<"$extras"
    [ "$(user_files "$home")" = "$before" ] || die 'removal changed files in the user home'
    pass 'complete removal; the user home is unchanged'
}

check_main() {
    local format version package previous family extension architecture native file work previous_native=''
    local -A options=()
    cli_parse options - 'format=! version=! package=! previous=' "$@" || { cli_usage $? usage; return; }
    format=${options[format]} version=${options[version]} package=${options[package]} previous=${options[previous]-}
    family=$(format_field "$format" family) || return 2
    extension=$(format_field "$format" extension)
    architecture=$(format_field "$format" architecture)
    native=$(package_native_version "$format" "$version") || return 2
    # shellcheck source=pkg-deb.sh
    source "$here/pkg-$family.sh"
    for file in "$package" ${previous:+"$previous"}; do
        [ -f "$file" ] || { echo "error: $file is not a file" >&2; return 2; }
        [[ "$file" == *".$extension" ]] || { echo "error: $file is not a .$extension package" >&2; return 2; }
    done
    package=$(realpath -e -- "$package")
    [ -z "$previous" ] || previous=$(realpath -e -- "$previous")

    step "inspect $(basename "$package")"
    local expected extras
    expected=$(installed_files "$root/packaging/installed-files.txt" /usr) || die 'cannot read expected payload'
    extras=$(pkg_extra_files) || die 'cannot read extra package files'
    expected=$(printf '%s\n' "$expected" "$extras" | sed '/^$/d' | LC_ALL=C sort) || die 'cannot sort expected payload'
    check_metadata "$package" "$native" "$architecture" "$expected"
    if [ -n "$previous" ]; then
        previous_native=$(check_previous "$previous" "$native" "$architecture")
        pass "previous package branch-truce $previous_native is older than $native"
    fi

    [ "$(id -u)" -eq 0 ] || die 'run as root in a disposable target container'
    pkg_target_ready
    [ -z "$(pkg_installed_version)" ] || die 'branch-truce is already installed; use a clean target'
    work=$(fixture_root)
    local cleanup
    printf -v cleanup 'rm -rf -- %q' "$work"
    # shellcheck disable=SC2064
    trap "$cleanup" EXIT
    printf '%s\n' "$expected" > "$work/payload"
    seed_user_home "$work" "$work/home"
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
