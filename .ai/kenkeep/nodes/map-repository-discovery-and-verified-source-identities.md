---
type: map
title: Repository discovery and verified source identities
description: >-
  Verify merge/rebase source identities and keep renamed text conflicts
  supported while isolating path provenance failures.
tags:
  - git
  - discovery
  - source-identity
  - scope
kk_schema_version: 3
kk_id: map-repository-discovery-and-verified-source-identities
kk_derived_from: []
kk_relates_to:
  - practice-keep-sources-identifiable-and-the-result-editable
  - practice-keep-unsupported-and-unfinished-work-visible
  - map-branch-truce-first-release-scope
kk_depends_on: []
kk_confidence: high
---
`Operation::DiscoverRepository` returns structured `Result.repository` and exact unmerged `Result.indexEntries` through the constrained Git adapter. [`RepositoryDiscovery`](../../../src/git/RepositoryDiscovery.cpp), declared in [`RepositoryDiscovery.h`](../../../src/git/RepositoryDiscovery.h), verifies the worktree root and canonical per-worktree Git directory, including linked worktrees. Consumers use the structured paths and [`RepositoryContext`](../../../src/git/GitAdapter.h); `standardOutput` remains available for display. The `supported` flag confirms operation identity only. File type, conversion policy and fresh mutation checks still determine whether a path can be edited or published.

For a merge, stage 2 is the verified HEAD, labelled "Current checkout", and stage 3 is MERGE_HEAD, labelled "Merged commit". For a rebase, stage 2 is the current HEAD, labelled "Current rebased state", and stage 3 is the verified replayed commit, labelled "Commit being replayed". Rebase `headName`, `originalHead` and `onto` provide context rather than side identities. The earlier source is the unique merge base for a merge or the replayed commit's parent for a rebase; it can be unavailable. Source labels follow verified identities, and stage sides differ between merge and rebase.

Discovery rejects inconsistent operation identities, multiple merge bases and replayed merge commits with explicit diagnostics. Cherry-pick, revert, sequencer and apply-mailbox operations remain outside the supported merge/rebase scope. Discovery rechecks the unmerged index, HEAD and operation metadata before returning a supported snapshot. A changed snapshot requires inspection again.

Ordinary text stages at renamed paths remain supportable under [PRD.md](../../../PRD.md) and [TECH_SPEC.md](../../../TECH_SPEC.md). If a source commit lacks the destination path, discovery can verify a regular-file stage's exact blob and mode elsewhere in that commit without inferring rename history. An unverified stage or ambiguous path topology puts only that path in `RepositoryContext.unsupportedPaths`, keyed by exact filename bytes, with a reason. Its index entries remain visible, and other supported paths can proceed. Resolving rename destinations or other topology disputes remains outside the app.

<!-- kk:related:start -->
# Related

- Related: [practice-keep-sources-identifiable-and-the-result-editable](practice-keep-sources-identifiable-and-the-result-editable.md)
- Related: [practice-keep-unsupported-and-unfinished-work-visible](practice-keep-unsupported-and-unfinished-work-visible.md)
- Related: [map-branch-truce-first-release-scope](map-branch-truce-first-release-scope.md)
<!-- kk:related:end -->
