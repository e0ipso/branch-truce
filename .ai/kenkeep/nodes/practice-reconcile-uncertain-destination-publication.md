---
type: practice
title: Reconcile uncertain destination publication
description: >-
  Use captured destination identities, fail closed on unsupported metadata, and
  inspect uncertain exchanges before retrying.
tags:
  - publication
  - storage
  - concurrency
  - git
kk_schema_version: 3
kk_id: practice-reconcile-uncertain-destination-publication
kk_derived_from: []
kk_relates_to:
  - practice-separate-save-from-mark-resolved
  - practice-fence-session-work-and-serialize-worktree-mutations
  - practice-keep-failed-work-outstanding
kk_depends_on: []
kk_confidence: high
---
Capture the destination through [DestinationWriter.h](../../../src/storage/DestinationWriter.h) at launch or after verified publication, before editing. Keep that snapshot across failed retries. Recapturing a stale destination without review accepts an external edit. The caller owns draft persistence, fresh operation/index/conversion guards, the worktree mutation queue, and Git staging.

The writer opens exact native filename bytes without following symlinks and requires an existing regular file. It preserves owner, group, and basic permission bits. Foreign ownership, multiple hard links, special permission bits, ACLs, and extended attributes fail closed. A complete private replacement and descriptor-relative exchange protect observed path and content identities; they cannot make filesystem publication atomic with Git.

A late exchange race or an ambiguous exchange reply reports `Uncertain` and retains the exchange slot for inspection. Never roll back automatically: a compensating rename could overwrite another external edit. Treat `retainedPath` as a location hint, especially if an ancestor has moved. Reconcile the filesystem and retained entry before establishing a new baseline.

`Published` means the writer has observed and synced the new destination. Cleanup failure still blocks staging and overall success. Only a failure-free publication yields the next baseline. The [publication contract](../../../docs/destination-publication.md) and [fault/race coverage](../../../tests/storage/DestinationWriterTests.cpp) describe these limits.

<!-- kk:related:start -->
# Related

- Related: [practice-separate-save-from-mark-resolved](practice-separate-save-from-mark-resolved.md)
- Related: [practice-fence-session-work-and-serialize-worktree-mutations](practice-fence-session-work-and-serialize-worktree-mutations.md)
- Related: [practice-keep-failed-work-outstanding](practice-keep-failed-work-outstanding.md)
<!-- kk:related:end -->
