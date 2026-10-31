#!/usr/bin/env bash
# Real-RPM rejection tests. Run in a clean Fedora target with the two built
# 1.2.3 and 1.2.3-rc.1 artifacts. A package-manager sentinel forbids mutations.
set -euo pipefail
[ $# -eq 2 ] || { echo "usage: $0 <stable.rpm> <prerelease.rpm>" >&2; exit 2; }
root="$(cd "$(dirname "$0")/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir "$work/bin"
export FEDORA_DNF_SENTINEL="$work/dnf-called"
cat > "$work/bin/dnf" <<'STUB'
#!/usr/bin/env bash
touch "$FEDORA_DNF_SENTINEL"
exit 91
STUB
chmod +x "$work/bin/dnf"
export PATH="$work/bin:$PATH"
reject() {
    local expected="$1"
    shift
    if bash "$root/packaging/scripts/check-fedora.sh" "$@" > "$work/result" 2>&1; then
        echo 'FAIL: checker accepted an invalid RPM input' >&2; exit 1
    fi
    grep -qF "$expected" "$work/result" || { cat "$work/result"; exit 1; }
    [ ! -e "$FEDORA_DNF_SENTINEL" ] || { echo 'FAIL: package manager ran for rejected input'; exit 1; }
}
reject 'is not older' --version 1.2.3 --package "$1" --previous "$1"
reject 'is not older' --version 1.2.3-rc.1 --package "$2" --previous "$1"
reject 'RPM identity mismatch' --version 1.2.4 --package "$1"
echo 'ok: real RPM identity/equal/downgrade rejection before package-manager mutation'
before="$(find /tmp -maxdepth 1 -type d -name 'bt-fedora-check.*' | sort)"
status=0
bash "$root/packaging/scripts/check-fedora.sh" --version 1.2.3 --package "$1" > "$work/result" 2>&1 || status=$?
[ "$status" -eq 91 ] && [ -f "$FEDORA_DNF_SENTINEL" ] || { cat "$work/result"; exit 1; }
after="$(find /tmp -maxdepth 1 -type d -name 'bt-fedora-check.*' | sort)"
[ "$before" = "$after" ] || { echo 'FAIL: failed install left temporary state behind'; exit 1; }
echo 'all real Fedora RPM input and failed-install cleanup checks passed'
