# kenkeep Index: git

↑ Parent: [kenkeep](../index.md)

> kenkeep navigation: the injected body above is the root index node, the top-level catalog of branches and root-level leaves. Do not expect the whole knowledge base here; descend on demand. Read the root index node, pick one or more branches whose intent and tags match your task (several branches can be relevant), and read those branch `index.md` nodes. Descend further only where the task needs it, opening only the leaves you have confirmed are relevant. Follow each leaf's `relates_to` and `depends_on` cross edges to reach related leaves in other branches. You decide how deep to go per branch.

> This index only orients you; leaves hold the durable guidance. Open at least one relevant leaf before acting.

## Subfolders
- Load [`mergetool/`](mergetool/index.md) for more information on mergetool socket transport, launcher lifecycle and real-parent tests; read when changing attachment, parent staging or socket isolation.

## Conventions (how we build)
- Open [**Treat mergetool file state as live evidence**](practice-treat-mergetool-file-state-as-live-evidence.md) to learn about: Check live MERGED bytes before trusting a recorded write, shrink restores after flushing, and wait for the socket on a wall-clock deadline. #mergetool #publication #git #failures
- Open [**Bound each converted representation independently**](practice-bound-each-converted-representation-independently.md) to learn about: Reject cleaned canonical objects over 4194304 bytes before reading; give each Git phase its own 4194304-byte budget. #git #conversion #limits #failures
- Open [**Hold a Git call to test racing repository changes**](practice-hold-a-git-call-to-test-racing-repository-changes.md) to learn about: GitGate holds one git subcommand on one path so a write lands mid-inspection; keep a race test only if it fails without its guard. #testing #git #concurrency #mergetool

## Components (what exists)
- Open [**Conflict admission and offline startup**](map-conflict-admission-and-offline-startup.md) to learn about: ConflictClassifier admits editable merge/rebase files; startup preserves excluded reasons and existing manual edits. #git #classification #conversion #startup #scope
- Open [**Repository discovery and verified source identities**](map-repository-discovery-and-verified-source-identities.md) to learn about: Verify merge/rebase source identities and keep renamed text conflicts supported while isolating path provenance failures. #git #discovery #source-identity #scope

## By topic

