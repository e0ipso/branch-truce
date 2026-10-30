#!/usr/bin/env bash
# Every runtime Depends of the branch-truce package must be installed in the
# test stage of each CI Dockerfile, because the test stage runs the app (icons,
# QML imports, platform plugins) from a build tree and a package missing there
# only shows up as a failed container-check. A name also in Build-Depends counts as
# installed: the build stage already has it. Reads text files only; no Docker, network or root.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
control=$root/packaging/debian/control

# Package names of a control field, without version constraints or substitution variables.
field_names() {
    awk -v field="$1" '
        $0 ~ "^" field ":" { on = 1; sub("^" field ":", "") }
        on { print }
        on && $0 !~ /,[ \t]*$/ { on = 0 }
    ' "$control" | sed 's/([^)]*)//g; s/\${[^}]*}//g' | tr ',' '\n' | awk 'NF { print $1 }'
}
# Words installed by the test stage of a Dockerfile.
test_stage_words() {
    awk '/^FROM / { on = ($0 ~ / AS test$/) } on' "$1" | sed 's/\\$//' | tr -s ' \t' '\n'
}

mapfile -t depends < <(field_names Depends)
mapfile -t build_depends < <(field_names Build-Depends)
((${#depends[@]} > 0)) || { echo "FAIL: no Depends found in $control" >&2; exit 1; }

failures=0
checked=0
for format in kubuntu; do
    dockerfile=$root/packaging/ci/$format.Dockerfile
    mapfile -t installed < <(test_stage_words "$dockerfile")
    ((${#installed[@]} > 0)) || { echo "FAIL: no test stage in $dockerfile" >&2; failures=$((failures + 1)); continue; }
    for package in "${depends[@]}"; do
        checked=$((checked + 1))
        if printf '%s\n' "${installed[@]}" "${build_depends[@]}" | grep -qxF -- "$package"; then
            continue
        fi
        echo "FAIL: $format.Dockerfile test stage does not install the runtime Depends $package" >&2
        failures=$((failures + 1))
    done
done
((failures == 0)) || exit 1
echo "$checked runtime Depends checks passed across the CI test stages"
