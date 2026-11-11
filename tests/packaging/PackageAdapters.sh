#!/usr/bin/env bash
# The contract every package manager adapter, packaging/scripts/pkg-<family>.sh,
# keeps for packaging/scripts/check-package.sh. Each adapter runs the same cases
# against stand-ins for its native tools (dpkg-deb and dpkg-query, rpm, bsdtar
# and pacman), which answer from a package described by a fixture directory.
# Where the host has the family's real version comparator, the ordering cases
# also run against it. No package is built, installed or removed.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
work=$(mktemp -d "${TMPDIR:-/tmp}/bt-package-adapters.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT
mkdir -p -- "$work/bin"
assertions=0
fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { assertions=$((assertions + 1)); }

# --- stand-ins for the native tools
# A package fixture is a directory: tags (TAG=value lines), files (payload
# paths), scripts (maintainer script names) and configs (configuration files).
# STUB_INSTALLED names the fixture that is installed, if any.
cat > "$work/bin/stub-tags" <<'TAGS'
#!/usr/bin/env bash
# stub-tags FIXTURE FORMAT: FORMAT with every %{TAG} replaced from FIXTURE/tags.
fixture=$1 format=$2
while IFS='=' read -r tag value; do
    format=${format//"%{$tag}"/$value}
done < "$fixture/tags"
printf '%b' "$format"
TAGS
cat > "$work/bin/dpkg-deb" <<'DPKG'
#!/usr/bin/env bash
set -euo pipefail
case "$1" in
    -f) sed -n "s/^$3=//p" "$2/tags" ;;
    --fsys-tarfile)
        mkdir -p "$STUB_TMP/payload/usr/bin"
        touch "$STUB_TMP/payload/usr/bin/branch-truce"
        tar -C "$STUB_TMP/payload" -cf - . ;;
    --ctrl-tarfile)
        mkdir -p "$STUB_TMP/control"
        rm -rf "${STUB_TMP:?}/control/"*
        touch "$STUB_TMP/control/control" "$STUB_TMP/control/md5sums"
        while IFS= read -r name; do touch "$STUB_TMP/control/$name"; done < "$2/scripts"
        [ ! -s "$2/configs" ] || cp "$2/configs" "$STUB_TMP/control/conffiles"
        tar -C "$STUB_TMP/control" -cf - . ;;
    *) exit 9 ;;
esac
DPKG
cat > "$work/bin/dpkg-query" <<'DPKG'
#!/usr/bin/env bash
[ "$*" = '-W -f ${Package}|${Status}|${Version}\n' ] || exit 9
[ "${STUB_DB_FAIL:-0}" = 0 ] || { echo 'database damaged' >&2; exit 42; }
[ "${STUB_DB_BAD:-0}" = 0 ] || { echo 'branch-truce|install ok installed'; exit 0; }
[ -n "${STUB_INSTALLED:-}" ] || exit 0
printf 'branch-truce|install ok installed|%s\n' "$(sed -n 's/^Version=//p' "$STUB_INSTALLED/tags")"
DPKG
cat > "$work/bin/rpm" <<'RPM'
#!/usr/bin/env bash
set -euo pipefail
case "$1 $2" in
    '-qp --qf')
        if [[ "$3" == *FILENAMES* ]]; then echo '/usr/bin/branch-truce -rwxr-xr-x'; else stub-tags "$4" "$3"; fi ;;
    '-qpR '*) echo 'git >= 2.43' ;;
    '-qp --scripts') if [ -s "$4/scripts" ]; then echo 'postinstall scriptlet (using /bin/sh):'; fi ;;
    '-qp --configfiles') cat "$3/configs" ;;
    '-qa --qf')
        [ "${STUB_DB_FAIL:-0}" = 0 ] || { echo 'database damaged' >&2; exit 42; }
        [ "${STUB_DB_BAD:-0}" = 0 ] || { echo 'branch-truce|0'; exit 0; }
        [ -z "${STUB_INSTALLED:-}" ] || stub-tags "$STUB_INSTALLED" "$3" ;;
    *) exit 9 ;;
esac
RPM
cat > "$work/bin/bsdtar" <<'BSDTAR'
#!/usr/bin/env bash
set -euo pipefail
case "$1" in
    -xOf) [ "$3" = .PKGINFO ] || exit 9; sed 's/^\([a-z]*\)=/\1 = /' "$2/tags" ;;
    -tf) printf '.PKGINFO\n.MTREE\n'; if [ -s "$2/scripts" ]; then echo .INSTALL; fi; cat "$2/files" ;;
    *) exit 9 ;;
