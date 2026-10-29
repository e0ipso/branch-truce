#!/usr/bin/env bash
set -euo pipefail
cli=${1:?Pass the path to the kenkeep 1.19.3 dist/cli.js}
cli=$(realpath "$cli")
version=$(node "$cli" --version)
printf 'kenkeep version: %s\n' "$version"
[ "$version" = 1.19.3 ]
fixture=$(mktemp -d "${TMPDIR:-/tmp}/kenkeep-related-repro.XXXXXXXX")
cleanup() {
  cd /
  rm -rf -- "$fixture"
  if [ -e "$fixture" ]; then
    printf 'cleanup failed: %s\n' "$fixture" >&2
    return 1
  fi
  printf 'cleanup confirmed: %s\n' "$fixture"
}
trap cleanup EXIT
export XDG_CONFIG_HOME="$fixture/config" GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
mkdir -p "$fixture/repo/.ai/kenkeep/nodes" "$fixture/repo/.ai/kenkeep/.state" "$fixture/empty-template"
git init --quiet --template="$fixture/empty-template" "$fixture/repo"
cd "$fixture/repo"
printf '1.19.3\n' > .ai/kenkeep/.state/installed-version
for name in alpha beta gamma; do
  cat > ".ai/kenkeep/nodes/practice-$name.md" <<NODE
---
type: practice
title: $name
description: $name example
tags: [example]
kk_schema_version: 3
kk_id: practice-$name
kk_derived_from: []
kk_relates_to: []
kk_depends_on: []
kk_confidence: high
---
$name body.
NODE
done
python3 - <<'PY'
from pathlib import Path
p = Path('.ai/kenkeep/nodes/practice-alpha.md')
s = p.read_text().replace('kk_relates_to: []', 'kk_relates_to: [practice-beta]')
s += '\n<!-- kk:related:start -->\n# Related\n\n- Related: [practice-beta](practice-beta.md)\n<!-- kk:related:end -->\n'
p.write_text(s)
PY
printf 'before edit and rebuild:\n'
sed -n '/<!-- kk:related:start -->/,/<!-- kk:related:end -->/p' .ai/kenkeep/nodes/practice-alpha.md
python3 - <<'PY'
from pathlib import Path
p = Path('.ai/kenkeep/nodes/practice-alpha.md')
p.write_text(p.read_text().replace('kk_relates_to: [practice-beta]', 'kk_relates_to: [practice-gamma]'))
PY
node "$cli" index rebuild
printf 'front matter after edit:\n'
grep '^kk_relates_to:' .ai/kenkeep/nodes/practice-alpha.md
printf 'related block after rebuild:\n'
sed -n '/<!-- kk:related:start -->/,/<!-- kk:related:end -->/p' .ai/kenkeep/nodes/practice-alpha.md
printf 'graph relation after rebuild:\n'
grep -F -- '- **relates_to:** practice-gamma' .ai/kenkeep/GRAPH.md
if grep -q -- '- Related: \[practice-beta\](practice-beta.md)' .ai/kenkeep/nodes/practice-alpha.md; then
  printf 'observation: stale related block reproduced\n'
else
  printf 'observation: stale related block not reproduced\n'
fi
printf 'running existing refresh-links command:\n'
node "$cli" node refresh-links
printf 'related block after refresh-links:\n'
sed -n '/<!-- kk:related:start -->/,/<!-- kk:related:end -->/p' .ai/kenkeep/nodes/practice-alpha.md
