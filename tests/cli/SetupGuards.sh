#!/usr/bin/env bash
# Exercise setup CLI rejection paths before tools or packages are consulted.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
assertions=0

expect_rejected() {
    local expected_message=$1 output status
    shift
    if output=$("$@" 2>&1); then status=0; else status=$?; fi
    if [[ "$status" != 2 || "$output" != *"$expected_message"* ]]; then
        printf 'Expected exit 2 with %q from:' "$expected_message" >&2
        printf ' %q' "$@" >&2
        printf '\nActual exit %s:\n%s\n' "$status" "$output" >&2
        exit 1
    fi
    assertions=$((assertions + 1))
}

expect_rejected 'Usage:' "$root/tools/prepare-worktree.sh" invalid-mode
for mode in desktop core sanitize; do
    expect_rejected 'Build directory must be absolute.' \
        "$root/tools/prepare-worktree.sh" "$mode" 'relative build directory'
done
expect_rejected 'Usage:' "$root/tools/provision-debian.sh" --invalid-mode
printf '%s CLI guard assertions passed\n' "$assertions"

# Run the tracked installer only inside an owned repository. PATHs below hide
# the host Node/npm; fake tools never install dependencies or change user config.
fixture=$(mktemp -d "${TMPDIR:-/tmp}/truce-hook-fixture.XXXXXXXX")
trap 'rm -rf -- "$fixture"' EXIT
mkdir -p "$fixture/repo/.agents/scripts" "$fixture/repo/.husky" "$fixture/bin"
cp "$root/.agents/scripts/install-worktree-hooks.sh" "$fixture/repo/.agents/scripts/"
cp "$root/.husky/commit-msg" "$fixture/repo/.husky/"
GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null git -C "$fixture/repo" init -q
ln -s "$(command -v bash)" "$fixture/bin/bash"
ln -s "$(command -v dirname)" "$fixture/bin/dirname"
ln -s "$(command -v env)" "$fixture/bin/env"
installer="$fixture/repo/.agents/scripts/install-worktree-hooks.sh"
if output=$(PATH="$fixture/bin" "$installer" 2>&1); then status=0; else status=$?; fi
[[ "$status" == 0 && "$output" == *'Node is unavailable'* ]] || {
    printf 'no-Node installer: exit %s: %s\n' "$status" "$output" >&2; exit 1;
}
assertions=$((assertions + 1))
printf '#!/bin/sh\nexit 0\n' > "$fixture/bin/node"
chmod +x "$fixture/bin/node"
if output=$(PATH="$fixture/bin" "$installer" 2>&1); then status=0; else status=$?; fi
[[ "$status" == 1 && "$output" == *'npm is missing'* ]] || {
    printf 'no-npm installer: exit %s: %s\n' "$status" "$output" >&2; exit 1;
}
assertions=$((assertions + 1))
cat > "$fixture/bin/npm" <<'NPM'
#!/bin/bash
[[ "$*" == 'ci --ignore-scripts' ]] || exit 9
/bin/mkdir -p node_modules/.bin
cat_file=$HOOK_FIXTURE_HUSKY
/bin/cp "$cat_file" node_modules/.bin/husky
NPM
cat > "$fixture/husky" <<'HUSKY'
#!/bin/bash
[[ ! ${HUSKY+x} ]] || exit 10
printf 'husky called\n' > "$HOOK_FIXTURE_RESULT"
HUSKY
chmod +x "$fixture/bin/npm" "$fixture/husky"
HOOK_FIXTURE_HUSKY="$fixture/husky" HOOK_FIXTURE_RESULT="$fixture/result" HUSKY=0 \
    PATH="$fixture/bin" "$installer"
[[ $(<"$fixture/result") == 'husky called' ]]
cmp "$root/.husky/commit-msg" "$fixture/repo/.husky/commit-msg"
assertions=$((assertions + 2))
printf '%s total CLI and installer assertions passed\n' "$assertions"
