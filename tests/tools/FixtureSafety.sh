#!/usr/bin/env bash
# All destructive calls are stubbed or confined to directories this invocation created.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
source "$root/tools/lib/fixture.sh"
work=$(fixture_root)
outside=$(mktemp -d)
trap 'command rm -rf -- "$work" "$outside"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
fixture_home "$work" "$work/home"
fixture_gitconfig "$work" "$work/home"
ln -s /usr "$work/escape"
ln -s "$outside" "$work/other"
rm() { echo "unexpected removal $*" >> "$work/removals"; return 99; }
for path in / /usr/ /usr// "$HOME/" "$work" "$work/" "$work//repo" "$work/../repo" "$work/escape/repo" "$work/other/repo"; do
    status=0
    fixture_merge_repo "$work" "$work/home" "$path" tracked.txt > "$work/result" 2>&1 || status=$?
    [[ "$status" == 2 ]] || fail "accepted repo '$path': $status"
    status=0
    fixture_home "$work" "$path" > "$work/result" 2>&1 || status=$?
    [[ "$status" == 2 ]] || fail "accepted home '$path': $status"
done
if fixture_home "$outside" "$outside/home" 2>/dev/null; then fail 'accepted an unowned root'; fi
[[ ! -e "$work/removals" ]] || fail 'invalid paths reached removal'
unset -f rm
fixture_merge_repo "$work" "$work/home" "$work/repo" tracked.txt notes.md image.bin
fixture_merge_repo "$work" "$work/home" "$work/repo" tracked.txt
[[ $(fixture_run "$work/home" "$work/repo" git diff --name-only --diff-filter=U) == tracked.txt ]] || fail 'ordinary merge fixture failed'
fixture_home "$work" "$work/home"
echo 'fixture ownership, normalization, symlink escapes and ordinary merge passed'

# Separate installed-validation steps must recover the same owned root without a package operation.
mkdir -p "$work/validation/lib"
cp "$root/tools/lib/fixture.sh" "$root/tools/lib/report.sh" "$work/validation/lib/"
sed "s|/run/bt-installed-fixture-root|$work/installed-fixture-root|g" "$root/tools/installed-checks.sh" \
    > "$work/validation/installed-checks.sh"
(
    source "$work/validation/installed-checks.sh"
    fixture=
    fixture_root() { printf '%s\n' "$work"; }
    c_install() { fixture_home "$fixture" "$home" && fixture_gitconfig "$fixture" "$home"; }
    installed_checks_main install
) || fail 'installed validation could not create its fixture state'
(
    source "$work/validation/installed-checks.sh"
    fixture=
    c_cli() { fixture_merge_repo "$fixture" "$home" "$repo" tracked.txt; }
    installed_checks_main cli
) || fail 'installed validation could not reuse its owned root'
echo 'installed validation steps preserve root ownership across invocations'
