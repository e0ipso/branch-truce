# shellcheck shell=bash
# The report lines of the tools that run a series of checks:
# tools/validate-installed-artifact.sh, tools/installed-checks.sh and
# tools/verify-install-prefix.sh. Sourcing defines the functions below; it
# sets no shell options and runs nothing.
#
#   step TITLE...   a blank line, then "== TITLE", on stdout
#   pass TEXT...    "PASS TEXT" on stdout
#   fail TEXT...    "FAIL TEXT" on stdout, and one more failed check in the
#                   caller's variable failures (unset counts as 0); the run goes on
#   die TEXT...     "FAIL TEXT" on stderr, then exit 1
#
# A tool that calls fail decides its result from failures, never from its output.

step() { printf '\n== %s\n' "$*"; }
pass() { printf 'PASS %s\n' "$*"; }
fail() { printf 'FAIL %s\n' "$*"; failures=$((${failures:-0} + 1)); }
die() { printf 'FAIL %s\n' "$*" >&2; exit 1; }
