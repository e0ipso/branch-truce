---
schema_version: 3
nodes_hash: 'sha256:9e1cfd85a00c70a4f81da23676b4801b1cba7a55e519dd504a74ee9ee1d341bd'
node_count: 20
---
# kenkeep

> kenkeep navigation: the injected body above is the root index node, the top-level catalog of branches and root-level leaves. Do not expect the whole knowledge base here; descend on demand. Read the root index node, pick one or more branches whose intent and tags match your task (several branches can be relevant), and read those branch `index.md` nodes. Descend further only where the task needs it, opening only the leaves you have confirmed are relevant. Follow each leaf's `relates_to` and `depends_on` cross edges to reach related leaves in other branches. You decide how deep to go per branch.

## Branches
_None._

## Conventions (how we build)
- Open [**Keep failed work outstanding**](nodes/practice-keep-failed-work-outstanding.md) to learn about: Keep failed saves and resolutions visible, retain edited results, and claim resolution only after both steps succeed. #product #failures #saving #resolution
- Open [**Keep sources identifiable and the result editable**](nodes/practice-keep-sources-identifiable-and-the-result-editable.md) to learn about: Require truthful source identities, an editable center result, earlier context, and reading aids that preserve text. #product #comparison #editing
- Open [**Fence session work and serialize worktree mutations**](nodes/practice-fence-session-work-and-serialize-worktree-mutations.md) to learn about: Fence background completions by session and revision; serialize worktree mutations through cancellation reconciliation. #architecture #concurrency #cancellation #git #desktop
- Open [**Keep session captures immutable across revisions**](nodes/practice-keep-session-captures-immutable-across-revisions.md) to learn about: Retain source bytes, conversion policy, provenance and the launch result separately from editable revision history. #session #snapshots #editing #source-identity
- Open [**Keep unsupported and unfinished work visible**](nodes/practice-keep-unsupported-and-unfinished-work-visible.md) to learn about: Show all current files, prominent outside-app warnings, and an honest handoff that directs users to outstanding work. #product #scope #progress #handoff
- Open [**Recover unfinished work without changing its status**](nodes/practice-recover-unfinished-work-without-changing-its-status.md) to learn about: Restore unsaved edits and outstanding decisions after closing or a crash while preserving honest work status. #product #recovery #editing #progress
- Open [**Bound each converted representation independently**](nodes/practice-bound-each-converted-representation-independently.md) to learn about: Reject cleaned canonical objects over 4194304 bytes before reading; give each Git phase its own 4194304-byte budget. #git #conversion #limits #failures
- Open [**Design desktop apps around screens and components**](nodes/practice-design-desktop-apps-around-screens-and-components.md) to learn about: Define screens and components, then build loosely coupled implementations using each desktop environment's idiomatic UI kit. #architecture #desktop #techspec
- Open [**Keep PRD.md focused on user needs**](nodes/practice-keep-prd-focused-on-user-needs.md) to learn about: PRD.md describes user needs, interactions, and capabilities without technical or implementation details. #product #prd #documentation
- Open [**Keep TechSpecs and PRDs independent**](nodes/practice-keep-techspecs-and-prds-independent.md) to learn about: TechSpecs explain how to build; PRDs explain what to build and why. Keep both separate and independently understandable. #documentation #techspec #prd
- Open [**Make conflict choices explicit and reversible**](nodes/practice-make-conflict-choices-explicit-and-reversible.md) to learn about: Keep accept, ignore, combine, and broader choices distinct, inspectable, and reversible with their decision states. #product #choices #editing #undo
- Open [**Reconcile uncertain destination publication**](nodes/practice-reconcile-uncertain-destination-publication.md) to learn about: Use captured destination identities, fail closed on unsupported metadata, and inspect uncertain exchanges before retrying. #publication #storage #concurrency #git
- Open [**Separate Save from Mark resolved**](nodes/practice-separate-save-from-mark-resolved.md) to learn about: Save keeps app-only drafts; Mark resolved writes the reviewed result and resolves the file in Git. #product #saving #resolution #review
- Open [**Treat mergetool file state as live evidence**](nodes/practice-treat-mergetool-file-state-as-live-evidence.md) to learn about: Check live MERGED bytes before trusting a recorded write, shrink restores after flushing, and wait for the socket on a wall-clock deadline. #mergetool #publication #git #failures

## Components (what exists)
- Open [**Branch Truce first-release scope**](nodes/map-branch-truce-first-release-scope.md) to learn about: PRD.md defines a standalone, offline Linux app for text conflicts during merges and rebases. #product #scope #workflow
- Open [**Branch Truce desktop implementation baseline**](nodes/map-branch-truce-desktop-implementation-baseline.md) to learn about: Locked C++/Qt Quick/Kirigami stack for native Linux/KDE on Kubuntu 26.04 LTS; Tuxedo OS compatibility needs validation. #architecture #desktop #platform #cpp
- Open [**Conflict admission and offline startup**](nodes/map-conflict-admission-and-offline-startup.md) to learn about: ConflictClassifier admits editable merge/rebase files; startup preserves excluded reasons and existing manual edits. #git #classification #conversion #startup #scope
- Open [**Repository discovery and verified source identities**](nodes/map-repository-discovery-and-verified-source-identities.md) to learn about: Verify merge/rebase source identities and keep renamed text conflicts supported while isolating path provenance failures. #git #discovery #source-identity #scope
- Open [**Recovery journal and acknowledgment boundary**](nodes/map-recovery-journal-and-acknowledgment-boundary.md) to learn about: Persist complete text-and-decision transactions before acknowledgment; retain failed input and loaded captures for retry. #recovery #storage #editing #failures
- Open [**Revision-bound passage correspondence**](nodes/map-revision-bound-passage-correspondence.md) to learn about: Map immutable source ranges to one result revision; reject uncertain targets and keep alignment memory linear. #comparison #editing #revisions #failures
