# kenkeep Index: recovery

↑ Parent: [kenkeep](../index.md)

> kenkeep navigation: the injected body above is the root index node, the top-level catalog of branches and root-level leaves. Do not expect the whole knowledge base here; descend on demand. Read the root index node, pick one or more branches whose intent and tags match your task (several branches can be relevant), and read those branch `index.md` nodes. Descend further only where the task needs it, opening only the leaves you have confirmed are relevant. Follow each leaf's `relates_to` and `depends_on` cross edges to reach related leaves in other branches. You decide how deep to go per branch.

> This index only orients you; leaves hold the durable guidance. Open at least one relevant leaf before acting.

## Subfolders
- Load [`journal/`](journal/index.md) for more information on recovery journal format, acknowledgment, store selection and interrupted publication; read when changing persistence or restart behavior.

## Conventions (how we build)
- Open [**Reconcile uncertain destination publication**](practice-reconcile-uncertain-destination-publication.md) to learn about: Use captured destination identities, fail closed on unsupported metadata, and inspect uncertain exchanges before retrying. #publication #storage #concurrency #git

## Components (what exists)
_None yet._

## By topic

### #concurrency
- Open [**Reconcile uncertain destination publication**](practice-reconcile-uncertain-destination-publication.md) — Use captured destination identities, fail closed on unsupported metadata, and inspect uncertain exchanges before retrying.
- Open [**Hold a Git call to test racing repository changes**](../git/practice-hold-a-git-call-to-test-racing-repository-changes.md) — GitGate holds one git subcommand on one path so a write lands mid-inspection; keep a race test only if it fails without its guard.
- Open [**Fence session work and serialize worktree mutations**](../architecture/practice-fence-session-work-and-serialize-worktree-mutations.md) — Fence background completions by session and revision; serialize worktree mutations through cancellation reconciliation.
### #git
- Open [**Mergetool launcher decisions and reloads**](../git/mergetool/map-mergetool-launcher-decisions-and-reloads.md) — Launcher replies control parent staging; reloads reassign waiting paths and revalidate copied launch results.
- Open [**Mergetool socket lookup and access**](../git/mergetool/map-mergetool-socket-lookup-and-access.md) — GUI and launcher share a short owner-only socket directory with a locked random fallback.
- Open [**Real Git parent compatibility and fixture isolation**](../git/mergetool/map-real-git-parent-compatibility-and-fixture-isolation.md) — Real Git parent tests bound the compatibility claim and require short private runtime directories.
### #publication
- Open [**Treat mergetool file state as live evidence**](../git/practice-treat-mergetool-file-state-as-live-evidence.md) — Check live MERGED bytes before trusting a recorded write, shrink restores after flushing, and wait for the socket on a wall-clock deadline.
- Open [**Reconcile uncertain destination publication**](practice-reconcile-uncertain-destination-publication.md) — Use captured destination identities, fail closed on unsupported metadata, and inspect uncertain exchanges before retrying.
### #storage
- Open [**Recovery acknowledgment and retained input**](journal/map-recovery-acknowledgment-and-retained-input.md) — One bounded serial worker computes and persists commands; failures retain exact pending transactions for retry.
- Open [**Recovery journal format and validation**](journal/map-recovery-journal-format-and-validation.md) — Version 3 frames and complete domain checkpoints preserve retained history and reject invalid recovery.
- Open [**Recovery of copied launches and interrupted publication**](journal/map-recovery-of-copied-launches-and-interrupted-publication.md) — Copied launch destinations and interrupted mutations require current evidence and durable observations.