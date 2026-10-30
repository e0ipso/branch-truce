#!/usr/bin/env bash
# Host-safe contracts for native naming, builder dispatch and container failures.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
bash "$root/tests/packaging/DesktopPreparation.sh"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/repo/packaging" "$work/bin"
cp -r "$root/packaging/scripts" "$root/packaging/ci" "$work/repo/packaging/"
expect_failure() {
    local message="$1"; shift
    if "$@" > "$work/result" 2>&1; then echo "FAIL: accepted $message" >&2; exit 1; fi
    grep -qF "$message" "$work/result" || { cat "$work/result"; exit 1; }
}
for format in kubuntu-deb fedora-rpm arch-pkg; do
    native=1.2.3~rc2; architecture=x86_64
    case "$format" in
        kubuntu-deb) filename="branch-truce_${native}_amd64-ubuntu26.04.deb"; architecture=amd64 ;;
        fedora-rpm) filename="branch-truce-$native-1.fc44.x86_64.rpm"; native+=-1.fc44 ;;
        arch-pkg) native=1.2.3rc2; filename="branch-truce-$native-1-x86_64.pkg.tar.zst"; native+=-1 ;;
    esac
    actual="$(bash "$root/packaging/scripts/package-info.sh" --format "$format" --version 1.2.3-rc.2)"
    expected="$(printf '{"format":"%s","version":"1.2.3-rc.2","nativeVersion":"%s","filename":"%s","architecture":"%s"}' "$format" "$native" "$filename" "$architecture")"
    [ "$actual" = "$expected" ] || { echo "FAIL: $actual != $expected"; exit 1; }
    expect_failure 'cannot be packaged' bash "$root/packaging/scripts/package-info.sh" --format "$format" --version 1.2.3-rc.0
    expect_failure 'cannot be packaged' bash "$root/packaging/scripts/package-release.sh" --format "$format" --version '1.2.3;true' --output "$work/out"
done
expect_failure 'usage:' bash "$root/packaging/scripts/package-info.sh" --format tuxedo-deb --version 1.2.3
# Dispatch uses the native helper's actual output, and preserves its failure.
for pair in fedora:rpm arch:pkg.tar.zst; do
    helper="${pair%:*}"; extension="${pair#*:}"
    cat > "$work/repo/packaging/scripts/package-$helper.sh" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$@" > "$NATIVE_DISPATCH_LOG"
[ "${NATIVE_DISPATCH_FAIL:-0}" = 0 ] || exit 43
mkdir -p "$4"
touch "$4/native-result"
echo "$4/native-result"
STUB
    chmod +x "$work/repo/packaging/scripts/package-$helper.sh"
    export NATIVE_DISPATCH_LOG="$work/dispatch"
    format="$helper-$extension"; [ "$helper" != arch ] || format=arch-pkg
    [ "$(bash "$work/repo/packaging/scripts/package-release.sh" --format "$format" --version 1.2.3 --output "$work/out")" = "$work/out/native-result" ]
    [ "$(cat "$work/dispatch")" = "$(printf '%s\n' --version 1.2.3 --output "$work/out")" ]
    if NATIVE_DISPATCH_FAIL=1 bash "$work/repo/packaging/scripts/package-release.sh" --format "$format" --version 1.2.3 --output "$work/out"; then exit 1; else [ "$?" = 43 ]; fi
done
# The engine logs complete commands; no Docker, package manager or host mutation.
cat > "$work/bin/engine" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >> "$NATIVE_ENGINE_LOG"
case "$1" in
    image) exit 1 ;;
    build) [ "${NATIVE_FAIL_STAGE:-}" != images ] || exit 41 ;;
    run)
        stage=baseline
        case "$*" in
            *test-desktop.sh*) stage=test ;;
            *package-release.sh*) stage=package ;;
            *check-package.sh*|*check-fedora.sh*|*check-arch.sh*)
                [[ "$*" != *':/src'* && "$*" != *':/out'* ]] || { echo 'source/output mounted into runtime'; exit 99; }
                for ((i=1; i<=$#; i++)); do
                    argument="${!i}"
                    case "$argument" in
                        *:/fixture:ro)
                            fixture="${argument%:/fixture:ro}"
                            [ -f "$fixture/assets/branding/platform/linux/hicolor/scalable/apps/com.canpicasoft.branch-truce.svg" ]
                            [ -f "$fixture/packaging/scripts/check-fedora.sh" ]
                            [ ! -e "$fixture/src" ] && [ ! -e "$fixture/build" ] && [ ! -e "$fixture/cmake" ] ;;
                    esac
                done
                stage=check ;;
        esac
        echo "$stage" ;;
    wait)
        if [ "${NATIVE_FAIL_STAGE:-}" = "$2" ]; then
            case "$2" in test) echo 42 ;; package) echo 43 ;; check) echo 44 ;; *) echo 0 ;; esac
        else echo 0; fi ;;
    logs)
        if [ "${NATIVE_FINAL_LOG_FAIL:-0}" = 1 ] && [ "${2-}" != --follow ]; then
            echo 'final log read failed' >&2; exit 90
        fi
        if [ "${NATIVE_FOLLOW_FAIL:-0}" = 1 ] && [ "${2-}" = --follow ]; then
            echo 'follow connection ended' >&2; exit 91
        fi
        echo 'complete native container logs' ;;
    inspect|rm) ;;
    *) exit 9 ;;
