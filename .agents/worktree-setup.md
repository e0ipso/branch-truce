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
