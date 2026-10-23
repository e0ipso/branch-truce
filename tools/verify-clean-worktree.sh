#!/usr/bin/env bash
# Validate the committed scaffold in a disposable detached worktree.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
fixture=$(mktemp -d "${TMPDIR:-/tmp}/branch-truce-clean.XXXXXXXX")
checkout="$fixture/checkout"
cleanup() {
    local status=$?
    trap - EXIT
    if [[ -d "$checkout" ]]; then
        git -C "$root" worktree remove "$checkout" || {
            echo "Cleanup failed; retained fixture: $fixture" >&2; exit 1;
        }
    fi
    rm -rf -- "$fixture"
    echo "Clean-worktree fixture removed: $fixture"
    exit "$status"
}
trap cleanup EXIT
git -C "$root" worktree add --detach "$checkout" HEAD
test -z "$(git -C "$checkout" status --porcelain)"
for mode in desktop core sanitize; do
    # Exercise the setup hook's default in-checkout build directory.
    (cd -- "$checkout" && ./tools/prepare-worktree.sh "$mode")
    test -z "$(git -C "$checkout" status --porcelain)"
    cmake --build "$checkout/build/$mode" --parallel 2
    ctest --test-dir "$checkout/build/$mode" --output-on-failure -V \
        -R '^(core.build-smoke|desktop.startup)$'
done
cmake --build "$checkout/build/desktop" --target branch_truce_ui_qmllint
test -z "$(git -C "$checkout" status --porcelain)"
echo 'Clean worktree remained clean after preparation, build and CTest.'
