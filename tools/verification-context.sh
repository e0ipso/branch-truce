#!/usr/bin/env bash
# Capture observed check dependencies. This does not certify dependency closure.
# Print an external environment.sh path; source that file before run/validation.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
mode=${1:-source}
case "$mode" in source|desktop|core|sanitize) ;; *)
    echo "Usage: $0 [source|desktop|core|sanitize]" >&2; exit 2 ;;
esac
if [[ "$mode" != source && ! -f "$root/build/$mode/CMakeCache.txt" ]]; then
    echo "Configure build/$mode before capturing its artifacts." >&2
    exit 1
fi
caller_umask=$(umask)
umask 077
context=$(mktemp -d /tmp/branch-truce-verification.XXXXXXXX)
digest() { sha256sum | awk '{print $1}'; }
file_identity() {
    local file=$1
    printf 'path %q\n' "$file"
    if [[ -f "$file" ]]; then
        stat -Lc 'mode %a size %s' -- "$file"
        sha256sum < "$file"
    else
        printf 'missing\n'
    fi
}
{
    uname -srmv
    file_identity /etc/os-release
    file_identity /etc/ld.so.cache
    getconf GNU_LIBC_VERSION
} > "$context/platform.txt"
{
    for tool in bash cmake ctest c++ make git awk sort find sha256sum readlink stat dpkg-query; do
        executable=$(command -v "$tool")
        printf 'tool %q\n' "$tool"
        file_identity "$(readlink -f -- "$executable")"
    done
    cmake --version
    ctest --version
    c++ --version
    c++ -print-search-dirs
    make --version
    git --version
    bash --version
} > "$context/toolchain.txt"
dpkg-query -W -f='${binary:Package}\t${Version}\t${db:Status-Status}\n' \
    | LC_ALL=C sort > "$context/packages.tsv"
# The normalized syntax check only executes env and Bash. Capture their linked
# libraries by content as well as the binaries. Other checks have broader,
# unresolved compiler/QML/plugin/configuration closures.
/usr/bin/env -i PATH=/usr/bin:/bin LC_ALL=C /usr/bin/ldd /usr/bin/env /bin/bash /usr/bin/bash \
    > "$context/shell-libraries.txt"
if grep -q 'not found' "$context/shell-libraries.txt"; then
    echo "Unresolved shell library; retained context: $context" >&2; exit 1
fi
awk '$2=="=>" && $3 ~ /^\// {print $3} $1 ~ /^\// {print $1}' \
    "$context/shell-libraries.txt" | LC_ALL=C sort -u > "$context/shell-library-paths.txt"
{
    file_identity /usr/bin/env
    file_identity /bin/bash
    file_identity /usr/bin/bash
    while IFS= read -r file; do file_identity "$file"; done < "$context/shell-library-paths.txt"
} > "$context/shell.txt"
shell_stamp=$(digest < "$context/shell.txt")
# The initial env process is loaded before it clears its environment. Refuse a
# reusable shell identity when loader overrides could alter that first process.
while IFS= read -r name; do
    case "$name" in LD_*|GLIBC_TUNABLES) shell_stamp='' ;; esac
done < <(compgen -e)
# Store hashes rather than environment values, which can contain credentials.
# This allowlist is observed context, not a claim that other variables cannot matter.
{
    for name in PATH HOME TMPDIR LANG LANGUAGE LC_ALL LC_CTYPE LC_MESSAGES \
        CC CXX CFLAGS CXXFLAGS CPPFLAGS LDFLAGS CPATH CPLUS_INCLUDE_PATH \
        LIBRARY_PATH COMPILER_PATH GCC_EXEC_PREFIX CMAKE_PREFIX_PATH CMAKE_GENERATOR \
        CMAKE_TOOLCHAIN_FILE CMAKE_BUILD_PARALLEL_LEVEL MAKEFLAGS \
        LD_LIBRARY_PATH LD_PRELOAD QT_PLUGIN_PATH QML_IMPORT_PATH QML2_IMPORT_PATH \
        QT_QPA_PLATFORM QT_QPA_PLATFORMTHEME QT_STYLE_OVERRIDE QT_QUICK_BACKEND \
        QT_QUICK_CONTROLS_STYLE QT_LOGGING_RULES QT_FATAL_WARNINGS \
        QSG_RHI_BACKEND QSG_RENDER_LOOP DISPLAY WAYLAND_DISPLAY \
        XDG_RUNTIME_DIR XDG_CONFIG_HOME XDG_CONFIG_DIRS XDG_DATA_HOME XDG_DATA_DIRS \
        ASAN_OPTIONS LSAN_OPTIONS UBSAN_OPTIONS BASH_ENV ENV \
        GIT_CONFIG_GLOBAL GIT_CONFIG_SYSTEM GIT_CONFIG_NOSYSTEM GIT_CONFIG_COUNT; do
        if [[ -v "$name" ]]; then
            printf '%s\tset\t%s\n' "$name" "$(printf '%s' "${!name}" | digest)"
        else
            printf '%s\tunset\n' "$name"
        fi
    done
    printf 'umask\t%s\n' "$caller_umask"
} > "$context/environment.tsv"
{
    printf 'mode %s\n' "$mode"
    if [[ "$mode" != source ]]; then
        build="$root/build/$mode"
        # Include generated test registration, binaries and build configuration.
        # Testing/ logs change during CTest and must not invalidate its own receipt.
        find "$build" -path "$build/Testing" -prune -o -type f \
            \( -name CMakeCache.txt -o -name CTestTestfile.cmake \
            -o -name compile_commands.json -o -name branch-truce \
            -o -name branch_truce_core_smoke -o -name '*.qrc' -o -name '*.rcc' \) \
            -print0 | LC_ALL=C sort -z > "$context/artifact-paths.nul"
        while IFS= read -r -d '' file; do
            printf 'artifact %q\n' "${file#"$root/"}"
            file_identity "$file"
        done < "$context/artifact-paths.nul"
        ctest --test-dir "$build" --show-only=json-v1 > "$context/ctest-membership.json"
        cat "$context/ctest-membership.json"
    fi
} > "$context/artifacts.txt"
git -C "$root" rev-parse HEAD > "$context/revision.txt"
{
    printf '# Observations only; dependency closure is not certified.\n'
    printf 'export BRANCH_TRUCE_PLATFORM_STAMP=%q\n' "$(digest < "$context/platform.txt")"
    printf 'export BRANCH_TRUCE_TOOLCHAIN_STAMP=%q\n' "$(digest < "$context/toolchain.txt")"
    printf 'export BRANCH_TRUCE_RUNTIME_STAMP=%q\n' "$(digest < "$context/packages.tsv")"
    printf 'export BRANCH_TRUCE_ENVIRONMENT_STAMP=%q\n' "$(digest < "$context/environment.tsv")"
    printf 'export BRANCH_TRUCE_ARTIFACT_STAMP=%q\n' "$(digest < "$context/artifacts.txt")"
    printf 'export BRANCH_TRUCE_REVISION_STAMP=%q\n' "$(digest < "$context/revision.txt")"
    printf 'export BRANCH_TRUCE_SHELL_STAMP=%q\n' "$shell_stamp"
} > "$context/environment.sh"
printf '%s\n' "$context/environment.sh"
