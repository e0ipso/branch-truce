#!/usr/bin/env bash
# Every runtime Depends of the branch-truce package must be in the package list of
# tools/provision-debian.sh, because the tests start the app (icons, QML imports, platform
# plugins) from a build tree and a host missing one of them fails app.startup-appearance.
# Reads text files only; never runs the provisioning script, apt, Docker, the network or root.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
control=$root/packaging/debian/control
script=$root/tools/provision-debian.sh

# Package names of the Depends field, without version constraints or substitution variables.
mapfile -t depends < <(awk '
        /^Depends:/ { on = 1; sub("^Depends:", "") }
        on { print }
        on && $0 !~ /,[ \t]*$/ { on = 0 }
    ' "$control" | sed 's/([^)]*)//g; s/\${[^}]*}//g' | tr ',' '\n' | awk 'NF { print $1 }')
# Words of the packages=( ... ) array.
mapfile -t provisioned < <(awk '
        /^packages=\(/ { on = 1; sub("^packages=\\(", "") }
        on { print }
        on && /\)/ { on = 0 }
    ' "$script" | sed 's/)//' | tr -s ' \t' '\n' | awk 'NF')
((${#depends[@]} > 0)) || { echo "FAIL: no Depends found in $control" >&2; exit 1; }
((${#provisioned[@]} > 0)) || { echo "FAIL: no packages list found in $script" >&2; exit 1; }

failures=0
for package in "${depends[@]}"; do
    if ! printf '%s\n' "${provisioned[@]}" | grep -qxF -- "$package"; then
        echo "FAIL: tools/provision-debian.sh does not install the runtime Depends $package" >&2
        failures=$((failures + 1))
    fi
done
((failures == 0)) || exit 1
echo "${#depends[@]} runtime Depends are in the provisioning package list"