esac
BSDTAR
cat > "$work/bin/pacman" <<'PACMAN'
#!/usr/bin/env bash
[ "$*" = '-Q' ] || exit 9
[ "${STUB_DB_FAIL:-0}" = 0 ] || { echo 'database damaged' >&2; exit 42; }
[ "${STUB_DB_BAD:-0}" = 0 ] || { echo branch-truce; exit 0; }
[ -n "${STUB_INSTALLED:-}" ] || exit 0
printf 'branch-truce %s\n' "$(sed -n 's/^pkgver=//p' "$STUB_INSTALLED/tags")"
PACMAN
# Return native-looking output and then fail, or fail with empty/truncated output.
for tool in dpkg-deb rpm bsdtar; do
    mv "$work/bin/$tool" "$work/bin/$tool.real"
    cat > "$work/bin/$tool" <<'FAULT'
#!/usr/bin/env bash
if [ "${STUB_INSPECT_FAIL:-0}" != 0 ]; then
    "$0.real" "$@" > "$STUB_TMP/partial" || true
    case "$STUB_INSPECT_FAIL" in
        1) cat "$STUB_TMP/partial" ;;
        truncated) head -c 64 "$STUB_TMP/partial" ;;
    esac
    echo 'inspection refused' >&2; exit 42
fi
exec "$0.real" "$@"
FAULT
done
chmod +x "$work/bin/"*
export STUB_TMP=$work/tmp
mkdir -p -- "$STUB_TMP"

# package FAMILY NAME EPOCH VERSION [config] [script]: a fixture of FAMILY's
# metadata for branch-truce at EPOCH (0 for none) and the native VERSION, with
# a configuration file and a maintainer script when asked.
package() {
    local family=$1 name=$2 epoch=$3 version=$4 dir
    shift 4
    dir=$work/packages/$family-$name-$epoch-$version${1:+-$1}${2:+-$2}
    rm -rf -- "$dir" && mkdir -p -- "$dir"
    printf 'usr/bin/branch-truce\n' > "$dir/files"
    : > "$dir/scripts"
    : > "$dir/configs"
    local evr=$version
    [ "$epoch" = 0 ] || evr=$epoch:$version
    case "$family" in
        deb) printf 'Package=%s\nVersion=%s\nArchitecture=amd64\nDepends=git (>= 2.43)\n' "$name" "$evr" > "$dir/tags" ;;
        rpm) printf 'NAME=%s\nEPOCHNUM=%s\nVERSION=%s\nRELEASE=1.fc44\nARCH=x86_64\nLICENSE=GPL-3.0-or-later AND CC0-1.0\n' \
            "$name" "$epoch" "$version" > "$dir/tags" ;;
        arch) printf 'pkgname=%s\npkgver=%s-1\narch=x86_64\nlicense=GPL-3.0-or-later\nlicense=CC0-1.0\ndepend=git>=2.43\n' \
            "$name" "$evr" > "$dir/tags" ;;
    esac
    local extra
    for extra in "$@"; do
        case "$extra" in
            config)
                echo /etc/branch-truce.conf > "$dir/configs"
                [ "$family" != arch ] || echo 'backup=etc/branch-truce.conf' >> "$dir/tags" ;;
            script) echo postinst > "$dir/scripts" ;;
        esac
    done
    printf '%s\n' "$dir"
}

# adapter FAMILY CODE [ARG...]: CODE in a shell that sourced only that adapter,
# with the stand-ins first on PATH.
adapter() {
    local family=$1 code=$2
    shift 2
    PATH="$work/bin:$PATH" bash -c 'set -eu; set '"$pipefail"' pipefail; before=$(set +o); trap '"'"'[ "$(set +o)" = "$before" ] || exit 99'"'"' EXIT; source "$1"; shift; '"$code" bash \
        "$root/packaging/scripts/pkg-$family.sh" "$@"
}

