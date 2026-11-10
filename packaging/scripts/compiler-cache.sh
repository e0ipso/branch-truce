# shellcheck shell=bash
# Optional ccache for the compilation entry points: package-*.sh,
# test-desktop.sh, tools/verify-install-prefix.sh and the cache tests.
#
# Sourcing defines the functions below. It sets no shell options and runs
# nothing. A missing or unwritable cache, or report directory, is never an
# error: the build then compiles directly. Every diagnostic goes to stderr.
#
#   cache_prepare            shares BRANCH_TRUCE_COMPILER_CACHE_DIR between
#                            container root, test users and the Arch builder
#   cache_begin PROFILE BASE when BRANCH_TRUCE_COMPILER_CACHE_DIR and
#                            BRANCH_TRUCE_COMPILER_CACHE_NAMESPACE are set and
#                            ccache works, exports the CCACHE_* settings and
#                            BRANCH_TRUCE_CACHE_CMAKE_INCLUDE for this shell and
#                            the build tools it starts; BASE is the directory
#                            the build's paths are relative to
#   cache_cmake_args         the CMake arguments that enable the cache, one per
#                            line, or nothing when it is off
#   cache_report_path        the prefix of this build's report files, or nothing
#   cache_end                prints the build's statistics and restores every
#                            variable cache_begin changed
#
# A cache_begin inside an active one changes the settings but keeps the values
# saved by the first, so cache_end always restores the state before it.
# Sourcing the file again keeps an active build's saved values.
#
# Package recipes (debian/rules, the spec, the PKGBUILD) read
# BRANCH_TRUCE_CACHE_CMAKE_INCLUDE, and compiler-cache.cmake reads
# BRANCH_TRUCE_CACHE_PROFILE, from the environment, because dpkg-buildpackage,
# rpmbuild and makepkg pass nothing else through to CMake.

# Host bind mounts keep their owner; shared group permissions let container
# root, desktop test users and the Arch builder share entries and archives.
cache_prepare() {
    local directory="${BRANCH_TRUCE_COMPILER_CACHE_DIR:-}"
    [ -n "$directory" ] || return 0
    if ! mkdir -p "$directory"; then
        echo "compiler cache unavailable: $directory" >&2
        return 0
    fi
    if [ "$(id -u)" -eq 0 ]; then
        if ! chgrp -R 0 "$directory" || ! chmod -R g+rwX "$directory" \
            || ! find "$directory" -type d -exec chmod g+s {} +; then
            echo "compiler cache permissions unavailable: $directory" >&2
        fi
    fi
}

# The variables cache_begin sets, and their values before it ran.
declare -ga _cache_variables=(CCACHE_CONFIGPATH CCACHE_DIR CCACHE_BASEDIR CCACHE_NAMESPACE CCACHE_MAXSIZE
    CCACHE_COMPILERCHECK CCACHE_UMASK CCACHE_STATS CCACHE_STATSLOG CCACHE_HARDLINK CCACHE_SLOPPINESS
    CCACHE_HASHDIR CCACHE_NOHARDLINK CCACHE_REMOTE_STORAGE BRANCH_TRUCE_CACHE_PROFILE
    BRANCH_TRUCE_CACHE_CMAKE_INCLUDE BRANCH_TRUCE_CACHE_REPORT)
if ! declare -p _cache_saved >/dev/null 2>&1; then declare -gA _cache_saved=(); fi
_cache_active=${_cache_active:-false}

_cache_save() {
    local name
    if $_cache_active; then return 0; fi
    _cache_saved=()
    for name in "${_cache_variables[@]}"; do
        if [ -n "${!name+set}" ]; then _cache_saved[$name]=${!name}; fi
    done
    _cache_active=true
}

