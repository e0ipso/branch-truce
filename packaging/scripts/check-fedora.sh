#!/usr/bin/env bash
# Run as root in a disposable Fedora 44 target container, never on a desktop.
# Supply helpers, expected branding exports and RPM inputs only. The installed
# binary runs with private HOME/XDG directories and no source/build tree.
set -euo pipefail
usage() { echo "usage: $0 --version <application-semver> --package <file> [--previous <file>]" >&2; exit 2; }
die() { echo "error: $*" >&2; exit 1; }
identity() { rpm -qp --qf '%{NAME}|%{EPOCHNUM}|%{VERSION}|%{RELEASE}|%{ARCH}|%{LICENSE}\n' "$1"; }
# Use RPM's comparison, including tilde prereleases and release revisions.
older() {
    [ "$(OLD="$1" NEW="$2" rpm --eval '%{lua:print(rpm.vercmp(os.getenv("OLD"), os.getenv("NEW")))}')" = -1 ]
}
check_identity() {
    local got
    got="$(identity "$1")"
    [ "$got" = "branch-truce|0|$2|1.fc44|x86_64|GPL-3.0-or-later AND CC0-1.0" ] \
        || die "RPM identity mismatch: $got"
}
main() {
    local version='' package='' previous='' native_version previous_version previous_release got deps dep path files payload_files
    while [ $# -gt 0 ]; do
        [ $# -ge 2 ] || usage
        case "$1" in
            --version) version="$2" ;;
            --package) package="$2" ;;
            --previous) previous="$2" ;;
            *) usage ;;
        esac
        shift 2
    done
    [ -n "$version" ] && [ -n "$package" ] || usage
    native_version="$("$(dirname "${BASH_SOURCE[0]}")/native-version.sh" "$version")" || exit 2
    package="$(realpath -e "$package")"
    [[ "$package" = *.rpm ]] || die 'candidate must be an RPM'
    check_identity "$package" "$native_version"
    [ "$(id -u)" = 0 ] && [ "$(rpm --eval '%{?fedora}')" = 44 ] \
        && [ "$(rpm --eval '%{_arch}')" = x86_64 ] || die 'use a disposable Fedora 44 x86_64 target as root'
    if rpm -q branch-truce >/dev/null 2>&1; then die 'branch-truce is already installed; use a clean target'; fi
    if [ -n "$previous" ]; then
        previous="$(realpath -e "$previous")"
        [[ "$previous" = *.rpm ]] || die 'previous package must be an RPM'
        # Check all metadata and native ordering before any package mutation.
        IFS='|' read -r got dep previous_version previous_release path files < <(identity "$previous")
        [ "$got" = branch-truce ] && [ "$dep" = 0 ] && [ "$path" = x86_64 ] \
            && [ "$files" = 'GPL-3.0-or-later AND CC0-1.0' ] \
            && [[ "$previous_release" =~ ^[1-9][0-9]*\.fc44$ ]] || die 'previous RPM identity mismatch'
        older "$previous_version-$previous_release" "$native_version-1.fc44" \
            || die "previous RPM $previous_version-$previous_release is not older than $native_version-1.fc44"
        echo "ok: previous RPM $previous_version-$previous_release is older by native RPM ordering"
    fi
    local app_id=com.canpicasoft.branch-truce
    local binary=/usr/bin/branch-truce
    local expected_files=("$binary" "/usr/share/applications/$app_id.desktop" "/usr/share/metainfo/$app_id.metainfo.xml"
        "/usr/share/licenses/branch-truce/LICENSE" "/usr/share/icons/hicolor/scalable/apps/$app_id.svg")
    for got in 16 22 24 32; do expected_files+=("/usr/share/icons/hicolor/${got}x${got}/apps/$app_id.png"); done
    files="$(rpm -qpl "$package")"
    payload_files="$(rpm -qp --qf '[%{FILENAMES} %{FILEMODES:perms}\n]' "$package" | awk '$2 !~ /^d/ {print $1}')"
    for path in "${expected_files[@]}"; do
        grep -qxF "$path" <<<"$files" || die "missing payload $path"
    done
    [ -z "$(grep -v '^/usr/' <<<"$files")" ] || die 'payload outside /usr'
    [ -z "$(rpm -qp --configfiles "$package")" ] || die 'unexpected system configuration files'
    [ -z "$(rpm -qp --scripts --triggers "$package")" ] || die 'unexpected package scripts or triggers'
    deps="$(rpm -qpR "$package")"
    for dep in git hicolor-icon-theme breeze-icon-theme plasma-breeze-qt6 \
        'qt6-qtsvg(x86-64)' 'qt6-qtwayland(x86-64)' \
        'qt6qml(org.kde.desktop)' 'qt6qml(org.kde.kirigami)' \
        'qt6qml(QtQml)' 'qt6qml(QtQml.Models)' 'qt6qml(QtQml.WorkerScript)' \
        'qt6qml(QtQuick)' 'qt6qml(QtQuick.Controls)' 'qt6qml(QtQuick.Layouts)' \
        'qt6qml(QtQuick.Shapes)' 'qt6qml(QtQuick.Templates)' 'qt6qml(QtQuick.Window)' \
        'libQt6Core.so.6()(64bit)' 'libQt6Gui.so.6()(64bit)' 'libQt6Network.so.6()(64bit)' \
        'libQt6Qml.so.6()(64bit)' 'libQt6Quick.so.6()(64bit)' 'libQt6Widgets.so.6()(64bit)' \
        'libKF6SyntaxHighlighting.so.6()(64bit)'; do
        grep -qxF "$dep" < <(sed 's/ .*//' <<<"$deps") || die "missing declared dependency $dep"
    done
    echo 'ok: candidate metadata, payload and explicit/native runtime dependencies'
    local work cleanup_command
    work="$(mktemp -d /tmp/bt-fedora-check.XXXXXX)"
    printf -v cleanup_command 'rm -rf -- %q' "$work"
    # Capture the quoted path while the local work variable is in scope.
    # shellcheck disable=SC2064
    trap "$cleanup_command" EXIT
    mkdir -p "$work/user/.config" "$work/user/.local/share/branch-truce/recovery"
    printf '[General]\nwrapLines=true\n' > "$work/user/.config/branch-trucerc"
    head -c 256 /dev/urandom > "$work/user/.local/share/branch-truce/recovery/session.journal"
    (cd "$work/user" && find . -type f -exec sha256sum {} +) > "$work/user.sha256"
    # Only local-file signature checking is excepted for our unsigned RPMs.
    # Repository gpgcheck remains enabled. Weak deps are disabled to expose the
    # installed runtime closure rather than relying on recommended packages.
    install_rpm() { dnf -y --setopt=localpkg_gpgcheck=False --setopt=install_weak_deps=False install "$1"; }
    remove_rpm() { dnf -y remove --no-autoremove branch-truce; }
    check_installed() {
        local want="$1" out size
        [ "$(rpm -q --qf '%{VERSION}-%{RELEASE}' branch-truce)" = "$want" ] || die 'installed version mismatch'
        rpm -V branch-truce
        for path in "${expected_files[@]}"; do [ -f "$path" ] || die "missing installed file $path"; done
        desktop-file-validate "/usr/share/applications/$app_id.desktop"
        for size in 16 22 24 32; do
            path="/usr/share/icons/hicolor/${size}x${size}/apps/$app_id.png"
            file "$path" | grep -q "PNG image data, $size x $size," || die "wrong PNG dimensions: $path"
        done
        grep -q '<svg' "/usr/share/icons/hicolor/scalable/apps/$app_id.svg" || die 'invalid scalable icon'
        grep -qxF '[scalable/apps]' /usr/share/icons/hicolor/index.theme || die 'hicolor scalable directory missing'
        out="$(ldd "$binary")"
        ! grep -q 'not found' <<<"$out" || die "unresolved native libraries: $out"
        ! objdump -T "$binary" | grep 'Qt_6_PRIVATE_API' || die 'binary uses private Qt ABI'
        readelf -h "$binary" | grep -q 'DYN ' || die 'binary lacks PIE hardening'
        readelf -lW "$binary" | grep -q 'GNU_RELRO' || die 'binary lacks RELRO hardening'
        readelf -d "$binary" | grep -q 'BIND_NOW' || die 'binary lacks immediate binding hardening'
        mkdir -p "$work/version-home/data" "$work/version-home/config" "$work/version-home/cache" "$work/version-home/runtime"
        chmod 700 "$work/version-home/runtime"
        out="$(env -i PATH=/usr/bin:/bin HOME="$work/version-home" LC_ALL=C.UTF-8 \
            XDG_DATA_HOME="$work/version-home/data" XDG_CONFIG_HOME="$work/version-home/config" \
            XDG_CACHE_HOME="$work/version-home/cache" XDG_RUNTIME_DIR="$work/version-home/runtime" \
            QT_QPA_PLATFORM=offscreen "$binary" --version)"
        [ "$out" = "Branch Truce $version" ] || die "wrong application version: $out"
        local branding rel
        branding="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/assets/branding/platform/linux/hicolor"
        for path in "${expected_files[@]}"; do
            case "$path" in
                /usr/share/icons/hicolor/*)
                    rel="${path#/usr/share/icons/hicolor/}"
                    cmp -s "$branding/$rel" "$path" || die "icon bytes differ for $rel" ;;
            esac
        done
        appstreamcli validate --no-net "/usr/share/metainfo/$app_id.metainfo.xml"
        echo "ok: installed $want, file integrity, icons, native libraries, bytecode ABI and hardening"
    }
    smoke() {
        local repo="$work/repo" runtime_home="$work/runtime-home" out
        rm -rf "$repo" "$runtime_home"
        mkdir -p "$repo" "$runtime_home/data" "$runtime_home/config" "$runtime_home/cache" "$runtime_home/runtime"
        chmod 700 "$runtime_home/runtime"
        (
            cd "$repo"
            git init --quiet --initial-branch=main
            git config user.name 'Package Smoke'
            git config user.email smoke@example.invalid
            git config core.autocrlf false
            printf 'base\n' > tracked.txt
            git add tracked.txt
            git commit --quiet -m base
            git checkout --quiet -b side
            printf 'side\n' > tracked.txt
            git commit --quiet -am side
            git checkout --quiet main
            printf 'main\n' > tracked.txt
            git commit --quiet -am main
            if git merge --no-edit side > "$work/merge.log" 2>&1; then die 'fixture merge did not conflict'; fi
            [ "$(git diff --name-only --diff-filter=U)" = tracked.txt ] || die 'fixture conflict missing'
        )
        out="$(env -i PATH=/usr/bin:/bin HOME="$runtime_home" LC_ALL=C.UTF-8 \
            XDG_DATA_HOME="$runtime_home/data" XDG_CONFIG_HOME="$runtime_home/config" XDG_CACHE_HOME="$runtime_home/cache" XDG_RUNTIME_DIR="$runtime_home/runtime" \
            GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null GIT_TERMINAL_PROMPT=0 \
            QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
            timeout 60 "$binary" --smoke-test "$repo")"
        [[ "$out" = *'"state":"ready"'* && "$out" = *'"operation":"Merge"'* \
            && "$out" = *'"path":"tracked.txt"'* ]] || die "unexpected smoke result: $out"
        echo 'ok: installed QML/Kirigami smoke opened a real merge conflict with isolated HOME/XDG'
    }
    check_removed() {
        if rpm -q branch-truce >/dev/null 2>&1; then die 'package still installed'; fi
        while IFS= read -r path; do
            [ ! -e "$path" ] && [ ! -L "$path" ] || die "leftover $path"
        done <<<"$payload_files"
        # RPM has no separate purge operation. No package-owned configuration,
        # scripts or triggers are allowed; remove must leave no .rpmsave/.rpmnew.
        for path in "${expected_files[@]}"; do
            [ ! -e "$path.rpmsave" ] && [ ! -e "$path.rpmnew" ] || die "leftover RPM configuration $path"
        done
        (cd "$work/user" && sha256sum --quiet -c "$work/user.sha256")
        echo 'ok: complete removal, no RPM config leftovers, user settings/recovery unchanged'
    }
    install_rpm "$package"
    rpm -q qt6-qtbase qt6-qtdeclarative kf6-kirigami kf6-syntax-highlighting git rpm
    check_installed "$native_version-1.fc44"
    smoke
    remove_rpm
    check_removed
    if [ -n "$previous" ]; then
        install_rpm "$previous"
        [ "$(rpm -q --qf '%{VERSION}-%{RELEASE}' branch-truce)" = "$previous_version-$previous_release" ] \
            || die 'previous RPM was not installed'
        install_rpm "$package"
        check_installed "$native_version-1.fc44"
        smoke
        remove_rpm
        check_removed
        echo 'ok: native upgrade lifecycle'
    fi
    rm -rf "$work"
    trap - EXIT
    echo "all checks passed for $(basename "$package")"
}
if [ "${BASH_SOURCE[0]}" = "$0" ]; then main "$@"; fi
