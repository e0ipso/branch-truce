#!/usr/bin/env bash
# Sourced by compilation entry points. A missing/unwritable cache is optional.
# Host bind mounts retain their owner; shared group permissions let container
# root, desktop test users and the Arch builder share entries and host archives.
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

cache_begin() {
    local configuration="$1" base="$2" probe report_dir
    cache_cmake_args=()
    cache_report=''
    export BRANCH_TRUCE_CACHE_CMAKE_INCLUDE=''
    if [ -z "${BRANCH_TRUCE_COMPILER_CACHE_DIR:-}" ] \
        || [ -z "${BRANCH_TRUCE_COMPILER_CACHE_NAMESPACE:-}" ] \
        || ! command -v ccache >/dev/null; then
        echo "compiler cache disabled: $configuration"
        return 0
    fi
    if ! mkdir -p "$BRANCH_TRUCE_COMPILER_CACHE_DIR" \
        || ! probe="$(mktemp "$BRANCH_TRUCE_COMPILER_CACHE_DIR/.write.XXXXXXXX")"; then
        echo "compiler cache unavailable; compiling directly: $configuration" >&2
        return 0
    fi
    rm -f "$probe"
    report_dir="${BRANCH_TRUCE_COMPILER_CACHE_REPORT_DIR:-$base/cache-reports}"
    mkdir -p "$report_dir"
    if [ "$(id -u)" -eq 0 ]; then
        chgrp 0 "$report_dir"
        chmod g+rwx,g+s "$report_dir"
    fi
    cache_report="$(mktemp "$report_dir/$configuration.XXXXXXXX")"
    # The host archives reports written by a mapped, unprivileged container uid.
    chmod 644 "$cache_report"
    export CCACHE_CONFIGPATH=/dev/null
    export CCACHE_DIR="$BRANCH_TRUCE_COMPILER_CACHE_DIR"
    export CCACHE_BASEDIR="$base"
    export BRANCH_TRUCE_CACHE_PROFILE="$configuration"
    export CCACHE_NAMESPACE="$BRANCH_TRUCE_COMPILER_CACHE_NAMESPACE-$configuration"
    export CCACHE_MAXSIZE=2G CCACHE_COMPILERCHECK=content CCACHE_UMASK=002
    export CCACHE_STATS=true CCACHE_STATSLOG="$cache_report.log"
    unset CCACHE_HARDLINK
    export CCACHE_SLOPPINESS='' CCACHE_HASHDIR=true CCACHE_NOHARDLINK=1
    export CCACHE_REMOTE_STORAGE=''
    export BRANCH_TRUCE_CACHE_CMAKE_INCLUDE
    BRANCH_TRUCE_CACHE_CMAKE_INCLUDE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/compiler-cache.cmake"
    # Returned to the sourcing compiler entry point.
    # shellcheck disable=SC2034
    cache_cmake_args=("-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=$BRANCH_TRUCE_CACHE_CMAKE_INCLUDE")
    ccache --show-config > "$cache_report.config"
    printf 'compiler cache: %s, uid=%s gid=%s groups=%s, namespace=%s, report=%s\n' \
        "$configuration" "$(id -u)" "$(id -g)" "$(id -G)" "$CCACHE_NAMESPACE" "$cache_report"
}

cache_end() {
    [ -n "${cache_report:-}" ] || return 0
    # Per-build logs avoid resetting shared counters or mixing concurrent stages.
    ccache --print-log-stats > "$cache_report.stats"
    ccache --show-log-stats -v > "$cache_report.summary"
    cat "$cache_report.summary"
}