for pipefail in +o -o; do
for family in deb rpm arch; do
    for epoch in 0 2; do
        plain=$(package "$family" branch-truce "$epoch" 1.2.3)
        identity=$(adapter "$family" 'pkg_identity "$1"' "$plain")
        IFS='|' read -r name version architecture <<<"$identity"
        [ "$name" = branch-truce ] && [ -n "$version" ] && [ -n "$architecture" ] \
            || fail "$family identity '$identity'"
        pass
        if [ "$epoch" = 0 ]; then
            [[ "$version" != *:* ]] || fail "$family shows an epoch that is not set: $version"
        else
            [[ "$version" == "$epoch:"* ]] || fail "$family hides epoch $epoch: $version"
        fi
        pass
        # The installed version is the native version of the identity, epoch
        # included, because the checker compares the two.
        installed=$(STUB_INSTALLED=$plain adapter "$family" 'pkg_installed_version')
        [ "$installed" = "$version" ] || fail "$family installed version '$installed', identity version '$version'"
        pass
    done
    adapter "$family" 'status=0; output=$(pkg_installed_version) || status=$?; [ "$status" = 1 ] && [ -z "$output" ]' \
        || fail "$family did not confirm absence"
    pass
    for fault in STUB_DB_FAIL STUB_DB_BAD; do
        export "$fault=1"
        adapter "$family" 'status=0; pkg_installed_version || status=$?; [ "$status" = 2 ]' 2> "$work/error" \
            || fail "$family hid $fault"
        unset "$fault"
        pass
    done
    adapter "$family" '[ "$(pkg_files "$1")" = /usr/bin/branch-truce ]; [ "$(pkg_depends "$1")" = git ]' "$plain" || fail "$family inspector output"
    pass
    # Scripts, triggers and configuration files all count; a plain payload does not.
    adapter "$family" 'status=0; pkg_has_scripts "$1" || status=$?; [ "$status" = 1 ]' "$(package "$family" branch-truce 0 1.2.3)" \
        || fail "$family reports scripts for a plain package"
    pass
    adapter "$family" 'pkg_has_scripts "$1"' "$(package "$family" branch-truce 0 1.2.3 script)" \
        || fail "$family misses a maintainer script"
    pass
    adapter "$family" 'pkg_has_scripts "$1"' "$(package "$family" branch-truce 0 1.2.3 config)" \
        || fail "$family misses a configuration file"
    pass
    adapter "$family" 'pkg_check_metadata "$1"' "$(package "$family" branch-truce 0 1.2.3)" \
        || fail "$family rejects the expected metadata"
    pass
    for fault in 1 empty truncated; do
        for operation in pkg_identity pkg_check_metadata pkg_files pkg_depends pkg_has_scripts; do
            [ "$family:$operation" != deb:pkg_check_metadata ] || continue
            STUB_INSPECT_FAIL=$fault adapter "$family" 'status=0; "$1" "$2" || status=$?; if [ "$1" = pkg_has_scripts ]; then [ "$status" = 2 ]; else [ "$status" != 0 ]; fi' \
                "$operation" "$plain" > "$work/output" 2> "$work/error" || fail "$family $operation hid $fault inspection failure"
            grep -q 'inspection refused' "$work/error" || fail "$family lost the inspection diagnostic"
            pass
        done
    done
    adapter "$family" 'status=0; pkg_recipe_depends "$1" || status=$?; [ "$status" != 0 ]' "$work/missing" \
        2> "$work/error" || fail "$family hid an unreadable recipe"
    pass
    # Every adapter defines the whole contract.
    adapter "$family" 'declare -F pkg_identity pkg_check_metadata pkg_files pkg_extra_files pkg_has_scripts \
        pkg_depends pkg_recipe_depends pkg_older pkg_target_ready pkg_refresh pkg_install pkg_remove \
        pkg_installed_version pkg_verify_installed pkg_runtime_versions pkg_qt_root pkg_leftover_suffixes >/dev/null' \
        || fail "$family lacks part of the adapter contract"
    pass
done
done

# pkg_older with the family's real comparator, where the host has it: release
# candidates, stable releases and native release revisions order strictly.
comparator() { case "$1" in deb) echo dpkg ;; rpm) echo rpm ;; arch) echo vercmp ;; esac; }
native() {
    local family=$1 version=$2 style=tilde suffix=''
    case "$family" in rpm) suffix=-1.fc44 ;; arch) style=plain suffix=-1 ;; esac
    printf '%s%s\n' "$("$root/packaging/scripts/native-version.sh" --style "$style" "$version")" "$suffix"
}
for family in deb rpm arch; do
    command -v "$(comparator "$family")" >/dev/null || { echo "skip: no $(comparator "$family") for $family ordering"; continue; }
    older() { bash -c 'set -euo pipefail; source "$1"; pkg_older "$2" "$3"' bash "$root/packaging/scripts/pkg-$family.sh" "$1" "$2"; }
    for pair in '1.2.3-rc.2 1.2.3-rc.10' '1.2.3-rc.10 1.2.3' '1.2.3 1.10.0'; do
        read -r old new <<<"$pair"
        old=$(native "$family" "$old") new=$(native "$family" "$new")
        older "$old" "$new" || fail "$family rejects the upgrade $old -> $new"
        if older "$new" "$old"; then fail "$family accepts the downgrade $new -> $old"; fi
        if older "$old" "$old"; then fail "$family accepts the equal version $old"; fi
        pass
    done
    if [ "$family" != deb ]; then
        revision=$(native "$family" 1.2.3)
        older "$revision" "${revision/-1/-2}" || fail "$family loses the native release revision order"
        pass
    fi
done
echo "$assertions package adapter contract assertions passed"