cache_begin() {
    local configuration=$1 base=$2 probe report_dir report
    _cache_save
    unset BRANCH_TRUCE_CACHE_CMAKE_INCLUDE BRANCH_TRUCE_CACHE_REPORT
    if [ -z "${BRANCH_TRUCE_COMPILER_CACHE_DIR:-}" ] \
        || [ -z "${BRANCH_TRUCE_COMPILER_CACHE_NAMESPACE:-}" ] \
        || ! command -v ccache >/dev/null; then
        echo "compiler cache disabled: $configuration" >&2
        return 0
    fi
    if ! mkdir -p "$BRANCH_TRUCE_COMPILER_CACHE_DIR" \
        || ! probe="$(mktemp "$BRANCH_TRUCE_COMPILER_CACHE_DIR/.write.XXXXXXXX")"; then
        echo "compiler cache unavailable; compiling directly: $configuration" >&2
        return 0
    fi
    rm -f "$probe"
    report_dir="${BRANCH_TRUCE_COMPILER_CACHE_REPORT_DIR:-$base/cache-reports}"
    # The host archives reports written by a mapped, unprivileged container uid.
    if ! mkdir -p "$report_dir" \
        || { [ "$(id -u)" -eq 0 ] && ! { chgrp 0 "$report_dir" && chmod g+rwx,g+s "$report_dir"; }; } \
        || ! report="$(mktemp "$report_dir/$configuration.XXXXXXXX")" || ! chmod 644 "$report"; then
        echo "compiler cache report directory unavailable; compiling directly: $report_dir" >&2
        return 0
    fi
    export CCACHE_CONFIGPATH=/dev/null
    export CCACHE_DIR="$BRANCH_TRUCE_COMPILER_CACHE_DIR"
    export CCACHE_BASEDIR="$base"
    export CCACHE_NAMESPACE="$BRANCH_TRUCE_COMPILER_CACHE_NAMESPACE-$configuration"
    export CCACHE_MAXSIZE=2G CCACHE_COMPILERCHECK=content CCACHE_UMASK=002
    export CCACHE_STATS=true CCACHE_STATSLOG="$report.log"
    unset CCACHE_HARDLINK
    export CCACHE_SLOPPINESS='' CCACHE_HASHDIR=true CCACHE_NOHARDLINK=1
    export CCACHE_REMOTE_STORAGE=''
    export BRANCH_TRUCE_CACHE_PROFILE="$configuration"
    export BRANCH_TRUCE_CACHE_REPORT="$report"
    export BRANCH_TRUCE_CACHE_CMAKE_INCLUDE
    BRANCH_TRUCE_CACHE_CMAKE_INCLUDE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/compiler-cache.cmake"
    ccache --show-config > "$report.config" || echo "compiler cache configuration unreadable: $report" >&2
    printf 'compiler cache: %s, uid=%s gid=%s groups=%s, namespace=%s, report=%s\n' \
        "$configuration" "$(id -u)" "$(id -g)" "$(id -G)" "$CCACHE_NAMESPACE" "$report" >&2
}

cache_cmake_args() {
    [ -z "${BRANCH_TRUCE_CACHE_CMAKE_INCLUDE:-}" ] \
        || printf '%s\n' "-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=$BRANCH_TRUCE_CACHE_CMAKE_INCLUDE"
}

cache_report_path() { printf '%s\n' "${BRANCH_TRUCE_CACHE_REPORT:-}" | sed '/^$/d'; }

cache_end() {
    local report="${BRANCH_TRUCE_CACHE_REPORT:-}" name
    if [ -n "$report" ]; then
        # Per-build logs avoid resetting shared counters or mixing concurrent stages.
        { ccache --print-log-stats > "$report.stats" && ccache --show-log-stats -v > "$report.summary" \
            && cat "$report.summary" >&2; } || echo "compiler cache statistics unavailable: $report" >&2
    fi
    # Without an active cache_begin there is nothing to restore.
    $_cache_active || return 0
    for name in "${_cache_variables[@]}"; do
        if [ -n "${_cache_saved[$name]+set}" ]; then
            export "$name=${_cache_saved[$name]}"
        else
            unset "$name"
        fi
    done
    _cache_saved=()
    _cache_active=false
}