### #git
- Open [**Mergetool launcher decisions and reloads**](mergetool/map-mergetool-launcher-decisions-and-reloads.md) — Launcher replies control parent staging; reloads reassign waiting paths and revalidate copied launch results.
- Open [**Mergetool socket lookup and access**](mergetool/map-mergetool-socket-lookup-and-access.md) — GUI and launcher share a short owner-only socket directory with a locked random fallback.
- Open [**Real Git parent compatibility and fixture isolation**](mergetool/map-real-git-parent-compatibility-and-fixture-isolation.md) — Real Git parent tests bound the compatibility claim and require short private runtime directories.
### #conversion
- Open [**Bound each converted representation independently**](practice-bound-each-converted-representation-independently.md) — Reject cleaned canonical objects over 4194304 bytes before reading; give each Git phase its own 4194304-byte budget.
- Open [**Conflict admission and offline startup**](map-conflict-admission-and-offline-startup.md) — ConflictClassifier admits editable merge/rebase files; startup preserves excluded reasons and existing manual edits.
### #failures
- Open [**Recovery acknowledgment and retained input**](../recovery/journal/map-recovery-acknowledgment-and-retained-input.md) — One bounded serial worker computes and persists commands; failures retain exact pending transactions for retry.
- Open [**Recovery journal format and validation**](../recovery/journal/map-recovery-journal-format-and-validation.md) — Version 3 frames and complete domain checkpoints preserve retained history and reject invalid recovery.
- Open [**Recovery of copied launches and interrupted publication**](../recovery/journal/map-recovery-of-copied-launches-and-interrupted-publication.md) — Copied launch destinations and interrupted mutations require current evidence and durable observations.
### #mergetool
- Open [**Mergetool launcher decisions and reloads**](mergetool/map-mergetool-launcher-decisions-and-reloads.md) — Launcher replies control parent staging; reloads reassign waiting paths and revalidate copied launch results.
- Open [**Mergetool socket lookup and access**](mergetool/map-mergetool-socket-lookup-and-access.md) — GUI and launcher share a short owner-only socket directory with a locked random fallback.
- Open [**Real Git parent compatibility and fixture isolation**](mergetool/map-real-git-parent-compatibility-and-fixture-isolation.md) — Real Git parent tests bound the compatibility claim and require short private runtime directories.
### #scope
- Open [**Branch Truce first-release scope**](../product/map-branch-truce-first-release-scope.md) — PRD.md defines a standalone, offline Linux app for text conflicts during merges and rebases.
- Open [**Keep unsupported and unfinished work visible**](../product/practice-keep-unsupported-and-unfinished-work-visible.md) — Show all current files, prominent outside-app warnings, and an honest handoff that directs users to outstanding work.
- Open [**Repository discovery and verified source identities**](map-repository-discovery-and-verified-source-identities.md) — Verify merge/rebase source identities and keep renamed text conflicts supported while isolating path provenance failures.
### #classification
- Open [**Conflict admission and offline startup**](map-conflict-admission-and-offline-startup.md) — ConflictClassifier admits editable merge/rebase files; startup preserves excluded reasons and existing manual edits.
### #concurrency
- Open [**Reconcile uncertain destination publication**](../recovery/practice-reconcile-uncertain-destination-publication.md) — Use captured destination identities, fail closed on unsupported metadata, and inspect uncertain exchanges before retrying.
- Open [**Hold a Git call to test racing repository changes**](practice-hold-a-git-call-to-test-racing-repository-changes.md) — GitGate holds one git subcommand on one path so a write lands mid-inspection; keep a race test only if it fails without its guard.
- Open [**Fence session work and serialize worktree mutations**](../architecture/practice-fence-session-work-and-serialize-worktree-mutations.md) — Fence background completions by session and revision; serialize worktree mutations through cancellation reconciliation.
### #discovery
- Open [**Repository discovery and verified source identities**](map-repository-discovery-and-verified-source-identities.md) — Verify merge/rebase source identities and keep renamed text conflicts supported while isolating path provenance failures.
### #limits
- Open [**Bound each converted representation independently**](practice-bound-each-converted-representation-independently.md) — Reject cleaned canonical objects over 4194304 bytes before reading; give each Git phase its own 4194304-byte budget.
### #publication
- Open [**Treat mergetool file state as live evidence**](practice-treat-mergetool-file-state-as-live-evidence.md) — Check live MERGED bytes before trusting a recorded write, shrink restores after flushing, and wait for the socket on a wall-clock deadline.
- Open [**Reconcile uncertain destination publication**](../recovery/practice-reconcile-uncertain-destination-publication.md) — Use captured destination identities, fail closed on unsupported metadata, and inspect uncertain exchanges before retrying.
### #source-identity
- Open [**Keep session captures immutable across revisions**](../core/practice-keep-session-captures-immutable-across-revisions.md) — Retain source bytes, conversion policy, provenance and the launch result separately from editable revision history.
- Open [**Repository discovery and verified source identities**](map-repository-discovery-and-verified-source-identities.md) — Verify merge/rebase source identities and keep renamed text conflicts supported while isolating path provenance failures.
### #startup
- Open [**Startup style, colour scheme and icon theme**](../ui/map-startup-style-colour-scheme-and-icon-theme.md) — main.cpp picks org.kde.desktop or Fusion, names hicolor when no icon theme is set; kdeglobals sets colours; a preloaded probe tests it.
- Open [**Conflict admission and offline startup**](map-conflict-admission-and-offline-startup.md) — ConflictClassifier admits editable merge/rebase files; startup preserves excluded reasons and existing manual edits.
### #testing
- Open [**Wait for layout before clicking offscreen dialog items**](../ui/practice-wait-for-layout-before-clicking-offscreen-dialog-items.md) — In offscreen UI tests, find popup delegates through the visual item tree and click only after their scene position settles.
- Open [**Wait for rendered frames before capturing native scenes**](../ui/practice-wait-for-rendered-frames-before-capturing-native-scenes.md) — Capture native replay after publication, layout and rendered frames settle while preserving real controls and focus.
- Open [**Journey acceptance suite**](../ui/map-journey-acceptance-suite.md) — CTest acceptance groups drive Main offscreen over real Git merges and rebases with private XDG directories and an offline Git guard.