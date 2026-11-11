# shellcheck shell=bash
# Shared check output; sourcing sets no options and performs no checks.
# step TITLE... prints a blank line and == TITLE; pass TEXT... prints PASS TEXT.
# fail ACCUMULATOR TEXT... prints FAIL TEXT and increments the named accumulator,
# initially zero if unset. It returns success so callers can collect every failure
# under any shell options; the command decides its exit status from the count.
# die TEXT... prints FAIL TEXT on stderr and exits 1.

step() { printf '\n== %s\n' "$*"; }
pass() { printf 'PASS %s\n' "$*"; }
fail() {
    printf 'FAIL %s\n' "${*:2}"
    printf -v "$1" '%d' "$((${!1:-0} + 1))"
}
die() { printf 'FAIL %s\n' "$*" >&2; exit 1; }
