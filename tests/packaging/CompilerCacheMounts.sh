#!/usr/bin/env bash
# Exercise host cache mount selection through the real detached-stage runner.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cat > "$work/engine" <<'ENGINE'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >> "$CACHE_MOUNT_LOG"
case "$1" in
    image) if [[ "$*" == *'{{.Id}}'* ]]; then echo sha256:resolved-toolchain; else echo "$CACHE_IMAGE_INPUTS"; fi ;;
    run)
        case "$*" in
            *test-desktop.sh*|*package-release.sh*)
                if [ "${CACHE_EXPECT_MOUNT:-1}" = 1 ]; then
                    [[ "$*" == *':/compiler-cache '* && "$*" == *"v1-$CACHE_FORMAT-$(uname -m)-$CACHE_IMAGE_INPUTS-sha256:resolved-toolchain"* ]]
                else [[ "$*" != *':/compiler-cache'* ]]; fi ;;
            *) [[ "$*" != *':/compiler-cache'* ]] ;;
        esac
        echo cache-container ;;
    wait) echo 0 ;;
    logs|inspect|rm) ;;
    *) exit 99 ;;
esac
ENGINE
chmod +x "$work/engine"
export CONTAINER_ENGINE="$work/engine" CACHE_MOUNT_LOG="$work/engine.log"
for CACHE_FORMAT in kubuntu-deb fedora-rpm arch-pkg; do
    export CACHE_FORMAT
    CACHE_IMAGE_INPUTS="$("$root/packaging/ci/image-id.sh" --format "$CACHE_FORMAT")"
    export CACHE_IMAGE_INPUTS
    BRANCH_TRUCE_COMPILER_CACHE="$work/cache" bash "$root/packaging/scripts/container-check.sh" \
        --format "$CACHE_FORMAT" --version 1.2.3 --output "$work/$CACHE_FORMAT" > "$work/result"
    grep -qF "$CACHE_FORMAT check: ok" "$work/result"
    # Existing compatible results survive a subsequent run/source revision.
    touch "$work/cache/$CACHE_FORMAT-$(uname -m)-$CACHE_IMAGE_INPUTS/retained"
    BRANCH_TRUCE_COMPILER_CACHE="$work/cache" bash "$root/packaging/scripts/container-check.sh" \
        --format "$CACHE_FORMAT" --version 1.2.4 --output "$work/$CACHE_FORMAT-next" > "$work/result"
    test -f "$work/cache/$CACHE_FORMAT-$(uname -m)-$CACHE_IMAGE_INPUTS/retained"
done
# Host failure does not stop validation or add invalid bind mounts.
CACHE_EXPECT_MOUNT=0 BRANCH_TRUCE_COMPILER_CACHE=/proc/branch-truce-cache-unavailable \
    bash "$root/packaging/scripts/container-check.sh" --format arch-pkg --version 1.2.3 --output "$work/unavailable" > "$work/result" 2>&1
grep -qF 'arch-pkg check: ok' "$work/result"
grep -qF 'compiler cache unavailable on host' "$work/result"
echo 'cache host namespaces, build/test mounts, runtime exclusion and unavailable-host fallback passed'