esac
STUB
chmod +x "$work/bin/engine"
export CONTAINER_ENGINE="$work/bin/engine" NATIVE_ENGINE_LOG="$work/engine.log"
runner="$root/packaging/scripts/container-check.sh"
for format in kubuntu-deb fedora-rpm arch-pkg; do
    : > "$work/engine.log"
    bash "$runner" --format "$format" --version 1.2.3-rc.2 --output "$work/$format" > "$work/result"
    grep -qF "$format check: ok" "$work/result"
    grep -qF "ci/${format%%-*}.Dockerfile" "$work/engine.log"
    grep -qF 'test-desktop.sh' "$work/engine.log"
    candidate_version=1.2.3~rc2
    case "$format" in
        kubuntu-deb) candidate_name="branch-truce_${candidate_version}_amd64-ubuntu26.04.deb" ;;
        fedora-rpm) candidate_name="branch-truce-$candidate_version-1.fc44.x86_64.rpm" ;;
        arch-pkg) candidate_version=1.2.3rc2; candidate_name="branch-truce-$candidate_version-1-x86_64.pkg.tar.zst" ;;
    esac
    grep -qF "/packages/$candidate_name" "$work/engine.log"
    case "$format" in kubuntu-deb) extension=deb ;; fedora-rpm) extension=rpm ;; arch-pkg) extension=pkg.tar.zst ;; esac
    touch "$work/previous.$extension" "$work/wrong.deb" "$work/wrong.rpm"
    wrong_extension=rpm; [ "$format" != fedora-rpm ] || wrong_extension=deb
    expect_failure 'extension' bash "$runner" --format "$format" --version 1.2.3 --output "$work/bad" --previous-package "$work/wrong.$wrong_extension"
    : > "$work/engine.log"
    bash "$runner" --format "$format" --version 1.2.3 --output "$work/given" --previous-package "$work/previous.$extension" > "$work/result"
    grep -qF "bash $format  1.2.3" "$work/engine.log"
    grep -qF "/previous/previous.$extension" "$work/engine.log"
    expect_failure 'extension' bash "$runner" --format "$format" --version 1.2.3 --output "$work/bad" --previous-package "$work/wrong.zip"
done
for stage in images test package check; do
    export NATIVE_FAIL_STAGE="$stage"
    expect_failure 'FAILED' bash "$runner" --format fedora-rpm --version 1.2.3 --output "$work/failure-$stage"
    case "$stage" in
        images) grep -qF 'check: skipped (no images)' "$work/result" ;;
        test) grep -qF 'check: ok' "$work/result" ;;
        package) grep -qF 'check: skipped (no package was built)' "$work/result" ;;
        check) grep -qF 'check: FAILED (exit 44)' "$work/result" ;;
    esac
done
unset NATIVE_FAIL_STAGE
bash "$runner" --format all --version 1.2.3 --output "$work/all" > "$work/result"
[ "$(grep -c ' check: ok' "$work/result")" = 3 ]
expect_failure 'usage:' bash "$runner" --format tuxedo-deb --version 1.2.3 --output "$work/retired"
NATIVE_FOLLOW_FAIL=1 bash "$runner" --format arch-pkg --version 1.2.3 --output "$work/log-recovery" > "$work/result"
if ! grep -qF 'complete native container logs' "$work/log-recovery/containers/arch-pkg/baseline.log"; then
    echo 'FAIL: complete container logs lost after follow disconnect' >&2; exit 1
fi
: > "$work/engine.log"
NATIVE_FINAL_LOG_FAIL=1 expect_failure 'FAILED' bash "$runner" --format arch-pkg --version 1.2.3 --output "$work/log-failure"
[ "$(cat "$work/log-failure/containers/arch-pkg/baseline.exit")" = 0 ]
! grep -q '^rm ' "$work/engine.log"
# A log-collection failure must retain the successful container for diagnosis.
# Its recorded native exit remains zero even though the stage fails.
echo 'all native integration contracts passed'
