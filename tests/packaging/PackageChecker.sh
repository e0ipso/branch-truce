#!/usr/bin/env bash
# The shared package checker's home, application and removal contract, with a
# stand-in application and package manager. No root, package manager or
# installed package is used. NativeIntegration.sh runs this file.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
# expect_death MESSAGE CODE: CODE, run in a subshell after sourcing the checker, dies with MESSAGE.
expect_death() {
    local message=$1 code=$2
    if (source "$root/packaging/scripts/check-package.sh"; eval "$code") > "$work/result" 2>&1; then
        fail "accepted: $code"
    fi
    grep -qF -- "$message" "$work/result" || { cat "$work/result" >&2; fail "expected '$message'"; }
}

# Sourcing defines the functions and runs no check.
(source "$root/packaging/scripts/check-package.sh"; declare -F check_main run_app seed_user_home remove_and_check >/dev/null) \
    || fail 'sourcing the checker did not define its functions'

# The installed binary runs with the seeded home, so the removal checks cover
# the files it reads and writes there.
cat > "$work/app" <<'APP'
#!/usr/bin/env bash
printf '%s\n' "$HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$*" > "$HOME/app-saw"
echo 'written by the application' >> "$XDG_DATA_HOME/Branch Truce/recovery/unrelated/journal"
APP
chmod +x "$work/app"
(
    source "$root/packaging/scripts/check-package.sh"
    app_binary=$work/app
    seed_user_home "$work/home"
    [ -s "$work/home/config/branch-trucerc" ] && [ -s "$work/home/data/Branch Truce/recovery/unrelated/journal" ]
    run_app "$work/home" / --version
) || fail 'seeding or running the application failed'
[ "$(cat "$work/home/app-saw")" = "$(printf '%s\n' "$work/home" "$work/home/config" "$work/home/data" --version)" ] \
    || fail "the application did not run with the seeded home: $(cat "$work/home/app-saw" 2>/dev/null)"
grep -qF 'written by the application' "$work/home/data/Branch Truce/recovery/unrelated/journal" \
    || fail 'the application wrote outside the seeded home'

# Removal: the payload must be gone and the user home unchanged.
mkdir -p "$work/installed"
printf '%s\n' "$work/installed/branch-truce" > "$work/payload"
stubs="pkg_installed_version() { :; }; pkg_leftover_suffixes() { echo .save; }; pkg_extra_files() { :; }"
(source "$root/packaging/scripts/check-package.sh"; eval "$stubs"
    pkg_remove() { :; }
    remove_and_check "$work/home" "$work/payload") > "$work/result" || { cat "$work/result" >&2; fail 'a clean removal failed'; }
grep -qF 'the user home is unchanged' "$work/result" || fail 'a clean removal was not reported'
expect_death 'removal changed files in the user home' "$stubs
    pkg_remove() { echo changed >> \"$work/home/config/branch-trucerc\"; }
    remove_and_check \"$work/home\" \"$work/payload\""
expect_death 'removal changed files in the user home' "$stubs
    pkg_remove() { rm \"$work/home/data/Branch Truce/recovery/unrelated/journal\"; }
    remove_and_check \"$work/home\" \"$work/payload\""
touch "$work/installed/branch-truce"
expect_death "removal left $work/installed/branch-truce" "$stubs
    pkg_remove() { :; }
    remove_and_check \"$work/home\" \"$work/payload\""
rm "$work/installed/branch-truce"
touch "$work/installed/branch-truce.save"
expect_death "removal left $work/installed/branch-truce.save" "$stubs
    pkg_remove() { :; }
    remove_and_check \"$work/home\" \"$work/payload\""
expect_death 'branch-truce is still installed' "$stubs
    pkg_remove() { :; }; pkg_installed_version() { echo 1.2.3; }
    remove_and_check \"$work/home\" \"$work/payload\""
# A validator that rejects a file fails the check without the caller's errexit.
mkdir -p "$work/bin"
printf '#!/bin/sh\nexit 1\n' > "$work/bin/desktop-file-validate"
printf '#!/bin/sh\nexit 0\n' > "$work/bin/appstreamcli"
chmod +x "$work/bin/desktop-file-validate" "$work/bin/appstreamcli"
expect_death 'the desktop entry does not validate' "PATH=$work/bin:\$PATH; check_desktop_files"
# The previous package must be an older branch-truce for the same architecture,
# checked with the adapter's own version order before anything is installed.
previous="pkg_identity() { echo \"\$IDENTITY\"; }; pkg_older() { [ \"\$1\" = 1.0.0 ] && [ \"\$2\" = 1.2.3 ]; }"
(source "$root/packaging/scripts/check-package.sh"; eval "$previous"
    IDENTITY='branch-truce|1.0.0|amd64' check_previous /previous.deb 1.2.3 amd64) > "$work/result" \
    || { cat "$work/result" >&2; fail 'an older previous package was rejected'; }
[ "$(cat "$work/result")" = 1.0.0 ] || fail "previous version printed as '$(cat "$work/result")'"
for identity in 'branch-truce|1.2.3|amd64' 'branch-truce|1.2.4|amd64'; do
    expect_death "previous version ${identity:13:5} is not older than 1.2.3" "$previous
        IDENTITY='$identity' check_previous /previous.deb 1.2.3 amd64"
done
expect_death 'previous package is other for amd64, not branch-truce for amd64' "$previous
    IDENTITY='other|1.0.0|amd64' check_previous /previous.deb 1.2.3 amd64"
expect_death 'previous package is branch-truce for arm64, not branch-truce for amd64' "$previous
    IDENTITY='branch-truce|1.0.0|arm64' check_previous /previous.deb 1.2.3 amd64"
echo 'package checker home, application, previous package and removal contract passed'
