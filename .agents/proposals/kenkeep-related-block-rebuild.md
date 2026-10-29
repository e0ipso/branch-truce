# Proposal: refresh related blocks during index rebuild

**Version:** kenkeep 1.19.3, invoked with Node.js from the installed npm package.

After editing a node's `kk_relates_to` front matter, `kenkeep index rebuild`
updates the graph but leaves the node's generated `kk:related` block pointing
to the old node. Please consider refreshing generated related blocks during
`index rebuild`, so that a rebuild brings the visible links into agreement with
front matter as well as updating the graph.

## Reproduction

Run this in a disposable directory with kenkeep 1.19.3 installed. The manual
installed-version marker avoids running `init`, which would configure harness
hooks. The fixture uses schema 3 and needs no project or user configuration.

```sh
mkdir -p .ai/kenkeep/nodes .ai/kenkeep/.state
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
# Change the front matter while leaving the generated block alone.
python3 - <<'PY'
from pathlib import Path
p = Path('.ai/kenkeep/nodes/practice-alpha.md')
p.write_text(p.read_text().replace('kk_relates_to: [practice-beta]', 'kk_relates_to: [practice-gamma]'))
PY
kenkeep --version
kenkeep index rebuild
sed -n '/<!-- kk:related:start -->/,/<!-- kk:related:end -->/p' .ai/kenkeep/nodes/practice-alpha.md
```

The observed `index rebuild` exited 0, reported three nodes, and wrote the
`practice-gamma` relation to `GRAPH.md`. The alpha node's related block still
contained:

```markdown
<!-- kk:related:start -->
# Related

- Related: [practice-beta](practice-beta.md)
<!-- kk:related:end -->
```

Expected for this proposal: the block contains
`- Related: [practice-gamma](practice-gamma.md)` after the rebuild. The block
already pointed to beta before the front matter edit, so this is a stale link
rather than a missing initial block.

## Existing workaround and scope

`kenkeep node refresh-links` exited 0 in the same fixture, refreshed alpha's
block to gamma, and rebuilt the indexes. That command is a working workaround;
the request is to make `index rebuild` perform this synchronization too, or
otherwise make the distinction clear to users editing front matter by hand.
Consider the cost and write-scope change of rewriting leaves during a rebuild.

The local companion `kenkeep-related-block-rebuild-repro.sh` records the exact
fixture, checks the installed CLI version, prints the before and after blocks,
and removes its temporary Git repository on exit. The run used
`node /home/e0ipso/.npm/_npx/37f42b8f5bdb5da1/node_modules/kenkeep/dist/cli.js`
with version output `1.19.3`; that cache path is specific to the test machine.
