# Prepared worktrees

## Codex kenkeep hooks

Kenkeep's Codex hook scripts live under `.ai/kenkeep/hooks/codex/`, which
`.ai/kenkeep/.gitignore` excludes from Git. A prepared worktree can therefore
have the tracked knowledge base without the ignored hook scripts. In that
case, the hook-presence portion of `kenkeep doctor --harness codex` does not
apply to that worktree. A missing-hook result does not validate the knowledge
base; run the applicable knowledge-content checks separately.

The original checkout retains its installed hook scripts. Do not delete or
copy them implicitly during worktree preparation. If a worktree needs Codex
hook execution, install matching hooks explicitly and check their version
before relying on the harness doctor check.

## Commit message hook

The project worktree setup runs `.agents/scripts/install-worktree-hooks.sh`
after desktop preparation. With Node and npm available, it installs the locked
npm dependencies without lifecycle scripts and then installs Husky's hooks.
The tracked `.husky/commit-msg` runs commitlint. Installation errors stop setup.
If Node is absent, setup continues with a warning and commits are not checked
locally; follow the commit rules by hand and rely on CI until the hook is
installed. Node without npm is an incomplete installation and stops setup.
