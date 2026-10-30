#!/usr/bin/env bash
# Critical Fedora helper contracts, using an isolated package-builder fixture.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin" "$work/out"
export FEDORA_TEST_LOG="$work/log"
cat > "$work/bin/rpmbuild" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$@" > "$FEDORA_TEST_LOG"
while [ $# -gt 0 ]; do
    case "$1" in
        --define)
            case "$2" in
                '_topdir '*) top="${2#* }" ;;
                'branch_truce_native_version '*) version="${2#* }" ;;
            esac
            shift 2 ;;
        *) shift ;;
    esac
done
# Ensure the build receives the standalone source and its license.
tar -tzf "$top/SOURCES/branch-truce-$version.tar.gz" | sort > "$FEDORA_TEST_LOG.source"
grep -qx "branch-truce-$version/LICENSE" "$FEDORA_TEST_LOG.source"
mkdir -p "$top/RPMS/x86_64"
printf artifact > "$top/RPMS/x86_64/branch-truce-$version-1.fc44.x86_64.rpm"
STUB
cat > "$work/bin/rpm" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail
case "$1" in
    --eval)
        case "$2" in *fedora*) echo 44 ;; *_arch*) echo x86_64 ;; *) exit 9 ;; esac ;;
    -qp)
        # Identity is queried atomically, independently of the filename.
        printf '%s\n' "${FEDORA_TEST_IDENTITY:?}" ;;
    *) exit 9 ;;
esac
STUB
cat > "$work/bin/dnf" <<'STUB'
#!/usr/bin/env bash
printf 'dnf called\n' >> "$FEDORA_TEST_LOG.dnf"
exit 91
STUB
chmod +x "$work/bin/"*
export PATH="$work/bin:$PATH"
expect_failure() {
    local pattern="$1"
    shift
    if "$@" > "$work/result" 2>&1; then
        echo "FAIL: accepted invalid input: $*" >&2; exit 1
    fi
    grep -qF "$pattern" "$work/result" || { cat "$work/result"; exit 1; }
}
for semver in 1.2.3 1.2.3-rc.2; do
    native=1.2.3
    [ "$semver" = 1.2.3 ] || native=1.2.3~rc2
    export FEDORA_TEST_IDENTITY="branch-truce|0|$native|1.fc44|x86_64|GPL-3.0-or-later AND CC0-1.0"
    bash "$root/packaging/scripts/package-fedora.sh" --version "$semver" --output "$work/out" > "$work/result"
    test "$(tail -1 "$work/result")" = "$work/out/branch-truce-$native-1.fc44.x86_64.rpm"
    grep -qx "branch_truce_version $semver" "$FEDORA_TEST_LOG"
    grep -qx "branch_truce_native_version $native" "$FEDORA_TEST_LOG"
done
echo 'ok: stable/prerelease mapping and standalone licensed sources'
rm -f "$FEDORA_TEST_LOG"
expect_failure 'cannot be packaged' bash "$root/packaging/scripts/package-fedora.sh" --version 1.2.3-rc.0 --output "$work/out"
test ! -e "$FEDORA_TEST_LOG"
echo 'ok: unsupported version rejected before building'
stable_version=1.2.3
stable_package="$work/out/branch-truce-$stable_version-1.fc44.x86_64.rpm"
for identity in \
    'other|0|1.2.3|1.fc44|x86_64|GPL-3.0-or-later AND CC0-1.0' \
    'branch-truce|1|1.2.3|1.fc44|x86_64|GPL-3.0-or-later AND CC0-1.0' \
    'branch-truce|0|9.9.9|1.fc44|x86_64|GPL-3.0-or-later AND CC0-1.0' \
    'branch-truce|0|1.2.3|1.fc43|x86_64|GPL-3.0-or-later AND CC0-1.0' \
    'branch-truce|0|1.2.3|1.fc44|aarch64|GPL-3.0-or-later AND CC0-1.0' \
    'branch-truce|0|1.2.3|1.fc44|x86_64|MIT'; do
    export FEDORA_TEST_IDENTITY="$identity"
    rm -f "$stable_package"
    expect_failure 'RPM identity mismatch' bash "$root/packaging/scripts/package-fedora.sh" --version 1.2.3 --output "$work/out"
    test ! -e "$stable_package"
    touch "$work/candidate.rpm"
    expect_failure 'RPM identity mismatch' bash "$root/packaging/scripts/check-fedora.sh" --version 1.2.3 --package "$work/candidate.rpm"
    test ! -e "$FEDORA_TEST_LOG.dnf"
done
echo 'ok: wrong name, epoch, version, distribution, architecture and license fail before publication/install'
echo 'all Fedora helper contract checks passed'
