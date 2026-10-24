---
schema_version: 3
nodes_hash: 'sha256:25aa84081011ecb7cd349cd0f13d49042bc5964332139ab04feb8700f21b7271'
node_count: 19
---
# kenkeep Graph

Total nodes: 19

## map-branch-truce-desktop-implementation-baseline

- **kind:** map
- **title:** Branch Truce desktop implementation baseline
- **path:** map-branch-truce-desktop-implementation-baseline.md
- **tags:** architecture, desktop, platform, cpp
- **relates_to:** practice-design-desktop-apps-around-screens-and-components, map-branch-truce-first-release-scope
- **derived_from:** TECH_SPEC.md, src/ui/ComparisonView.qml, src/ui/Main.qml, src/app/ApplicationController.cpp, docs/editor-integration.md, docs/build.md, tests/fixtures/performance/evidence.md

## map-branch-truce-first-release-scope

- **kind:** map
- **title:** Branch Truce first-release scope
- **path:** map-branch-truce-first-release-scope.md
- **tags:** product, scope, workflow
- **derived_from:** PRD.md

## map-conflict-admission-and-offline-startup

- **kind:** map
- **title:** Conflict admission and offline startup
- **path:** map-conflict-admission-and-offline-startup.md
- **tags:** git, classification, conversion, startup, scope
- **relates_to:** map-repository-discovery-and-verified-source-identities, practice-keep-unsupported-and-unfinished-work-visible, practice-keep-sources-identifiable-and-the-result-editable, practice-bound-each-converted-representation-independently

## map-recovery-journal-and-acknowledgment-boundary

- **kind:** map
- **title:** Recovery journal and acknowledgment boundary
- **path:** map-recovery-journal-and-acknowledgment-boundary.md
- **tags:** recovery, storage, editing, failures
- **relates_to:** practice-recover-unfinished-work-without-changing-its-status, practice-keep-session-captures-immutable-across-revisions, practice-keep-failed-work-outstanding
- **derived_from:** docs/recovery-journal.md, src/app/RecoveryRestore.cpp, src/app/SessionModel.h, tests/fixtures/performance/evidence.md

## map-repository-discovery-and-verified-source-identities

- **kind:** map
- **title:** Repository discovery and verified source identities
- **path:** map-repository-discovery-and-verified-source-identities.md
- **tags:** git, discovery, source-identity, scope
- **relates_to:** practice-keep-sources-identifiable-and-the-result-editable, practice-keep-unsupported-and-unfinished-work-visible, map-branch-truce-first-release-scope

## map-revision-bound-passage-correspondence

- **kind:** map
- **title:** Revision-bound passage correspondence
- **path:** map-revision-bound-passage-correspondence.md
- **tags:** comparison, editing, revisions, failures
- **relates_to:** practice-keep-session-captures-immutable-across-revisions, practice-make-conflict-choices-explicit-and-reversible, practice-keep-failed-work-outstanding

## practice-bound-each-converted-representation-independently

- **kind:** practice
- **title:** Bound each converted representation independently
- **path:** practice-bound-each-converted-representation-independently.md
- **tags:** git, conversion, limits, failures
- **relates_to:** practice-keep-failed-work-outstanding

## practice-design-desktop-apps-around-screens-and-components

- **kind:** practice
- **title:** Design desktop apps around screens and components
- **path:** practice-design-desktop-apps-around-screens-and-components.md
- **tags:** architecture, desktop, techspec
- **relates_to:** map-branch-truce-first-release-scope, practice-keep-techspecs-and-prds-independent

## practice-fence-session-work-and-serialize-worktree-mutations

- **kind:** practice
- **title:** Fence session work and serialize worktree mutations
- **path:** practice-fence-session-work-and-serialize-worktree-mutations.md
- **tags:** architecture, concurrency, cancellation, git, desktop
- **relates_to:** map-branch-truce-desktop-implementation-baseline, practice-keep-failed-work-outstanding

## practice-keep-failed-work-outstanding

- **kind:** practice
- **title:** Keep failed work outstanding
- **path:** practice-keep-failed-work-outstanding.md
- **tags:** product, failures, saving, resolution
- **derived_from:** PRD.md, src/app/SessionModel.cpp, src/app/ApplicationController.cpp

## practice-keep-prd-focused-on-user-needs

- **kind:** practice
- **title:** Keep PRD.md focused on user needs
- **path:** practice-keep-prd-focused-on-user-needs.md
- **tags:** product, prd, documentation

## practice-keep-session-captures-immutable-across-revisions

- **kind:** practice
- **title:** Keep session captures immutable across revisions
- **path:** practice-keep-session-captures-immutable-across-revisions.md
- **tags:** session, snapshots, editing, source-identity
- **relates_to:** practice-keep-sources-identifiable-and-the-result-editable, practice-fence-session-work-and-serialize-worktree-mutations, map-conflict-admission-and-offline-startup

## practice-keep-sources-identifiable-and-the-result-editable

- **kind:** practice
- **title:** Keep sources identifiable and the result editable
- **path:** practice-keep-sources-identifiable-and-the-result-editable.md
- **tags:** product, comparison, editing
- **derived_from:** PRD.md, src/ui/ComparisonView.qml, src/ui/Main.qml, src/app/ApplicationController.cpp, docs/editor-integration.md

## practice-keep-techspecs-and-prds-independent

- **kind:** practice
- **title:** Keep TechSpecs and PRDs independent
- **path:** practice-keep-techspecs-and-prds-independent.md
- **tags:** documentation, techspec, prd
- **relates_to:** practice-keep-prd-focused-on-user-needs

## practice-keep-unsupported-and-unfinished-work-visible

- **kind:** practice
- **title:** Keep unsupported and unfinished work visible
- **path:** practice-keep-unsupported-and-unfinished-work-visible.md
- **tags:** product, scope, progress, handoff
- **derived_from:** PRD.md, src/app/SessionModel.cpp, src/app/ApplicationController.cpp, src/ui/Main.qml

## practice-make-conflict-choices-explicit-and-reversible

- **kind:** practice
- **title:** Make conflict choices explicit and reversible
- **path:** practice-make-conflict-choices-explicit-and-reversible.md
- **tags:** product, choices, editing, undo
- **derived_from:** PRD.md, src/core/EditingCommands.cpp, src/core/EditingCommands.md, src/core/SessionState.cpp, docs/recovery-journal.md

## practice-reconcile-uncertain-destination-publication

- **kind:** practice
- **title:** Reconcile uncertain destination publication
- **path:** practice-reconcile-uncertain-destination-publication.md
- **tags:** publication, storage, concurrency, git
- **relates_to:** practice-separate-save-from-mark-resolved, practice-fence-session-work-and-serialize-worktree-mutations, practice-keep-failed-work-outstanding

## practice-recover-unfinished-work-without-changing-its-status

- **kind:** practice
- **title:** Recover unfinished work without changing its status
- **path:** practice-recover-unfinished-work-without-changing-its-status.md
- **tags:** product, recovery, editing, progress
- **derived_from:** PRD.md, docs/recovery-journal.md, src/app/RecoveryRestore.cpp

## practice-separate-save-from-mark-resolved

- **kind:** practice
- **title:** Separate Save from Mark resolved
- **path:** practice-separate-save-from-mark-resolved.md
- **tags:** product, saving, resolution, review
- **derived_from:** PRD.md
