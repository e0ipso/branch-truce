# kenkeep Index: architecture

↑ Parent: [kenkeep](../index.md)

> kenkeep navigation: the injected body above is the root index node, the top-level catalog of branches and root-level leaves. Do not expect the whole knowledge base here; descend on demand. Read the root index node, pick one or more branches whose intent and tags match your task (several branches can be relevant), and read those branch `index.md` nodes. Descend further only where the task needs it, opening only the leaves you have confirmed are relevant. Follow each leaf's `relates_to` and `depends_on` cross edges to reach related leaves in other branches. You decide how deep to go per branch.

> This index only orients you; leaves hold the durable guidance. Open at least one relevant leaf before acting.

## Subfolders
- Load [`desktop-baseline/`](desktop-baseline/index.md) for more information on desktop stack, installed program and editing envelope; read when changing platform requirements, bootstrap or resource limits.

## Conventions (how we build)
- Open [**Design desktop apps around screens and components**](practice-design-desktop-apps-around-screens-and-components.md) to learn about: Define screens and components, then build loosely coupled implementations using each desktop environment's idiomatic UI kit. #architecture #desktop #techspec
- Open [**Fence session work and serialize worktree mutations**](practice-fence-session-work-and-serialize-worktree-mutations.md) to learn about: Fence background completions by session and revision; serialize worktree mutations through cancellation reconciliation. #architecture #concurrency #cancellation #git #desktop

## Components (what exists)
_None yet._

## By topic

### #architecture
- Open [**Desktop comparison and workflow integration**](desktop-baseline/map-desktop-comparison-and-workflow-integration.md) — The technical specification defines comparison, app drafts, publication and mergetool integration.
- Open [**Desktop platform and implementation stack**](desktop-baseline/map-desktop-platform-and-implementation-stack.md) — C++20, Qt Quick and Kirigami define the native Linux desktop stack and its platform baseline.
- Open [**Editing and recovery resource envelope**](desktop-baseline/map-editing-and-recovery-resource-envelope.md) — Text, metadata, history, destination and recovery limits preserve unfinished work on refusal.
### #desktop
- Open [**Desktop comparison and workflow integration**](desktop-baseline/map-desktop-comparison-and-workflow-integration.md) — The technical specification defines comparison, app drafts, publication and mergetool integration.
- Open [**Desktop platform and implementation stack**](desktop-baseline/map-desktop-platform-and-implementation-stack.md) — C++20, Qt Quick and Kirigami define the native Linux desktop stack and its platform baseline.
- Open [**Editing and recovery resource envelope**](desktop-baseline/map-editing-and-recovery-resource-envelope.md) — Text, metadata, history, destination and recovery limits preserve unfinished work on refusal.
### #cancellation
- Open [**Fence session work and serialize worktree mutations**](practice-fence-session-work-and-serialize-worktree-mutations.md) — Fence background completions by session and revision; serialize worktree mutations through cancellation reconciliation.
### #concurrency
- Open [**Reconcile uncertain destination publication**](../recovery/practice-reconcile-uncertain-destination-publication.md) — Use captured destination identities, fail closed on unsupported metadata, and inspect uncertain exchanges before retrying.
- Open [**Hold a Git call to test racing repository changes**](../git/practice-hold-a-git-call-to-test-racing-repository-changes.md) — GitGate holds one git subcommand on one path so a write lands mid-inspection; keep a race test only if it fails without its guard.
- Open [**Fence session work and serialize worktree mutations**](practice-fence-session-work-and-serialize-worktree-mutations.md) — Fence background completions by session and revision; serialize worktree mutations through cancellation reconciliation.
### #git
- Open [**Mergetool launcher decisions and reloads**](../git/mergetool/map-mergetool-launcher-decisions-and-reloads.md) — Launcher replies control parent staging; reloads reassign waiting paths and revalidate copied launch results.
- Open [**Mergetool socket lookup and access**](../git/mergetool/map-mergetool-socket-lookup-and-access.md) — GUI and launcher share a short owner-only socket directory with a locked random fallback.
- Open [**Real Git parent compatibility and fixture isolation**](../git/mergetool/map-real-git-parent-compatibility-and-fixture-isolation.md) — Real Git parent tests bound the compatibility claim and require short private runtime directories.
### #techspec
- Open [**Design desktop apps around screens and components**](practice-design-desktop-apps-around-screens-and-components.md) — Define screens and components, then build loosely coupled implementations using each desktop environment's idiomatic UI kit.
- Open [**Keep TechSpecs and PRDs independent**](../engineering/practice-keep-techspecs-and-prds-independent.md) — TechSpecs explain how to build; PRDs explain what to build and why. Keep both separate and independently understandable.