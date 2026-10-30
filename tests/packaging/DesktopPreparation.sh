#!/usr/bin/env bash
# Account-lookup adapter regression. All privileged operations are sentinels.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/source" "$work/bin" "$work/reports"
printf fixture > "$work/source/input"
# Adapt only the two fixed mount destinations in a disposable script copy.
sed -e "s|/src|$work/source|g" -e "s|/reports|$work/reports|g" "$root/packaging/scripts/test-desktop.sh" > "$work/entry.sh"
export PREPARATION_LOG="$work/operations"
cat > "$work/bin/getent" <<'STUB'
#!/usr/bin/env bash
if [ "${PREPARATION_EXISTING:-0}" = 1 ]; then
    [ "$1" != passwd ] || echo 'fixture-user:x:12001:12001:Fixture:/tmp:/bin/bash'
    exit 0
fi
exit 2
STUB
for operation in chown useradd groupadd runuser; do
    cat > "$work/bin/$operation" <<'STUB'
#!/usr/bin/env bash
printf '%s %s\n' "${0##*/}" "$*" >> "$PREPARATION_LOG"
STUB
    chmod +x "$work/bin/$operation"
done
chmod +x "$work/bin/getent"
for existing in 0 1; do
    : > "$PREPARATION_LOG"
    if ! PATH="$work/bin:$PATH" PREPARATION_EXISTING="$existing" bash "$work/entry.sh" 1.2.3 12001 12001 > "$work/result" 2>&1; then
        cat "$work/result"; echo 'FAIL: desktop account preparation exited before dispatch' >&2; exit 1
    fi
    grep -q '^chown ' "$PREPARATION_LOG"
    if [ "$existing" = 0 ]; then
        grep -q '^useradd ' "$PREPARATION_LOG"
        grep -q '^groupadd ' "$PREPARATION_LOG"
        grep -q '^runuser -u bt-test ' "$PREPARATION_LOG"
    else
        ! grep -q '^useradd ' "$PREPARATION_LOG"
        grep -q '^runuser -u fixture-user ' "$PREPARATION_LOG"
    fi
done
echo 'desktop account preparation passed for existing and absent uid/group'
