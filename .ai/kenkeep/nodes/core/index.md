# kenkeep Index: core

↑ Parent: [kenkeep](../index.md)

> kenkeep navigation: the injected body above is the root index node, the top-level catalog of branches and root-level leaves. Do not expect the whole knowledge base here; descend on demand. Read the root index node, pick one or more branches whose intent and tags match your task (several branches can be relevant), and read those branch `index.md` nodes. Descend further only where the task needs it, opening only the leaves you have confirmed are relevant. Follow each leaf's `relates_to` and `depends_on` cross edges to reach related leaves in other branches. You decide how deep to go per branch.

> This index only orients you; leaves hold the durable guidance. Open at least one relevant leaf before acting.

## Subfolders
_None._

## Conventions (how we build)
- Open [**Keep session captures immutable across revisions**](practice-keep-session-captures-immutable-across-revisions.md) to learn about: Retain source bytes, conversion policy, provenance and the launch result separately from editable revision history. #session #snapshots #editing #source-identity

## Components (what exists)
- Open [**Revision-bound passage correspondence**](map-revision-bound-passage-correspondence.md) to learn about: Map immutable source ranges to one result revision; reject uncertain targets and keep alignment memory linear. #comparison #editing #revisions #failures
- Open [**Conflict marker scan**](map-conflict-marker-scan.md) to learn about: FileSession derives marker counts from complete blocks and stray lines while preserving marker-like source text. #git #editing #markers #recovery
- Open [**Conflict marker sections**](map-conflict-marker-sections.md) to learn about: Marker sections tie intact Git conflict frames to choices and clean up fully decided blocks in the same undo entry. #git #editing #markers #undo #recovery

## By topic

### #editing
- Open [**Recovery acknowledgment and retained input**](../recovery/journal/map-recovery-acknowledgment-and-retained-input.md) — One bounded serial worker computes and persists commands; failures retain exact pending transactions for retry.
- Open [**Recovery journal format and validation**](../recovery/journal/map-recovery-journal-format-and-validation.md) — Version 3 frames and complete domain checkpoints preserve retained history and reject invalid recovery.
- Open [**Recovery of copied launches and interrupted publication**](../recovery/journal/map-recovery-of-copied-launches-and-interrupted-publication.md) — Copied launch destinations and interrupted mutations require current evidence and durable observations.
### #git
- Open [**Mergetool launcher decisions and reloads**](../git/mergetool/map-mergetool-launcher-decisions-and-reloads.md) — Launcher replies control parent staging; reloads reassign waiting paths and revalidate copied launch results.
- Open [**Mergetool socket lookup and access**](../git/mergetool/map-mergetool-socket-lookup-and-access.md) — GUI and launcher share a short owner-only socket directory with a locked random fallback.
- Open [**Real Git parent compatibility and fixture isolation**](../git/mergetool/map-real-git-parent-compatibility-and-fixture-isolation.md) — Real Git parent tests bound the compatibility claim and require short private runtime directories.
### #markers
- Open [**Conflict marker scan**](map-conflict-marker-scan.md) — FileSession derives marker counts from complete blocks and stray lines while preserving marker-like source text.
- Open [**Conflict marker sections**](map-conflict-marker-sections.md) — Marker sections tie intact Git conflict frames to choices and clean up fully decided blocks in the same undo entry.
- Open [**Result conflict-marker folding**](../ui/map-result-conflict-marker-folding.md) — TextBlocks folds recognized conflict content without changing result bytes, line numbers or application history.
### #recovery
- Open [**Recovery acknowledgment and retained input**](../recovery/journal/map-recovery-acknowledgment-and-retained-input.md) — One bounded serial worker computes and persists commands; failures retain exact pending transactions for retry.
- Open [**Recovery journal format and validation**](../recovery/journal/map-recovery-journal-format-and-validation.md) — Version 3 frames and complete domain checkpoints preserve retained history and reject invalid recovery.
- Open [**Recovery of copied launches and interrupted publication**](../recovery/journal/map-recovery-of-copied-launches-and-interrupted-publication.md) — Copied launch destinations and interrupted mutations require current evidence and durable observations.
### #comparison
- Open [**Revision-bound source control presentation**](../ui/map-revision-bound-source-control-presentation.md) — Stable source-row models publish complete comparison snapshots and update command fields without recreating unaffected controls.
- Open [**Revision-bound passage correspondence**](map-revision-bound-passage-correspondence.md) — Map immutable source ranges to one result revision; reject uncertain targets and keep alignment memory linear.
- Open [**Result conflict-marker folding**](../ui/map-result-conflict-marker-folding.md) — TextBlocks folds recognized conflict content without changing result bytes, line numbers or application history.
### #failures
- Open [**Recovery acknowledgment and retained input**](../recovery/journal/map-recovery-acknowledgment-and-retained-input.md) — One bounded serial worker computes and persists commands; failures retain exact pending transactions for retry.
- Open [**Recovery journal format and validation**](../recovery/journal/map-recovery-journal-format-and-validation.md) — Version 3 frames and complete domain checkpoints preserve retained history and reject invalid recovery.
- Open [**Recovery of copied launches and interrupted publication**](../recovery/journal/map-recovery-of-copied-launches-and-interrupted-publication.md) — Copied launch destinations and interrupted mutations require current evidence and durable observations.
### #revisions
- Open [**Revision-bound passage correspondence**](map-revision-bound-passage-correspondence.md) — Map immutable source ranges to one result revision; reject uncertain targets and keep alignment memory linear.
- Open [**Revision-bound source control presentation**](../ui/map-revision-bound-source-control-presentation.md) — Stable source-row models publish complete comparison snapshots and update command fields without recreating unaffected controls.
### #session
- Open [**Keep session captures immutable across revisions**](practice-keep-session-captures-immutable-across-revisions.md) — Retain source bytes, conversion policy, provenance and the launch result separately from editable revision history.
### #snapshots
- Open [**Keep session captures immutable across revisions**](practice-keep-session-captures-immutable-across-revisions.md) — Retain source bytes, conversion policy, provenance and the launch result separately from editable revision history.
### #source-identity
- Open [**Keep session captures immutable across revisions**](practice-keep-session-captures-immutable-across-revisions.md) — Retain source bytes, conversion policy, provenance and the launch result separately from editable revision history.
- Open [**Repository discovery and verified source identities**](../git/map-repository-discovery-and-verified-source-identities.md) — Verify merge/rebase source identities and keep renamed text conflicts supported while isolating path provenance failures.
### #undo
- Open [**Make conflict choices explicit and reversible**](../product/practice-make-conflict-choices-explicit-and-reversible.md) — Keep accept, ignore, combine, and broader choices distinct, inspectable, and reversible with their decision states.
- Open [**Conflict marker sections**](map-conflict-marker-sections.md) — Marker sections tie intact Git conflict frames to choices and clean up fully decided blocks in the same undo entry.