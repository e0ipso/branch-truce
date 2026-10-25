---
type: practice
title: Treat mergetool file state as live evidence
description: >-
  Check live MERGED bytes before trusting a recorded write, shrink restores
  after flushing, and wait for the socket on a wall-clock deadline.
tags:
  - mergetool
  - publication
  - git
  - failures
kk_schema_version: 3
kk_id: practice-treat-mergetool-file-state-as-live-evidence
kk_derived_from: []
kk_relates_to:
  - practice-reconcile-uncertain-destination-publication
  - practice-recover-unfinished-work-without-changing-its-status
  - practice-keep-failed-work-outstanding
kk_depends_on: []
kk_confidence: high
---
Git's mergetool parent moves its backup back over `MERGED` whenever the launcher exits non-zero, for example after a lost `resolved` reply. So a recorded successful write says nothing about the file on disk. `ResolutionPublisher::indexed()` in [ResolutionPublisher.cpp](../../../src/app/ResolutionPublisher.cpp) recaptures the destination before it skips publication of an already-written revision, and skips only while the live bytes still equal that revision. On a parent-owned path, live bytes equal to the baseline or to the captured launch bytes mean Git restored its backup, so Mark resolved writes again. Any other bytes fail without staging. In repository mode any mismatch fails with "The repository file no longer matches the written revision." before `git add`, and the external edit stays on disk. [ResolutionPublisherTests.cpp](../../../tests/app/ResolutionPublisherTests.cpp) covers both modes.

`restoreMergetoolFile` in [MergetoolProtocol.cpp](../../../src/app/MergetoolProtocol.cpp) opens an existing descriptor, and Qt ignores `QIODevice::Truncate` for that kind of open. The function writes and flushes the launch bytes first, then resizes the file to exactly their length. A failed write, such as one that hits `RLIMIT_FSIZE`, keeps the longer file instead of truncating it before the write.

`QLocalSocket::waitForConnected` returns at once when no server exists, so a counted retry loop gives up within milliseconds. After it starts the GUI, [MergetoolLauncher.cpp](../../../src/app/MergetoolLauncher.cpp) spends its 15000 ms listen budget as wall-clock time in 200 ms slices. [MergetoolLauncherTests.cpp](../../../tests/app/MergetoolLauncherTests.cpp) covers a late listener, a session that never listens, and both restore cases.

<!-- kk:related:start -->
# Related

- Related: [practice-reconcile-uncertain-destination-publication](practice-reconcile-uncertain-destination-publication.md)
- Related: [practice-recover-unfinished-work-without-changing-its-status](practice-recover-unfinished-work-without-changing-its-status.md)
- Related: [practice-keep-failed-work-outstanding](practice-keep-failed-work-outstanding.md)
<!-- kk:related:end -->
